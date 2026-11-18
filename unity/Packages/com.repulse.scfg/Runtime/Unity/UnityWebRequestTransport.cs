// No Python twin: the glue's real transport (Python's rpc.UrllibTransport, Godot's core/http_transport.gd, the
// driver's HttpClientTransport), on UnityWebRequest so that one code path also runs in WebGL builds (fetch()).
//
// PostJson POSTs Json.Dumps(payload) as application/json and completes, on the game's thread, with
// Coio.ParseRpcBody(status, body) (an HTTP error answer included: its status and body decide), or fails with a
// TransportError without a status when no HTTP answer came (refused, unresolvable, TLS, timeout). The body is read
// through a DownloadHandlerScript that stops the download as soon as it passes Coio.MaxResponseBytes ("response too
// large"), so a hostile endpoint cannot fill the game's memory. On WebGL (checked on 6000.6) refusing a chunk in
// ReceiveData does not stop the browser's fetch: the request would end only once the whole body is in. So Pump(),
// once per frame from the facade, aborts every request whose download passed the cap (the browser's fetch is
// aborted with it), giving the same "response too large" a frame or so later. UnityWebRequest.timeout is whole
// seconds and does not bound the name lookup: with a DNS server that never answers, a request waited for the
// resolver's own timeout (about 20 s measured in the testbed), past the 10 s each RPC request is given. So Pump() also
// fails every request older than its timeout ("timed out after Ns"), the lookup included, as Godot's HTTPRequest
// timeout does. Off the web the User-Agent is urllib's "scfg/0.1"; a browser keeps its own.
//
// Close() (the game quitting, or the facade reset with domain reload off) aborts every request in flight; their
// callers resume with HostGone, which is not a TransportError, so an RPC client does not cool down a healthy endpoint.
// Later posts fail with HostGone at once.
#nullable enable
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;
using System.Threading.Tasks;
using Scfg.Core;
using UnityEngine;
using UnityEngine.Networking;

namespace Scfg
{
    /// <summary>The transport's host went away (Godot's HostGone): <see cref="UnityWebRequestTransport.Close"/> ended the request.</summary>
    public sealed class HostGone : Exception
    {
        public HostGone(string url) : base("the transport was closed (request to " + url + ")") { }
    }

    /// <summary>The <see cref="ITransport"/> of a Unity game (see the file header).</summary>
    public sealed class UnityWebRequestTransport : ITransport
    {
        public const string UserAgent = "scfg/0.1";
        public const int RedirectLimit = 8;

        private readonly HashSet<Pending> _inFlight = new HashSet<Pending>();
        private bool _closed;

        /// <summary>The largest response body accepted, in bytes.</summary>
        public int BodySizeLimit { get; set; } = Coio.MaxResponseBytes;

        /// <summary>Seconds, for the requests' deadlines (Pump); unscaled real time. Test hook.</summary>
        internal Func<double> Clock { get; set; } = () => Time.realtimeSinceStartupAsDouble;

        /// <summary>How many posts were started (the integration driver's transport_stats).</summary>
        public int Posts { get; private set; }

        public int InFlight => _inFlight.Count;

        /// <summary>Test hook: the most bytes a request in flight has downloaded so far (0 with none).</summary>
        internal ulong MostDownloaded()
        {
            ulong most = 0;
            foreach (var p in _inFlight) most = Math.Max(most, p.Downloaded);
            return most;
        }

        public Task<object?> PostJson(string url, JsonObject payload, double timeout)
        {
            var tcs = new TaskCompletionSource<object?>();
            if (_closed)
            {
                tcs.SetException(new HostGone(url));
                return tcs.Task;
            }
            Posts++;
            UnityWebRequest request;
            CappedDownload download;
            try
            {
                download = new CappedDownload(BodySizeLimit);
                request = new UnityWebRequest(url, UnityWebRequest.kHttpVerbPOST, download,
                    new UploadHandlerRaw(new UTF8Encoding(false).GetBytes(Json.Dumps(payload))));
                request.SetRequestHeader("Content-Type", "application/json");
#if !UNITY_WEBGL || UNITY_EDITOR
                request.SetRequestHeader("User-Agent", UserAgent);
#endif
                request.redirectLimit = RedirectLimit;
                request.timeout = WholeSeconds(timeout);
            }
            catch (Exception e) when (e is ArgumentException || e is InvalidOperationException || e is UriFormatException)
            {
                // an unusable URL: configured URLs are validated, so this is not reachable from a config
                tcs.SetException(new TransportError("cannot start the request to " + url + ": " + e.Message));
                return tcs.Task;
            }
            var pending = new Pending(this, url, request, download, tcs, Clock() + Math.Max(0, timeout), timeout);
            _inFlight.Add(pending);
            UnityWebRequestAsyncOperation op;
            try
            {
                op = request.SendWebRequest();
            }
            catch (Exception e) when (e is ArgumentException || e is InvalidOperationException || e is UriFormatException)
            {
                pending.Fail(new TransportError("cannot start the request to " + url + ": " + e.Message));
                return tcs.Task;
            }
            if (op.isDone) pending.Finish();
            else op.completed += _ => pending.Finish();
            return tcs.Task;
        }

        /// <summary>
        /// Once per frame: a request whose download passed <see cref="BodySizeLimit"/> is aborted and fails with
        /// "response too large" (on WebGL a refused chunk does not stop the browser's download), and one older than its
        /// timeout fails with "timed out" (UnityWebRequest's own timeout leaves the name lookup out; see the file header).
        /// </summary>
        public void Pump()
        {
            if (_inFlight.Count == 0) return;
            double now = Clock();
            foreach (var p in new List<Pending>(_inFlight))
            {
                p.AbortIfTooLarge();
                p.AbortIfLate(now);
            }
        }

        /// <summary>Ends every request in flight with <see cref="HostGone"/> and refuses later ones.</summary>
        public void Close()
        {
            _closed = true;
            foreach (var p in new List<Pending>(_inFlight)) p.Fail(new HostGone(p.Url));
        }

        /// <summary>UnityWebRequest.timeout for <paramref name="seconds"/>: whole seconds, rounded up, at least 1.</summary>
        public static int WholeSeconds(double seconds)
        {
            if (double.IsNaN(seconds) || seconds <= 1) return 1;
            if (seconds >= int.MaxValue) return int.MaxValue;
            return (int)Math.Ceiling(seconds);
        }

        private sealed class Pending
        {
            private readonly UnityWebRequestTransport _owner;
            private readonly UnityWebRequest _request;
            private readonly CappedDownload _download;
            private readonly TaskCompletionSource<object?> _tcs;
            private readonly double _deadline;
            private readonly double _timeout;
            private bool _done;

            public string Url { get; }

            public ulong Downloaded
            {
                get
                {
                    try
                    {
                        return _done ? 0 : _request.downloadedBytes;
                    }
                    catch (Exception)
                    {
                        return 0; // the request is already disposed
                    }
                }
            }

            public Pending(UnityWebRequestTransport owner, string url, UnityWebRequest request, CappedDownload download,
                           TaskCompletionSource<object?> tcs, double deadline, double timeout)
            {
                _owner = owner;
                Url = url;
                _request = request;
                _download = download;
                _tcs = tcs;
                _deadline = deadline;
                _timeout = timeout;
            }

            /// <summary>Fails the request once it is older than its timeout, however far it got (the lookup included).</summary>
            public void AbortIfLate(double now)
            {
                if (_done || _timeout <= 0 || now < _deadline) return;
                Fail(new TransportError("timed out after " + _timeout.ToString("0.###", CultureInfo.InvariantCulture) + "s"));
            }

            public void Fail(Exception error)
            {
                if (_done) return;
                _done = true;
                _owner._inFlight.Remove(this);
                try
                {
                    _request.Abort();
                }
                finally
                {
                    _request.Dispose();
                }
                _tcs.TrySetException(error);
            }

            public void AbortIfTooLarge()
            {
                if (_done) return;
                bool tooLarge;
                try
                {
                    tooLarge = _download.TooLarge || _request.downloadedBytes > (ulong)Math.Max(0, _owner.BodySizeLimit);
                }
                catch (Exception)
                {
                    return; // the request is already disposed: Finish ran
                }
                if (tooLarge) Fail(new TransportError(Coio.ResponseTooLarge));
            }

            public void Finish()
            {
                if (_done) return;
                _done = true;
                _owner._inFlight.Remove(this);
                object? value = null;
                Exception? error = null;
                try
                {
                    if (_download.TooLarge)
                        error = new TransportError(Coio.ResponseTooLarge);
                    else if (_request.result == UnityWebRequest.Result.Success || _request.result == UnityWebRequest.Result.ProtocolError)
                        value = Coio.ParseRpcBody((int)_request.responseCode, _download.Body());
                    else
                        error = new TransportError(Describe(_request));
                }
                catch (Exception e)
                {
                    error = e; // ParseRpcBody's TransportError, or a glue fault: the caller gets it either way
                }
                finally
                {
                    _request.Dispose();
                }
                if (error != null) _tcs.TrySetException(error);
                else _tcs.TrySetResult(value);
            }

            private static string Describe(UnityWebRequest r)
            {
                string why = string.IsNullOrEmpty(r.error) ? r.result.ToString() : r.error;
                if (why.IndexOf("timeout", StringComparison.OrdinalIgnoreCase) >= 0 ||
                    why.IndexOf("timed out", StringComparison.OrdinalIgnoreCase) >= 0)
                    return "timed out after " + r.timeout.ToString(CultureInfo.InvariantCulture) + "s";
                return why;
            }
        }

        /// <summary>A body buffer that refuses to grow past its cap: the download stops there.</summary>
        private sealed class CappedDownload : DownloadHandlerScript
        {
            private readonly int _cap;
            private readonly MemoryStream _body = new MemoryStream();

            public bool TooLarge { get; private set; }

            public CappedDownload(int cap)
            {
                _cap = cap;
            }

            public byte[] Body() => _body.ToArray();

            protected override bool ReceiveData(byte[] data, int dataLength)
            {
                if (data == null || dataLength <= 0) return true;
                if (_body.Length + dataLength > _cap)
                {
                    TooLarge = true;
                    return false; // aborts the request
                }
                _body.Write(data, 0, dataLength);
                return true;
            }

            protected override byte[] GetData() => _body.ToArray();
        }
    }
}
