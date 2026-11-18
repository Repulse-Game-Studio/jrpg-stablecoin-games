// Twin of scfg_core/local_server.py (CheckoutServer), route by route and limit by limit as its non-blocking twin
// godot/addons/scfg/local_server.gd (ScfgCheckoutServer), with rpgmaker/src/mz/local_server.ts's refinements: the
// localhost checkout server for desktop browser wallets.
//
// Browser extensions (MetaMask, Rabby, Phantom...) only talk to web pages, so the game serves the checkout page on
// 127.0.0.1; the page connects the wallet, sends the transfer and posts the tx hash back here.
//
// `new CheckoutServer(options)` takes local_server.CheckoutServer's keyword arguments (CheckoutServerOptions):
//   - LoadAsset(name) -> bytes: one of Assets (null, a throw or no callback is a 500);
//   - SessionInfo: the object served as session.json;
//   - OnTx(txHash, sender): the page's hint, already normalised (PageMessages.ParseTxMessage);
//   - GetStatus() -> object: served as status;
//   - LoadLogo() -> (bytes, content type) (optional; no route without it);
//   - BuildTx(account) -> Job (optional, Solana; no route without it);
//   - Host ("127.0.0.1", an IP address: no name is resolved);
//   - Clock (seconds, monotonic; default a Stopwatch).
// Callbacks are plain delegates, called inside Pump(). One that is missing or throws answers 503 (500 for an asset,
// 404 for the logo) and never escapes Pump(); a GetStatus or BuildTx that returns null is a 503 too. A callback may
// call Stop() (and Start() again): the connections of the stopped server are closed and get no answer.
//
// The game calls Pump() every frame; it never waits on a socket. Every socket is non-blocking: the listener accepts
// only when Poll(0) says a connection is waiting, a read takes only the bytes Available, and a write sends what the
// socket takes and carries the rest to the next Pump(). No thread, no timer, no *Async or Begin/End call: the deadlines
// run on the injected clock. Start() binds Host on a random free port and throws RuntimeError when it cannot; a second
// Start() while running does nothing, and a stopped server starts again on a new port with the same secret (as
// Godot's). Stop() closes everything and cancels a build a page still waits for. Pump() inside Pump() (from a
// callback) does nothing.
//
// Security (as local_server.py):
//   - bound to 127.0.0.1 on a random port;
//   - every URL holds a 128-bit secret, /c/<secret>/..., otherwise 404;
//   - the Host header must be exactly 127.0.0.1:<port> (defeats DNS rebinding), otherwise 403;
//   - no CORS headers; a request body is at most MaxBody bytes (413), with a valid Content-Length (400), read as
//     Python's int() reads it;
//   - every answer has Cache-Control: no-store, Referrer-Policy: no-referrer and X-Content-Type-Options: nosniff;
//   - /build returns the *unsigned* transaction only for a valid account, and gives up after BuildTimeoutS (503, the
//     job cancelled); a failed or cancelled job, or a result that is not a JsonObject, is a 503;
//   - a posted tx hash is only a hint: the watcher verifies it on-chain;
//   - the logo is served only as a raster image (the types Config.LogoTypes holds: png, jpeg, webp), never SVG.
// As the Godot server: a connection has DeadlineS to deliver its request (and again to take its answer), at most
// MaxConnections are open at once (the next ones wait in the listen backlog), and a request head is at most MaxHead
// bytes in all (431; Python caps only each line, at MaxLine, and the number of header lines).
//
// HTTP, as Python's BaseHTTPRequestHandler reads it: GET and POST only (anything else, HEAD included, is 501; HEAD
// gets no body); an empty request line is closed without an answer, a bad version is 400 and HTTP/2+ is 505; a line
// may end in LF or CRLF; the header block is read as email.parser (compat32) reads it (a bare CR inside a header line
// ends that header, the first of a repeated header counts, a folded value keeps its line ends); a leading `//`
// becomes `/`; the query string is ignored. Every answer is HTTP/1.1 with `Connection: close`, its JSON written by
// Json.Dumps (Python's json.dumps). Deviations (Godot's and Node's): a two-word (HTTP/0.9) request line is a 400, a bad
// request line gets a status line (http.server sends a bare error page), the 501's body is plain text, and a client
// that closes its side before its head is complete gets no answer.
//
// Closing: an answer to a request read in full, with nothing after it, is closed once written. Otherwise (a body
// never read, a head cut short, more bytes after the request) the server sends its FIN (Shutdown(Send)) and drains
// what the client still sends for up to LingerS or LingerMaxBytes, until the client closes: closing on unread input
// resets the connection, and the client could lose the answer. A socket is closed with Close(), never Close(timeout):
// a timeout makes .NET block in Close (and Close(0) resets the connection).
#nullable enable
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Runtime.CompilerServices;
using System.Text;
using Scfg.Core;

[assembly: InternalsVisibleTo("Scfg.Core.Tests")]

namespace Scfg.Desktop
{
    /// <summary>local_server.CheckoutServer's keyword arguments.</summary>
    public sealed class CheckoutServerOptions
    {
        /// <summary>The bytes of one of <see cref="CheckoutServer.Assets"/> (null, a throw or no callback: a 500).</summary>
        public Func<string, byte[]>? LoadAsset { get; set; }

        /// <summary>Served as session.json (default: an empty object).</summary>
        public JsonObject? SessionInfo { get; set; }

        /// <summary>The page's hint, already normalised: the tx hash and the sender or null.</summary>
        public Action<string, string?>? OnTx { get; set; }

        /// <summary>Served as status (null, a throw or no callback: a 503).</summary>
        public Func<JsonObject>? GetStatus { get; set; }

        /// <summary>The logo's bytes and content type (optional: no logo route without it). Only a raster type is served.</summary>
        public Func<(byte[] Body, string ContentType)>? LoadLogo { get; set; }

        /// <summary>Solana: the unsigned transaction for the player's account, as a Job (optional: no build route without it).</summary>
        public Func<string, Job>? BuildTx { get; set; }

        /// <summary>The address to bind, an IP address (no name is resolved: that would block).</summary>
        public string Host { get; set; } = "127.0.0.1";

        /// <summary>Seconds, monotonic (default: a Stopwatch). The deadlines run on it.</summary>
        public Func<double>? Clock { get; set; }
    }

    /// <summary>The localhost checkout page: Python's local_server.CheckoutServer, served from Pump().</summary>
    public sealed class CheckoutServer : ICheckoutServer
    {
        public const int MaxBody = 4096;

        public static readonly IReadOnlyList<string> Assets = Array.AsReadOnly(new[] { "index.html", "app.js", "evm.js", "solana.js" });

        /// <summary>Seconds a connection has to send its whole request, and again to take its answer.</summary>
        public const double DeadlineS = 10.0;

        /// <summary>
        /// One line of the head in bytes, with its line end (http.server's readline(65537)): longer is a 414 for the
        /// request line, a 431 for a header line. A browser may send 127.0.0.1 a Cookie this large.
        /// </summary>
        public const int MaxLine = 65536;

        /// <summary>The request line plus headers in all, in bytes; more is a 431 (Python has no total cap).</summary>
        public const int MaxHead = 262144;

        /// <summary>
        /// http.client's _MAXHEADERS. It counts the blank line that ends the head as one of them, so a request has at
        /// most MaxHeaders - 1 header lines; one more is a 431.
        /// </summary>
        public const int MaxHeaders = 100;

        public const int MaxConnections = 32;

        /// <summary>The listen backlog: connections the kernel holds for us while MaxConnections are open.</summary>
        public const int Backlog = 64;

        /// <summary>After answering a request not read in full: how long, and how much, of the rest is drained before closing.</summary>
        public const double LingerS = 1.0;

        public const int LingerMaxBytes = 65536;

        /// <summary>One Pump() offers the socket at most WriteChunks pieces of WriteChunk bytes (a frame stays short).</summary>
        public const int WriteChunk = 65536;

        public const int WriteChunks = 16;

        /// <summary>The content types the logo is served as: the ones Config accepts for a logo file.</summary>
        public static readonly IReadOnlyList<string> RasterTypes = Array.AsReadOnly(Config.LogoTypes.Values.Distinct().ToArray());

        private const string Text = "text/plain; charset=utf-8";

        internal enum ConnState { Head, Body, Building, Writing, Linger, Closed }

        private sealed class Conn
        {
            public readonly Socket Sock;
            public ConnState State = ConnState.Head;
            public double Deadline;
            public byte[] Buf = new byte[4096];
            public int Len;
            public int LineStart; // where the line being read starts in Buf
            public int Scanned; // where the search for that line's end goes on
            public readonly List<string> Lines = new List<string>(); // the request line, then the header lines (Latin-1)
            public string Method = "";
            public string Path = "";
            public string Sub = ""; // the path after /c/<secret>/
            public int Need; // body bytes to read
            public Job? Job;
            public byte[] Out = Array.Empty<byte>();
            public int Sent;
            public int Drained;
            public bool Complete; // the whole request was read (nothing more is expected from the client)

            public Conn(Socket sock, double deadline)
            {
                Sock = sock;
                Deadline = deadline;
            }

            public void Ensure(int size)
            {
                if (size <= Buf.Length) return;
                int n = Math.Max(Buf.Length, 1024);
                while (n < size) n *= 2;
                Array.Resize(ref Buf, n);
            }
        }

        private readonly Func<string, byte[]>? _loadAsset;
        private readonly JsonObject _sessionInfo;
        private readonly Action<string, string?>? _onTx;
        private readonly Func<JsonObject>? _getStatus;
        private readonly Func<(byte[] Body, string ContentType)>? _loadLogo;
        private readonly Func<string, Job>? _buildTx;
        private readonly Func<double> _clock;
        private readonly byte[] _scratch = new byte[LingerMaxBytes];
        private Socket? _listener;
        private int _port;
        private List<Conn> _conns = new List<Conn>();
        private bool _pumping;

        public CheckoutServer(CheckoutServerOptions options)
        {
            if (options == null) throw new TypeError("CheckoutServer needs its options");
            _loadAsset = options.LoadAsset;
            _sessionInfo = options.SessionInfo ?? new JsonObject();
            _onTx = options.OnTx;
            _getStatus = options.GetStatus;
            _loadLogo = options.LoadLogo;
            _buildTx = options.BuildTx;
            Host = options.Host ?? "127.0.0.1";
            _clock = options.Clock ?? Monotonic;
            var bytes = new byte[16];
            SystemEntropy.Fill(bytes);
            Secret = Base64Url(bytes);
        }

        public string Host { get; }

        /// <summary>16 random bytes, base64url without padding: Python's secrets.token_urlsafe(16).</summary>
        public string Secret { get; }

        /// <summary>How long /build waits for its job. Python's module constant BUILD_TIMEOUT_S; tests lower it.</summary>
        public double BuildTimeoutS { get; set; } = PageMessages.BuildTimeoutS;

        /// <summary>The port bound by the last Start() (0 before).</summary>
        public int Port => _port;

        public string Url => "http://" + Host + ":" + _port.ToString(CultureInfo.InvariantCulture) + "/c/" + Secret + "/";

        public bool Running => _listener != null;

        /// <summary>Binds <see cref="Host"/> on a random free port; a RuntimeError when it cannot. A second Start() while running does nothing.</summary>
        public void Start()
        {
            if (_listener != null) return;
            if (!IPAddress.TryParse(Host, out var address))
                throw new RuntimeError("cannot listen on " + Host + ": not an IP address");
            Socket? socket = null;
            try
            {
                socket = new Socket(address.AddressFamily, SocketType.Stream, ProtocolType.Tcp);
                socket.Blocking = false;
                socket.Bind(new IPEndPoint(address, 0));
                socket.Listen(Backlog);
                var bound = socket.LocalEndPoint as IPEndPoint;
                if (bound == null) throw new SocketException((int)SocketError.AddressNotAvailable);
                _port = bound.Port;
            }
            catch (Exception e) when (e is SocketException || e is ObjectDisposedException || e is NotSupportedException)
            {
                Abort(socket);
                throw new RuntimeError("cannot listen on " + Host + ": " + e.Message);
            }
            _listener = socket;
        }

        /// <summary>Closes the listener and every connection, and cancels a build a page still waits for. Safe to call twice.</summary>
        public void Stop()
        {
            var listener = _listener;
            if (listener == null) return;
            _listener = null;
            Abort(listener);
            var conns = _conns;
            _conns = new List<Conn>();
            foreach (var c in conns) Close(c); // (Close cancels a pending build: the page is gone)
        }

        /// <summary>The page lives in the player's browser: there is nothing to show again.</summary>
        public void Reopen()
        {
        }

        /// <summary>Accept, read, answer and close, without waiting on any socket. Call it every frame.</summary>
        public void Pump()
        {
            if (_listener == null || _pumping) return;
            _pumping = true;
            try
            {
                Accept();
                foreach (var c in _conns.ToArray())
                {
                    if (_listener == null) return; // a callback stopped the server: Stop() closed everything
                    Step(c);
                }
                _conns.RemoveAll(c => c.State == ConnState.Closed);
            }
            finally
            {
                _pumping = false;
            }
        }

        // ---- test hooks -------------------------------------------------------------------------------------

        internal int ConnectionCount => _conns.Count;

        /// <summary>The connections accepted since the server was made.</summary>
        internal int AcceptedCount { get; private set; }

        internal IReadOnlyList<ConnState> ConnectionStates() => _conns.Select(c => c.State).ToList();

        /// <summary>True when the listener and every open connection are non-blocking sockets.</summary>
        internal bool NonBlocking() => (_listener == null || !_listener.Blocking) && _conns.All(c => c.State == ConnState.Closed || !c.Sock.Blocking);

        // ---- connections ------------------------------------------------------------------------------------

        private static double Monotonic() => Stopwatch.GetTimestamp() / (double)Stopwatch.Frequency;

        private double Now() => _clock();

        private void Accept()
        {
            while (_listener != null && _conns.Count < MaxConnections)
            {
                Socket peer;
                try
                {
                    if (!_listener.Poll(0, SelectMode.SelectRead)) return;
                    peer = _listener.Accept();
                }
                catch (Exception e) when (e is SocketException || e is ObjectDisposedException || e is InvalidOperationException)
                {
                    return; // nothing waiting after all (WouldBlock), a handshake reset before it was taken, no descriptor left: next pump
                }
                double deadline;
                try
                {
                    peer.Blocking = false;
                    peer.NoDelay = true;
                    deadline = Now() + DeadlineS;
                }
                catch (Exception)
                {
                    Abort(peer); // a socket that failed at once (or a clock that throws): never out of Pump()
                    continue;
                }
                _conns.Add(new Conn(peer, deadline));
                AcceptedCount++;
            }
        }

        private void Step(Conn c)
        {
            try
            {
                switch (c.State)
                {
                    case ConnState.Head:
                    case ConnState.Body:
                        if (Now() >= c.Deadline)
                        {
                            Close(c);
                            return;
                        }
                        if (!Read(c)) return;
                        if (c.State == ConnState.Head) ReadHead(c);
                        if (c.State == ConnState.Body && c.Len >= c.Need) OnBody(c);
                        return;
                    case ConnState.Building:
                        CheckBuild(c);
                        return;
                    case ConnState.Writing:
                        Write(c);
                        return;
                    case ConnState.Linger:
                        Linger(c);
                        return;
                }
            }
            catch (Exception)
            {
                Close(c); // a socket that failed (reset, closed under us), or anything else: never out of Pump()
            }
        }

        /// <summary>
        /// The bytes the socket has now, up to what this state may hold. False if the connection closed (the client
        /// closed its side before its request was complete, or it broke).
        /// </summary>
        private bool Read(Conn c)
        {
            int avail = c.Sock.Available;
            if (avail == 0)
            {
                if (!c.Sock.Poll(0, SelectMode.SelectRead)) return true; // nothing yet
                avail = c.Sock.Available;
                if (avail == 0)
                {
                    Close(c); // readable with nothing to read: the client closed or reset the connection
                    return false;
                }
            }
            int room = c.State == ConnState.Head ? MaxHead + 1 - c.Len : c.Need - c.Len;
            int n = Math.Min(avail, room);
            if (n <= 0) return true;
            c.Ensure(c.Len + n);
            int got = c.Sock.Receive(c.Buf, c.Len, n, SocketFlags.None, out SocketError error);
            if (error == SocketError.WouldBlock) return true;
            if (error != SocketError.Success || got <= 0)
            {
                Close(c);
                return false;
            }
            c.Len += got;
            return true;
        }

        /// <summary>
        /// Splits off the complete lines received so far: the request line, then headers up to the blank line. Only
        /// the first MaxHead + 1 bytes may hold the head. Answers a bad request line at once, as http.server does
        /// before reading the headers.
        /// </summary>
        private void ReadHead(Conn c)
        {
            while (c.State == ConnState.Head)
            {
                int limit = Math.Min(c.Len, MaxHead + 1);
                int nl = FindLf(c.Buf, c.Scanned, limit);
                c.Scanned = nl >= 0 ? nl + 1 : limit;
                int lineSize = (nl >= 0 ? nl + 1 : limit) - c.LineStart; // with its line end
                if (lineSize > MaxLine)
                {
                    if (c.Lines.Count == 0) Send(c, 414, "request line too long");
                    else Send(c, 431, "line too long");
                    return;
                }
                if (nl < 0)
                {
                    if (limit > MaxHead) Send(c, 431, "headers too large");
                    return;
                }
                int start = c.LineStart;
                c.LineStart = nl + 1;
                if (c.Lines.Count == 0)
                {
                    RequestLine(c, StripLineEnds(Latin1(c.Buf, start, nl)));
                }
                else if (c.Lines.Count > MaxHeaders)
                {
                    // MaxHeaders header lines are in: this line, a header or the blank end, is one more
                    Send(c, 431, "too many headers");
                }
                else if (nl == start || (nl == start + 1 && c.Buf[start] == 13))
                {
                    int rest = c.Len - c.LineStart;
                    Buffer.BlockCopy(c.Buf, c.LineStart, c.Buf, 0, rest);
                    c.Len = rest;
                    c.LineStart = 0;
                    c.Scanned = 0;
                    OnHead(c);
                }
                else
                {
                    c.Lines.Add(Latin1(c.Buf, start, nl));
                }
            }
        }

        /// <summary>BaseHTTPRequestHandler.parse_request's checks of the request line.</summary>
        private void RequestLine(Conn c, string line)
        {
            var words = PySplit(line);
            if (words.Count == 0)
            {
                Close(c); // http.server closes without an answer
                return;
            }
            if (words.Count >= 3)
            {
                var version = words[words.Count - 1];
                var numbers = version.StartsWith("HTTP/", StringComparison.Ordinal) ? version.Substring(5).Split('.') : Array.Empty<string>();
                bool valid = numbers.Length == 2;
                foreach (var n in numbers) valid = valid && n.Length > 0 && n.Length <= 10 && AsciiDigits(n);
                if (!valid)
                {
                    Send(c, 400, "bad request version");
                    return;
                }
                long major = 0;
                foreach (var ch in numbers[0]) major = major * 10 + (ch - '0');
                if (major >= 2)
                {
                    Send(c, 505, "invalid http version");
                    return;
                }
            }
            if (words.Count != 3)
            {
                Send(c, 400, "bad request syntax");
                return;
            }
            c.Method = words[0];
            c.Path = words[1];
            if (c.Path.StartsWith("//", StringComparison.Ordinal))
            {
                int at = 0;
                while (at < c.Path.Length && c.Path[at] == '/') at++;
                c.Path = "/" + c.Path.Substring(at); // http.server's gh-87389 fix
            }
            c.Lines.Add(line);
        }

        private void OnHead(Conn c)
        {
            var headers = Headers(c.Lines.Skip(1));
            c.Complete = !(headers.ContainsKey("content-length") || headers.ContainsKey("transfer-encoding"));
            if (c.Method != "GET" && c.Method != "POST")
            {
                Send(c, 501, Encoding.UTF8.GetBytes("unsupported method"), Text, c.Method == "HEAD");
                return;
            }
            if (!headers.TryGetValue("host", out var host) || host != Host + ":" + _port.ToString(CultureInfo.InvariantCulture))
            {
                Send(c, 403, "forbidden");
                return;
            }
            var prefix = "/c/" + Secret + "/";
            var path = c.Path;
            int query = path.IndexOf("?", StringComparison.Ordinal);
            if (query >= 0) path = path.Substring(0, query);
            if (!path.StartsWith(prefix, StringComparison.Ordinal))
            {
                NotFound(c);
                return;
            }
            c.Sub = path.Substring(prefix.Length);
            if (c.Method == "GET")
            {
                ServeGet(c);
                return;
            }
            if ((c.Sub == "build" && _buildTx == null) || (c.Sub != "build" && c.Sub != "tx"))
            {
                NotFound(c);
                return;
            }
            int length = ContentLength(headers.TryGetValue("content-length", out var raw) ? raw : "0");
            if (length <= 0 || length > MaxBody)
            {
                Send(c, length > MaxBody ? 413 : 400, "bad body");
                return;
            }
            c.Need = length;
            c.State = ConnState.Body;
            if (c.Len >= c.Need) OnBody(c);
        }

        /// <summary>A Content-Length as Python's int() reads it: MaxBody + 1 when larger, -1 where int() raises.</summary>
        private static int ContentLength(string raw)
        {
            System.Numerics.BigInteger n;
            try
            {
                n = Py.Int(raw);
            }
            catch (Exception)
            {
                return -1;
            }
            return n > MaxBody ? MaxBody + 1 : n < 0 ? -1 : (int)n;
        }

        private void ServeGet(Conn c)
        {
            var sub = c.Sub == "" ? "index.html" : c.Sub;
            if (Assets.Contains(sub))
            {
                byte[]? body = null;
                if (_loadAsset != null)
                {
                    try
                    {
                        body = _loadAsset(sub);
                    }
                    catch (Exception)
                    {
                        body = null;
                    }
                }
                if (body == null) Send(c, 500, "asset missing");
                else Send(c, 200, body, AssetType(sub));
            }
            else if (sub == "session.json")
            {
                SendJson(c, 200, _sessionInfo);
            }
            else if (sub == "status")
            {
                JsonObject? status = null;
                if (_getStatus != null)
                {
                    try
                    {
                        status = _getStatus();
                    }
                    catch (Exception)
                    {
                        status = null;
                    }
                }
                if (status == null) Unavailable(c);
                else SendJson(c, 200, status);
            }
            else if (sub == "logo" && _loadLogo != null)
            {
                byte[]? body = null;
                string? ctype = null;
                try
                {
                    var logo = _loadLogo();
                    body = logo.Body;
                    ctype = logo.ContentType;
                }
                catch (Exception)
                {
                    body = null;
                }
                if (body == null || ctype == null || !RasterTypes.Contains(ctype)) NotFound(c);
                else Send(c, 200, body, ctype);
            }
            else
            {
                NotFound(c);
            }
        }

        private void OnBody(Conn c)
        {
            var raw = new byte[c.Need];
            Buffer.BlockCopy(c.Buf, 0, raw, 0, c.Need);
            c.Complete = c.Len == c.Need;
            if (!(ParseBody(raw) is JsonObject data))
            {
                Send(c, 400, "bad body");
                return;
            }
            if (c.Sub == "build")
            {
                string account;
                try
                {
                    account = PageMessages.ParseBuildMessage(data);
                }
                catch (Exception)
                {
                    Send(c, 400, "bad account");
                    return;
                }
                Job? job = null;
                if (_buildTx != null)
                {
                    try
                    {
                        job = _buildTx(account);
                    }
                    catch (Exception)
                    {
                        job = null; // the worker is busy, the RPC is down...: the page offers a retry
                    }
                }
                if (c.State == ConnState.Closed)
                {
                    // BuildTx stopped the server: nobody waits for this build
                    if (job != null && !job.Done()) job.Cancel();
                    return;
                }
                if (job == null)
                {
                    Unavailable(c);
                    return;
                }
                c.Job = job;
                c.State = ConnState.Building;
                c.Deadline = Now() + BuildTimeoutS;
                CheckBuild(c);
                return;
            }
            (string TxHash, string? Sender) hint;
            try
            {
                hint = PageMessages.ParseTxMessage(data);
            }
            catch (Exception)
            {
                Send(c, 400, "bad tx hash");
                return;
            }
            if (_onTx == null)
            {
                Unavailable(c);
                return;
            }
            try
            {
                _onTx(hint.TxHash, hint.Sender);
            }
            catch (Exception)
            {
                Unavailable(c);
                return;
            }
            SendJson(c, 202, new JsonObject { { "ok", true } }); // (nothing when OnTx stopped the server)
        }

        /// <summary>The body as Python's json.loads(raw.decode("utf-8")) takes it (strict UTF-8, a BOM refused), or null.</summary>
        private static object? ParseBody(byte[] raw)
        {
            if (!Coio.IsUtf8(raw)) return null;
            try
            {
                return Json.Loads(Encoding.UTF8.GetString(raw));
            }
            catch (Exception)
            {
                return null;
            }
        }

        /// <summary>
        /// The held /build connection: answered once its job is done, 503 (and the job cancelled) after
        /// BuildTimeoutS. A failed or cancelled job, or a result that is not a JsonObject, is a 503.
        /// </summary>
        private void CheckBuild(Conn c)
        {
            var job = c.Job;
            if (job == null)
            {
                Unavailable(c);
                return;
            }
            if (job.Done())
            {
                c.Job = null;
                object? value = null;
                if (job.Exception() == null)
                {
                    try
                    {
                        value = job.Result();
                    }
                    catch (Exception)
                    {
                        value = null;
                    }
                }
                if (value is JsonObject o) SendJson(c, 200, o);
                else Unavailable(c);
            }
            else if (Now() >= c.Deadline)
            {
                c.Job = null;
                job.Cancel(); // don't build it later for a page that stopped waiting
                Unavailable(c);
            }
        }

        private void NotFound(Conn c) => Send(c, 404, "not found");

        private void Unavailable(Conn c) => SendJson(c, 503, new JsonObject { { "error", "unavailable" } });

        private void SendJson(Conn c, int code, object value)
        {
            string text;
            try
            {
                text = Json.Dumps(value);
            }
            catch (Exception)
            {
                code = 503; // a value json.dumps cannot write
                text = "{\"error\": \"unavailable\"}";
            }
            Send(c, code, Encoding.UTF8.GetBytes(text), "application/json");
        }

        private void Send(Conn c, int code, string body) => Send(c, code, Encoding.UTF8.GetBytes(body), Text);

        private void Send(Conn c, int code, byte[] body, string ctype, bool headOnly = false)
        {
            if (c.State == ConnState.Closed) return;
            var head = Encoding.ASCII.GetBytes(
                "HTTP/1.1 " + code.ToString(CultureInfo.InvariantCulture) + " " + Reason(code) + "\r\n"
                + "Content-Type: " + ctype + "\r\n"
                + "Content-Length: " + body.Length.ToString(CultureInfo.InvariantCulture) + "\r\n"
                + "Cache-Control: no-store\r\nReferrer-Policy: no-referrer\r\n"
                + "X-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n");
            var output = new byte[head.Length + (headOnly ? 0 : body.Length)];
            Buffer.BlockCopy(head, 0, output, 0, head.Length);
            if (!headOnly) Buffer.BlockCopy(body, 0, output, head.Length, body.Length);
            c.Out = output;
            c.Sent = 0;
            c.Buf = Array.Empty<byte>();
            c.Len = 0;
            c.State = ConnState.Writing;
            c.Deadline = Now() + DeadlineS;
            Write(c);
        }

        /// <summary>
        /// Sends what the socket takes now (at most WriteChunk * WriteChunks bytes); the rest waits for the next pump.
        /// Once all is out: closes, or sends the FIN and lingers when the client may still be sending.
        /// </summary>
        private void Write(Conn c)
        {
            if (Now() >= c.Deadline)
            {
                Close(c);
                return;
            }
            for (int i = 0; i < WriteChunks && c.Sent < c.Out.Length; i++)
            {
                int chunk = Math.Min(WriteChunk, c.Out.Length - c.Sent);
                int sent = c.Sock.Send(c.Out, c.Sent, chunk, SocketFlags.None, out SocketError error);
                if (error == SocketError.WouldBlock) break; // the socket is full
                if (error != SocketError.Success)
                {
                    Close(c);
                    return;
                }
                c.Sent += sent;
                if (sent < chunk) break; // the socket is full
            }
            if (c.Sent < c.Out.Length) return;
            c.Out = Array.Empty<byte>();
            if (c.Complete && c.Sock.Available == 0)
            {
                Close(c);
                return;
            }
            // the client may still be sending what was never read: drain it before closing, so the close does not
            // reset the connection under the answer
            c.Sock.Shutdown(SocketShutdown.Send);
            c.State = ConnState.Linger;
            c.Deadline = Math.Min(c.Deadline, Now() + LingerS);
        }

        /// <summary>
        /// The answer is out: read and discard what the client still sends until it closes (or LingerS or
        /// LingerMaxBytes pass), so closing does not reset the connection under the answer.
        /// </summary>
        private void Linger(Conn c)
        {
            if (Now() >= c.Deadline)
            {
                Close(c);
                return;
            }
            int avail = c.Sock.Available;
            if (avail == 0)
            {
                if (!c.Sock.Poll(0, SelectMode.SelectRead)) return;
                avail = c.Sock.Available;
                if (avail == 0)
                {
                    Close(c); // the client closed its side
                    return;
                }
            }
            int n = Math.Min(avail, LingerMaxBytes - c.Drained);
            if (n > 0)
            {
                int got = c.Sock.Receive(_scratch, 0, n, SocketFlags.None, out SocketError error);
                if (error == SocketError.WouldBlock) return;
                if (error != SocketError.Success || got <= 0)
                {
                    Close(c);
                    return;
                }
                c.Drained += got;
            }
            if (c.Drained >= LingerMaxBytes) Close(c);
        }

        private void Close(Conn c)
        {
            if (c.State == ConnState.Closed) return;
            c.State = ConnState.Closed;
            var job = c.Job;
            c.Job = null;
            Abort(c.Sock);
            if (job != null && !job.Done()) job.Cancel(); // the page stopped waiting: don't build it later
        }

        /// <summary>Close() without a timeout: a graceful close that never waits (the kernel sends what is queued).</summary>
        private static void Abort(Socket? socket)
        {
            if (socket == null) return;
            try
            {
                socket.Close();
            }
            catch (Exception)
            {
                // already gone
            }
        }

        // ---- HTTP, as Python reads it -------------------------------------------------------------------------

        /// <summary>
        /// The header block as email.parser (compat32) reads it: {lowercase name: first value}, as local_server.gd's
        /// _headers. <paramref name="lines"/> (each read up to an LF, the LF removed) are split again at each \r\n, \r
        /// or \n (feedparser's universal newlines); the first non-header line ends the block. A value loses leading
        /// blanks and trailing \r\n, not trailing spaces; a continuation line is appended line ends and all (so a
        /// folded value matches nothing); an empty name or an envelope "From " line is skipped.
        /// </summary>
        internal static Dictionary<string, string> Headers(IEnumerable<string> lines)
        {
            var output = new Dictionary<string, string>(StringComparer.Ordinal);
            var parts = new List<string>(); // the header being read: its first line, then continuations
            foreach (var line in UniversalLines(lines))
            {
                if (line.StartsWith(" ", StringComparison.Ordinal) || line.StartsWith("\t", StringComparison.Ordinal))
                {
                    if (parts.Count > 0) parts.Add(line);
                    continue;
                }
                bool from = line.StartsWith("From ", StringComparison.Ordinal);
                int colon = line.IndexOf(":", StringComparison.Ordinal);
                if (!from && (colon < 0 || !HeaderName(line, colon))) break;
                FinishHeader(output, parts);
                parts = new List<string>();
                if (!from && colon > 0) parts.Add(line);
            }
            FinishHeader(output, parts);
            return output;
        }

        // Adds the header `parts` hold (its first line, then its continuation lines) unless its name is in `output`.
        private static void FinishHeader(Dictionary<string, string> output, List<string> parts)
        {
            if (parts.Count == 0) return;
            var first = parts[0];
            int colon = first.IndexOf(":", StringComparison.Ordinal);
            var name = AsciiLower(first.Substring(0, colon));
            if (output.ContainsKey(name)) return;
            int at = colon + 1;
            while (at < first.Length && (first[at] == ' ' || first[at] == '\t')) at++;
            output[name] = StripLineEnds(first.Substring(at) + string.Concat(parts.Skip(1)));
        }

        // `lines` (each read up to an LF, the LF removed) split after each \r of the block they make, line ends kept
        // (each line's own end becomes \n, or stays \r\n after a final \r).
        private static IEnumerable<string> UniversalLines(IEnumerable<string> lines)
        {
            foreach (var line in lines)
            {
                int start = 0;
                while (true)
                {
                    int cr = line.IndexOf("\r", start, StringComparison.Ordinal);
                    if (cr < 0 || cr == line.Length - 1)
                    {
                        yield return line.Substring(start) + "\n";
                        break;
                    }
                    yield return line.Substring(start, cr + 1 - start);
                    start = cr + 1;
                }
            }
        }

        // The name before the colon: printable ASCII, no space (an empty one is skipped by the caller).
        private static bool HeaderName(string line, int colon)
        {
            for (int i = 0; i < colon; i++)
            {
                if (line[i] < 0x21 || line[i] > 0x7e) return false;
            }
            return true;
        }

        private static string AsciiLower(string s)
        {
            var chars = s.ToCharArray();
            for (int i = 0; i < chars.Length; i++)
            {
                if (chars[i] >= 'A' && chars[i] <= 'Z') chars[i] = (char)(chars[i] + 32);
            }
            return new string(chars);
        }

        /// <summary>Python's str.split() with no argument.</summary>
        private static List<string> PySplit(string s)
        {
            var words = new List<string>();
            int start = -1;
            for (int i = 0; i < s.Length; i++)
            {
                if (Py.IsSpace(s[i]))
                {
                    if (start >= 0) words.Add(s.Substring(start, i - start));
                    start = -1;
                }
                else if (start < 0)
                {
                    start = i;
                }
            }
            if (start >= 0) words.Add(s.Substring(start));
            return words;
        }

        private static bool AsciiDigits(string s)
        {
            foreach (var ch in s)
            {
                if (ch < '0' || ch > '9') return false;
            }
            return true;
        }

        /// <summary><paramref name="s"/> without its trailing \r and \n characters (Python's rstrip("\r\n")).</summary>
        private static string StripLineEnds(string s)
        {
            int end = s.Length;
            while (end > 0 && (s[end - 1] == '\r' || s[end - 1] == '\n')) end--;
            return end == s.Length ? s : s.Substring(0, end);
        }

        /// <summary>Bytes [start, end) as Python's str(b, "iso-8859-1").</summary>
        private static string Latin1(byte[] b, int start, int end)
        {
            var chars = new char[end - start];
            for (int i = 0; i < chars.Length; i++) chars[i] = (char)b[start + i];
            return new string(chars);
        }

        private static int FindLf(byte[] b, int from, int to)
        {
            for (int i = from; i < to; i++)
            {
                if (b[i] == 10) return i;
            }
            return -1;
        }

        /// <summary>base64url without padding: Python's secrets.token_urlsafe of these bytes.</summary>
        internal static string Base64Url(byte[] bytes)
        {
            var s = Convert.ToBase64String(bytes);
            int end = s.Length;
            while (end > 0 && s[end - 1] == '=') end--;
            var chars = new char[end];
            for (int i = 0; i < end; i++) chars[i] = s[i] == '+' ? '-' : s[i] == '/' ? '_' : s[i];
            return new string(chars);
        }

        private static string AssetType(string name) =>
            name == "index.html" ? "text/html; charset=utf-8" : "application/javascript; charset=utf-8";

        private static string Reason(int code)
        {
            switch (code)
            {
                case 200: return "OK";
                case 202: return "Accepted";
                case 400: return "Bad Request";
                case 403: return "Forbidden";
                case 404: return "Not Found";
                case 413: return "Request Entity Too Large";
                case 414: return "Request-URI Too Long";
                case 431: return "Request Header Fields Too Large";
                case 500: return "Internal Server Error";
                case 501: return "Not Implemented";
                case 503: return "Service Unavailable";
                case 505: return "HTTP Version Not Supported";
                default: return "Unknown";
            }
        }
    }
}
