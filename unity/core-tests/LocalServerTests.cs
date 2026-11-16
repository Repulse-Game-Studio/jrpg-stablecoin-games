// Twin of godot/tests/test_local_server.gd: what only a non-blocking server has to prove about Scfg.Desktop.CheckoutServer
// (Runtime/Desktop/LocalServer.cs, the twin of scfg_core/local_server.py). Its HTTP route and status matrix against
// local_server.py lives in the shared suite (tests/server_security/test_server_security.py).
//
// Here: Pump() never waits on a socket (a slow, partial or non-reading client, a build that takes frames), the
// deadlines and the linger, the connection cap, callbacks that throw or stop the server. The tests drive the server
// synchronously over real loopback sockets: the test's clients are blocking sockets with timeouts, read only when they
// have bytes, and the server is pumped in the test's loop (Thread.Sleep(1) lets the kernel move bytes between pumps;
// only the test sleeps). A fake clock stands in for the passing seconds.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using NUnit.Framework;
using Scfg.Desktop;

namespace Scfg.Core.Tests
{
    public class LocalServerTests
    {
        private const string Account = "CejwupnRUhnYHb72dZ8rqnWsvFw49MDYKZ2SvurkX4K3";
        private const string Tx = "0xabababababababababababababababababababababababababababababababab";

        private sealed class FakeClock
        {
            public double Now = 1000.0;
            public double Read() => Now;
        }

        private sealed class Response
        {
            public int Status;
            public Dictionary<string, string> Headers = new Dictionary<string, string>();
            public byte[] Body = Array.Empty<byte>();
            public string Text => Encoding.UTF8.GetString(Body);
        }

        private readonly List<CheckoutServer> _servers = new List<CheckoutServer>();
        private readonly List<Socket> _clients = new List<Socket>();

        [TearDown]
        public void TearDown()
        {
            foreach (var s in _servers) s.Stop();
            foreach (var c in _clients) c.Dispose();
            _servers.Clear();
            _clients.Clear();
        }

        private CheckoutServer Server(FakeClock clock, Action<CheckoutServerOptions>? edit = null)
        {
            var o = new CheckoutServerOptions
            {
                LoadAsset = name => Encoding.UTF8.GetBytes("<" + name + ">"),
                SessionInfo = new JsonObject { { "chain_id", 42161L } },
                OnTx = (tx, sender) => { },
                GetStatus = () => new JsonObject { { "state", "waiting" } },
                Clock = clock.Read,
            };
            edit?.Invoke(o);
            var srv = new CheckoutServer(o);
            _servers.Add(srv);
            srv.Start();
            return srv;
        }

        private CheckoutServer Server(Action<CheckoutServerOptions>? edit = null) => Server(new FakeClock(), edit);

        /// <summary>A connected client, the server pumped until it took the connection.</summary>
        private Socket Connect(CheckoutServer srv)
        {
            var c = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp)
            {
                ReceiveTimeout = 5000,
                SendTimeout = 5000,
            };
            _clients.Add(c);
            int before = srv.AcceptedCount;
            c.Connect(IPAddress.Loopback, srv.Port); // the kernel completes it from the listen backlog
            Until(srv, () => srv.AcceptedCount > before, "the server accepts the connection");
            return c;
        }

        private static byte[] Latin1(string s) => s.Select(ch => (byte)ch).ToArray();

        private static void Send(Socket c, string text) => c.Send(Latin1(text));

        private static string Head(CheckoutServer srv, string method, string sub, int length = -1)
        {
            var h = method + " /c/" + srv.Secret + "/" + sub + " HTTP/1.1\r\nHost: 127.0.0.1:" + srv.Port.ToString(CultureInfo.InvariantCulture) + "\r\n";
            if (length >= 0) h += "Content-Length: " + length.ToString(CultureInfo.InvariantCulture) + "\r\n";
            return h + "\r\n";
        }

        private static string Post(CheckoutServer srv, string sub, string body) =>
            Head(srv, "POST", sub, Encoding.UTF8.GetByteCount(body)) + body;

        /// <summary>Pumps (1 ms apart, the test's sleep) until <paramref name="done"/> or 5 s of real time.</summary>
        private static void Until(CheckoutServer srv, Func<bool> done, string what, double seconds = 5)
        {
            var sw = Stopwatch.StartNew();
            while (!done())
            {
                if (sw.Elapsed.TotalSeconds > seconds) Assert.Fail("timed out: " + what);
                srv.Pump();
                Thread.Sleep(1);
            }
        }

        private static void PumpFor(CheckoutServer srv, int frames)
        {
            for (int i = 0; i < frames; i++)
            {
                srv.Pump();
                Assert.That(srv.NonBlocking(), Is.True, "every socket is non-blocking");
                Thread.Sleep(1);
            }
        }

        /// <summary>Reads what the client has received, pumping meanwhile, until a whole response is there (or null).</summary>
        private static Response? Read(CheckoutServer srv, Socket c, double seconds = 5)
        {
            var buf = new MemoryStream();
            var chunk = new byte[1 << 20];
            long total = -1; // the response's size once its head is in
            var sw = Stopwatch.StartNew();
            while (sw.Elapsed.TotalSeconds < seconds)
            {
                srv.Pump();
                int n = c.Available;
                if (n > 0)
                {
                    int got = c.Receive(chunk, 0, Math.Min(n, chunk.Length), SocketFlags.None);
                    buf.Write(chunk, 0, got);
                    if (total < 0) total = Size(buf.GetBuffer(), (int)buf.Length);
                    if (total >= 0 && buf.Length >= total) return Parse(buf.ToArray());
                    continue;
                }
                if (c.Poll(0, SelectMode.SelectRead) && c.Available == 0) break; // closed
                Thread.Sleep(1);
            }
            return Parse(buf.ToArray());
        }

        private static int HeadEnd(byte[] buf, int len)
        {
            for (int i = 0; i + 3 < len; i++)
            {
                if (buf[i] == 13 && buf[i + 1] == 10 && buf[i + 2] == 13 && buf[i + 3] == 10) return i;
            }
            return -1;
        }

        // The whole response's size in bytes once its head is in, else -1.
        private static long Size(byte[] buf, int len)
        {
            int end = HeadEnd(buf, len);
            if (end < 0) return -1;
            var head = ParseHead(buf, end);
            return end + 4 + int.Parse(head.Headers["content-length"], CultureInfo.InvariantCulture);
        }

        private static Response ParseHead(byte[] buf, int end)
        {
            var lines = new string(buf.Take(end).Select(b => (char)b).ToArray()).Split("\r\n");
            var r = new Response { Status = int.Parse(lines[0].Split(' ')[1], CultureInfo.InvariantCulture) };
            foreach (var line in lines.Skip(1))
            {
                int colon = line.IndexOf(':', StringComparison.Ordinal);
                r.Headers[line.Substring(0, colon).ToLowerInvariant()] = line.Substring(colon + 1).Trim();
            }
            return r;
        }

        private static Response? Parse(byte[] buf)
        {
            int end = HeadEnd(buf, buf.Length);
            if (end < 0) return null;
            var r = ParseHead(buf, end);
            int length = int.Parse(r.Headers["content-length"], CultureInfo.InvariantCulture);
            if (buf.Length < end + 4 + length) return null;
            r.Body = new byte[length];
            Buffer.BlockCopy(buf, end + 4, r.Body, 0, length);
            return r;
        }

        /// <summary>True once the server has closed the client's connection (pumping meanwhile, draining what arrives).</summary>
        private static bool Closed(CheckoutServer srv, Socket c, double seconds = 5)
        {
            var sink = new byte[65536];
            var sw = Stopwatch.StartNew();
            while (sw.Elapsed.TotalSeconds < seconds)
            {
                srv.Pump();
                if (c.Poll(0, SelectMode.SelectRead))
                {
                    if (c.Available == 0) return true;
                    try
                    {
                        if (c.Receive(sink, 0, Math.Min(c.Available, sink.Length), SocketFlags.None) == 0) return true;
                    }
                    catch (SocketException)
                    {
                        return true;
                    }
                    continue;
                }
                Thread.Sleep(1);
            }
            return false;
        }

        private static Response Answer(CheckoutServer srv, Socket c, string what, double seconds = 5)
        {
            var r = Read(srv, c, seconds);
            Assert.That(r, Is.Not.Null, "no answer: " + what);
            return r!;
        }

        // ---- start, stop, the secret ------------------------------------------------------------------------

        [Test]
        public void UrlAndSecret()
        {
            var srv = Server();
            Assert.That(srv.Running, Is.True);
            Assert.That(srv.Port, Is.GreaterThan(0));
            Assert.That(srv.Url, Is.EqualTo("http://127.0.0.1:" + srv.Port.ToString(CultureInfo.InvariantCulture) + "/c/" + srv.Secret + "/"));
            Assert.That(srv.Secret, Does.Match("^[A-Za-z0-9_-]{22}$"), "16 random bytes, base64url without padding");
            Assert.That(new CheckoutServer(new CheckoutServerOptions()).Secret, Is.Not.EqualTo(srv.Secret), "a new secret per server");
            Assert.That(CheckoutServer.Base64Url(new byte[] { 0xfb, 0xff, 0xbf }), Is.EqualTo("-_-_"));
            Assert.That(CheckoutServer.Base64Url(new byte[] { 0xfb }), Is.EqualTo("-w"));
            Assert.That(srv.BuildTimeoutS, Is.EqualTo(PageMessages.BuildTimeoutS));
        }

        [Test]
        public void StartIsIdempotentAndARestartKeepsTheSecret()
        {
            var srv = Server();
            int port = srv.Port;
            var secret = srv.Secret;
            srv.Start();
            Assert.That(srv.Port, Is.EqualTo(port), "a second Start() does nothing");
            srv.Stop();
            Assert.That(srv.Running, Is.False);
            srv.Stop(); // twice is fine
            srv.Pump(); // harmless when stopped
            var gone = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp);
            _clients.Add(gone);
            Assert.Throws<SocketException>(() => gone.Connect(IPAddress.Loopback, port), "Stop() releases the port");
            srv.Start();
            Assert.That(srv.Running, Is.True, "restarts");
            Assert.That(srv.Secret, Is.EqualTo(secret), "same secret, as Python's (a new port)");
            var c = Connect(srv);
            Send(c, Head(srv, "GET", "status"));
            Assert.That(Answer(srv, c, "status").Status, Is.EqualTo(200), "serves after the restart");
        }

        [TestCase("192.0.2.1")] // TEST-NET-1: not on this machine
        [TestCase("localhost")] // a name: never resolved (that would block)
        [TestCase("")]
        public void StartFailureIsARuntimeError(string host)
        {
            var srv = new CheckoutServer(new CheckoutServerOptions { Host = host });
            Assert.Throws<RuntimeError>(() => srv.Start());
            Assert.That(srv.Running, Is.False);
            srv.Pump(); // harmless
            srv.Stop();
        }

        [Test]
        public void ReopenIsANoOp()
        {
            var srv = Server();
            srv.Reopen();
            Assert.That(srv.Running, Is.True);
        }

        // ---- never waiting: slow, partial and non-reading clients, the deadlines -------------------------------

        [Test]
        public void ASlowClientDoesNotBlockOthersAndIsDroppedAfterTheDeadline()
        {
            var clock = new FakeClock();
            var srv = Server(clock);
            var slow = Connect(srv);
            Send(slow, "GET /c/" + srv.Secret + "/ HTTP/1.1\r\nHost: 127.0"); // and never the rest
            PumpFor(srv, 30);
            Assert.That(srv.ConnectionCount, Is.EqualTo(1), "held before the deadline");
            var other = Connect(srv);
            Send(other, Head(srv, "GET", "status"));
            var got = Answer(srv, other, "status");
            Assert.That(got.Status, Is.EqualTo(200), "a second client is answered while the first stalls");
            Assert.That(got.Text, Is.EqualTo("{\"state\": \"waiting\"}"), "Python's json.dumps");
            Assert.That(Closed(srv, other), Is.True, "the answered client is closed");
            clock.Now += CheckoutServer.DeadlineS - 0.5;
            PumpFor(srv, 3);
            Assert.That(srv.ConnectionCount, Is.EqualTo(1), "the slow client is still held at 9.5 s");
            clock.Now += 1.0;
            srv.Pump();
            Assert.That(srv.ConnectionCount, Is.EqualTo(0), "dropped in the pump after the deadline");
            Assert.That(Closed(srv, slow), Is.True, "the slow client is closed after the deadline");
        }

        [Test]
        public void APartialBodyIsDroppedAfterTheDeadline()
        {
            var clock = new FakeClock();
            var srv = Server(clock);
            var c = Connect(srv);
            Send(c, Head(srv, "POST", "tx", 100) + "{\"tx");
            PumpFor(srv, 30);
            Assert.That(srv.ConnectionStates(), Is.EqualTo(new[] { CheckoutServer.ConnState.Body }), "held while the body may still come");
            clock.Now += CheckoutServer.DeadlineS + 0.1;
            Assert.That(Closed(srv, c), Is.True, "dropped after the deadline");
            Assert.That(srv.ConnectionCount, Is.EqualTo(0));
        }

        [Test]
        public void AByteAtATimeClientIsServed()
        {
            var srv = Server();
            var c = Connect(srv);
            foreach (var ch in Head(srv, "GET", "session.json"))
            {
                c.Send(new[] { (byte)ch });
                srv.Pump();
            }
            var got = Answer(srv, c, "session.json");
            Assert.That(got.Status, Is.EqualTo(200));
            Assert.That(got.Text, Is.EqualTo("{\"chain_id\": 42161}"));
        }

        [Test]
        public void AClientThatNeverReadsDoesNotBlockAndIsDroppedAfterTheDeadline()
        {
            var big = new byte[32 << 20]; // far more than the socket buffers hold
            var clock = new FakeClock();
            var srv = Server(clock, o => o.LoadAsset = _ => big);
            var c = Connect(srv);
            Send(c, Head(srv, "GET", "app.js"));
            Until(srv, () => srv.ConnectionStates().SequenceEqual(new[] { CheckoutServer.ConnState.Writing }), "answering");
            PumpFor(srv, 40);
            Assert.That(srv.ConnectionStates(), Is.EqualTo(new[] { CheckoutServer.ConnState.Writing }), "still writing: the rest waits for the next pumps");
            clock.Now += CheckoutServer.DeadlineS + 0.1;
            srv.Pump();
            Assert.That(srv.ConnectionCount, Is.EqualTo(0), "dropped after the deadline");
        }

        [Test]
        public void PartialWritesAreCarriedToTheNextPump()
        {
            var big = new byte[16 << 20];
            for (int i = 0; i < big.Length; i++) big[i] = (byte)(i * 7 + (i >> 16));
            var srv = Server(o => o.LoadAsset = _ => big);
            var c = Connect(srv);
            Send(c, Head(srv, "GET", "evm.js"));
            Until(srv, () => srv.ConnectionStates().SequenceEqual(new[] { CheckoutServer.ConnState.Writing }), "answering");
            PumpFor(srv, 5);
            Assert.That(srv.ConnectionStates(), Is.EqualTo(new[] { CheckoutServer.ConnState.Writing }), "the answer did not fit at once");
            var got = Answer(srv, c, "the big asset", 30);
            Assert.That(got.Status, Is.EqualTo(200));
            Assert.That(got.Headers["content-type"], Is.EqualTo("application/javascript; charset=utf-8"));
            Assert.That(got.Body.AsSpan().SequenceEqual(big), Is.True, "every byte, in order, over many pumps");
            Assert.That(Closed(srv, c), Is.True, "closed once written");
        }

        // ---- /build -------------------------------------------------------------------------------------------

        [Test]
        public void BuildWaitsForTheJobWithoutBlocking()
        {
            var job = new Job();
            var asked = new List<string>();
            var clock = new FakeClock();
            var srv = Server(clock, o => o.BuildTx = account =>
            {
                asked.Add(account);
                return job;
            });
            var c = Connect(srv);
            Send(c, Post(srv, "build", "{\"account\": \"" + Account + "\"}"));
            PumpFor(srv, 30);
            Assert.That(asked, Is.EqualTo(new[] { Account }), "BuildTx got the normalised account once");
            Assert.That(c.Available, Is.EqualTo(0), "no answer before the job finishes");
            clock.Now += CheckoutServer.DeadlineS + 1.0; // past the request deadline: the build has its own
            srv.Pump();
            Assert.That(srv.ConnectionStates(), Is.EqualTo(new[] { CheckoutServer.ConnState.Building }), "the build connection is still held");
            job.SetResult(new JsonObject { { "transaction", "AA==" }, { "sol_balance", "2" } });
            var got = Answer(srv, c, "the build");
            Assert.That(got.Status, Is.EqualTo(200), "answered when the job finished");
            Assert.That(got.Text, Is.EqualTo("{\"transaction\": \"AA==\", \"sol_balance\": \"2\"}"), "the job's result");
            Assert.That(got.Headers["content-type"], Is.EqualTo("application/json"));
            Assert.That(job.Cancelled(), Is.False);
        }

        [Test]
        public void BuildTimeoutAnswers503AndCancelsTheJob()
        {
            var job = new Job();
            var clock = new FakeClock();
            var srv = Server(clock, o => o.BuildTx = _ => job);
            var c = Connect(srv);
            Send(c, Post(srv, "build", "{\"account\": \"" + Account + "\"}"));
            Until(srv, () => srv.ConnectionStates().SequenceEqual(new[] { CheckoutServer.ConnState.Building }), "building");
            clock.Now += PageMessages.BuildTimeoutS - 0.5;
            srv.Pump();
            Assert.That(job.Cancelled(), Is.False, "not before BuildTimeoutS");
            clock.Now += 1.0;
            var got = Answer(srv, c, "the timeout");
            Assert.That(got.Status, Is.EqualTo(503), "503 after BuildTimeoutS");
            Assert.That(got.Text, Is.EqualTo("{\"error\": \"unavailable\"}"));
            Assert.That(job.Cancelled(), Is.True, "the job is cancelled");
        }

        [Test]
        public void TheBuildTimeoutCanBeLowered()
        {
            var job = new Job();
            var clock = new FakeClock();
            var srv = Server(clock, o => o.BuildTx = _ => job);
            srv.BuildTimeoutS = 0.05;
            var c = Connect(srv);
            Send(c, Post(srv, "build", "{\"account\": \"" + Account + "\"}"));
            Until(srv, () => srv.ConnectionStates().SequenceEqual(new[] { CheckoutServer.ConnState.Building }), "building");
            clock.Now += 0.1;
            Assert.That(Answer(srv, c, "the timeout").Status, Is.EqualTo(503));
            Assert.That(job.Cancelled(), Is.True);
        }

        private static IEnumerable<TestCaseData> OddBuilds()
        {
            yield return new TestCaseData((Func<Job?>)(() =>
            {
                var j = new Job();
                j.SetException(new RuntimeError("rpc down"));
                return j;
            })).SetName("AFailedJobIs503");
            yield return new TestCaseData((Func<Job?>)(() =>
            {
                var j = new Job();
                j.SetResult("not an object");
                return j;
            })).SetName("AResultThatIsNotAJsonObjectIs503");
            yield return new TestCaseData((Func<Job?>)(() =>
            {
                var j = new Job();
                j.SetResult(null);
                return j;
            })).SetName("ANullResultIs503");
            yield return new TestCaseData((Func<Job?>)(() =>
            {
                var j = new Job();
                j.Cancel();
                return j;
            })).SetName("ACancelledJobIs503");
            yield return new TestCaseData((Func<Job?>)(() =>
            {
                var j = new Job();
                j.SetResult(new JsonObject { { "x", new object() } });
                return j;
            })).SetName("AResultJsonCannotWriteIs503");
            yield return new TestCaseData((Func<Job?>)(() => null)).SetName("BuildTxReturningNullIs503");
            yield return new TestCaseData((Func<Job?>)(() => throw new RuntimeError("busy"))).SetName("BuildTxThatThrowsIs503");
        }

        [TestCaseSource(nameof(OddBuilds))]
        public void FailedOrOddBuildJobsAre503(Func<Job?> make)
        {
            var srv = Server(o => o.BuildTx = _ => make()!);
            var c = Connect(srv);
            Send(c, Post(srv, "build", "{\"account\": \"" + Account + "\"}"));
            var got = Answer(srv, c, "the build");
            Assert.That(got.Status, Is.EqualTo(503));
            Assert.That(got.Text, Is.EqualTo("{\"error\": \"unavailable\"}"));
        }

        [Test]
        public void AJobDoneAtOnceIsAnsweredInTheSamePump()
        {
            var srv = Server(o => o.BuildTx = _ =>
            {
                var j = new Job();
                j.SetResult(new JsonObject { { "transaction", "AA==" } });
                return j;
            });
            var c = Connect(srv);
            Send(c, Post(srv, "build", "{\"account\": \"" + Account + "\"}"));
            Assert.That(Answer(srv, c, "the build").Status, Is.EqualTo(200));
        }

        [Test]
        public void StopCancelsAPendingBuild()
        {
            var job = new Job();
            var srv = Server(o => o.BuildTx = _ => job);
            var c = Connect(srv);
            Send(c, Post(srv, "build", "{\"account\": \"" + Account + "\"}"));
            Until(srv, () => srv.ConnectionStates().SequenceEqual(new[] { CheckoutServer.ConnState.Building }), "building");
            srv.Stop();
            Assert.That(job.Cancelled(), Is.True, "a page that is gone gets no build");
            Assert.That(srv.ConnectionCount, Is.EqualTo(0), "connections closed");
            Assert.That(Closed(srv, c), Is.True);
        }

        [Test]
        public void ABuildRouteNeedsBuildTx()
        {
            var srv = Server();
            var c = Connect(srv);
            Send(c, Post(srv, "build", "{\"account\": \"" + Account + "\"}"));
            Assert.That(Answer(srv, c, "build").Status, Is.EqualTo(404));
        }

        // ---- callbacks ------------------------------------------------------------------------------------------

        [Test]
        public void CallbacksThatThrowAnswerWithoutEscapingPump()
        {
            var srv = Server(o =>
            {
                o.OnTx = (tx, sender) => throw new InvalidOperationException("on_tx");
                o.GetStatus = () => throw new InvalidOperationException("status");
                o.LoadAsset = _ => throw new FileNotFoundException("asset");
                o.LoadLogo = () => throw new FileNotFoundException("logo");
                o.BuildTx = _ => throw new InvalidOperationException("build");
            });
            foreach (var (request, status, what) in new[]
                     {
                         (Post(srv, "tx", "{\"tx_hash\": \"" + Tx + "\"}"), 503, "OnTx"),
                         (Head(srv, "GET", "status"), 503, "GetStatus"),
                         (Head(srv, "GET", "app.js"), 500, "LoadAsset"),
                         (Head(srv, "GET", "logo"), 404, "LoadLogo"),
                         (Post(srv, "build", "{\"account\": \"" + Account + "\"}"), 503, "BuildTx"),
                     })
            {
                var c = Connect(srv);
                Send(c, request);
                Assert.That(Answer(srv, c, what).Status, Is.EqualTo(status), "a throwing " + what);
            }
            Assert.That(srv.Running, Is.True);
        }

        [Test]
        public void MissingCallbacksOrNullResultsAnswer503Or500()
        {
            var plain = Server(o =>
            {
                o.OnTx = null;
                o.GetStatus = null;
                o.LoadAsset = null;
                o.SessionInfo = null;
            });
            foreach (var (request, status, body) in new[]
                     {
                         (Post(plain, "tx", "{\"tx_hash\": \"" + Tx + "\"}"), 503, "{\"error\": \"unavailable\"}"),
                         (Head(plain, "GET", "status"), 503, "{\"error\": \"unavailable\"}"),
                         (Head(plain, "GET", ""), 500, "asset missing"),
                         (Head(plain, "GET", "session.json"), 200, "{}"),
                     })
            {
                var c = Connect(plain);
                Send(c, request);
                var got = Answer(plain, c, request);
                Assert.That((got.Status, got.Text), Is.EqualTo((status, body)), "a missing callback: " + request.Split(' ')[1]);
            }
            var nulls = Server(o =>
            {
                o.GetStatus = () => null!;
                o.LoadAsset = _ => null!;
            });
            foreach (var (request, status) in new[] { (Head(nulls, "GET", "status"), 503), (Head(nulls, "GET", "solana.js"), 500) })
            {
                var c = Connect(nulls);
                Send(c, request);
                Assert.That(Answer(nulls, c, request).Status, Is.EqualTo(status), "a callback that returns null: " + request.Split(' ')[1]);
            }
        }

        [Test]
        public void AStatusJsonCannotWriteIs503()
        {
            var srv = Server(o => o.GetStatus = () => new JsonObject { { "x", new object() } });
            var c = Connect(srv);
            Send(c, Head(srv, "GET", "status"));
            var got = Answer(srv, c, "status");
            Assert.That((got.Status, got.Text), Is.EqualTo((503, "{\"error\": \"unavailable\"}")));
        }

        private static IEnumerable<TestCaseData> Stoppers()
        {
            yield return new TestCaseData("OnTx").SetName("OnTxThatStopsTheServerIsSafe");
            yield return new TestCaseData("GetStatus").SetName("GetStatusThatStopsTheServerIsSafe");
            yield return new TestCaseData("LoadAsset").SetName("LoadAssetThatStopsTheServerIsSafe");
            yield return new TestCaseData("BuildTx").SetName("BuildTxThatStopsTheServerIsSafe");
        }

        [TestCaseSource(nameof(Stoppers))]
        public void ACallbackThatStopsTheServerIsSafe(string which)
        {
            CheckoutServer? holder = null;
            var built = new Job();
            var srv = Server(o =>
            {
                o.OnTx = (tx, sender) => holder!.Stop();
                o.GetStatus = () =>
                {
                    holder!.Stop();
                    return new JsonObject();
                };
                o.LoadAsset = _ =>
                {
                    holder!.Stop();
                    return new byte[1];
                };
                o.BuildTx = _ =>
                {
                    holder!.Stop();
                    return built;
                };
            });
            holder = srv;
            var c = Connect(srv);
            var other = Connect(srv); // a second connection, stepped after the one whose callback stops the server
            var request = which switch
            {
                "OnTx" => Post(srv, "tx", "{\"tx_hash\": \"" + Tx + "\"}"),
                "GetStatus" => Head(srv, "GET", "status"),
                "LoadAsset" => Head(srv, "GET", "index.html"),
                _ => Post(srv, "build", "{\"account\": \"" + Account + "\"}"),
            };
            Send(c, request);
            Send(other, "GET /c/");
            Until(srv, () => !srv.Running, "the callback stops the server");
            Assert.That(srv.ConnectionCount, Is.EqualTo(0), "no connection left");
            Assert.That(Closed(srv, c), Is.True, "the connection is closed without an answer");
            Assert.That(Closed(srv, other), Is.True, "and so is the other one");
            srv.Pump(); // harmless
        }

        [Test]
        public void ACallbackThatRestartsTheServerIsSafe()
        {
            CheckoutServer? holder = null;
            var srv = Server(o => o.GetStatus = () =>
            {
                holder!.Stop();
                holder.Start();
                return new JsonObject();
            });
            holder = srv;
            var c = Connect(srv);
            Send(c, Head(srv, "GET", "status"));
            int port = srv.Port;
            Until(srv, () => srv.Port != port, "the callback restarts the server");
            Assert.That(Closed(srv, c), Is.True, "the old connection is closed");
            Assert.That(srv.Running, Is.True);
            var fresh = Connect(srv);
            Send(fresh, Head(srv, "GET", "session.json"));
            Assert.That(Answer(srv, fresh, "after the restart").Status, Is.EqualTo(200));
        }

        [Test]
        public void PumpInsidePumpDoesNothing()
        {
            CheckoutServer? holder = null;
            int calls = 0;
            var srv = Server(o => o.GetStatus = () =>
            {
                calls++;
                holder!.Pump();
                return new JsonObject { { "state", "waiting" } };
            });
            holder = srv;
            var c = Connect(srv);
            Send(c, Head(srv, "GET", "status"));
            Assert.That(Answer(srv, c, "status").Status, Is.EqualTo(200));
            Assert.That(calls, Is.EqualTo(1));
        }

        [Test]
        public void OnTxGetsTheNormalisedHint()
        {
            var calls = new List<(string, string?)>();
            var srv = Server(o => o.OnTx = (tx, sender) => calls.Add((tx, sender)));
            var c = Connect(srv);
            Send(c, Post(srv, "tx", "{\"tx_hash\": \"0x" + new string('A', 2) + Tx.Substring(4).ToUpperInvariant()
                                    + "\", \"from\": \"0x52908400098527886e0f7030069857d2e4169ee7\"}"));
            var got = Answer(srv, c, "tx");
            Assert.That(got.Status, Is.EqualTo(202));
            Assert.That(got.Text, Is.EqualTo("{\"ok\": true}"));
            Assert.That(got.Headers["content-type"], Is.EqualTo("application/json"));
            Assert.That(calls, Is.EqualTo(new[] { ("0xaa" + Tx.Substring(4), (string?)"0x52908400098527886E0F7030069857D2E4169EE7") }));
        }

        [Test]
        public void TheLogoMustBeARasterImage()
        {
            Assert.That(CheckoutServer.RasterTypes, Is.EquivalentTo(new[] { "image/png", "image/jpeg", "image/webp" }), "the logo types Config accepts");
            foreach (var (ctype, status) in new[]
                     {
                         ("image/png", 200), ("image/jpeg", 200), ("image/webp", 200), ("image/svg+xml", 404),
                         ("text/html", 404), ("image/png\r\nX-Evil: 1", 404), ("IMAGE/PNG", 404),
                     })
            {
                var srv = Server(o => o.LoadLogo = () => (Encoding.ASCII.GetBytes("<svg/>"), ctype));
                var c = Connect(srv);
                Send(c, Head(srv, "GET", "logo"));
                var got = Answer(srv, c, ctype);
                Assert.That(got.Status, Is.EqualTo(status), ctype);
                Assert.That(got.Headers.ContainsKey("x-evil"), Is.False, "no header injection");
                if (status == 200) Assert.That(got.Headers["content-type"], Is.EqualTo(ctype));
                srv.Stop();
            }
            foreach (var bad in new (byte[], string)[] { (null!, "image/png"), (new byte[1], null!) })
            {
                var srv = Server(o => o.LoadLogo = () => bad);
                var c = Connect(srv);
                Send(c, Head(srv, "GET", "logo"));
                Assert.That(Answer(srv, c, "a null part").Status, Is.EqualTo(404));
                srv.Stop();
            }
        }

        // ---- limits ---------------------------------------------------------------------------------------------

        [Test]
        public void OversizedHeadsAre431()
        {
            var srv = Server();
            var c = Connect(srv);
            // lines each under MaxLine (as http.client reads them) that add up to more than MaxHead
            var pad = "X-Pad: " + new string('a', CheckoutServer.MaxLine - 10) + "\r\n";
            Send(c, "GET /c/" + srv.Secret + "/ HTTP/1.1\r\n" + string.Concat(Enumerable.Repeat(pad, CheckoutServer.MaxHead / pad.Length + 1)));
            var got = Answer(srv, c, "a head over MaxHead");
            Assert.That((got.Status, got.Text), Is.EqualTo((431, "headers too large")));
            Assert.That(got.Headers["x-content-type-options"], Is.EqualTo("nosniff"));
            c = Connect(srv);
            Send(c, "GET /c/" + srv.Secret + "/ HTTP/1.1\r\nX-Pad: " + new string('a', CheckoutServer.MaxLine));
            got = Answer(srv, c, "a header line over MaxLine");
            Assert.That((got.Status, got.Text), Is.EqualTo((431, "line too long")), "before its end arrives");
            c = Connect(srv);
            Send(c, "GET /c/" + srv.Secret + "/?q=" + new string('a', CheckoutServer.MaxLine));
            Assert.That(Answer(srv, c, "a long request line").Status, Is.EqualTo(414));
            // http.client counts the blank line that ends the head as one of its 100: 99 headers at most
            var most = "GET /c/" + srv.Secret + "/ HTTP/1.1\r\nHost: 127.0.0.1:" + srv.Port.ToString(CultureInfo.InvariantCulture) + "\r\n"
                       + string.Concat(Enumerable.Range(0, 98).Select(i => "X-" + i.ToString(CultureInfo.InvariantCulture) + ": 1\r\n"));
            c = Connect(srv);
            Send(c, most + "\r\n");
            Assert.That(Answer(srv, c, "99 headers").Status, Is.EqualTo(200), "99 header lines are read");
            c = Connect(srv);
            Send(c, most + "X-98: 1\r\n\r\n");
            Assert.That(Answer(srv, c, "100 headers").Status, Is.EqualTo(431), "100 header lines are too many");
        }

        [Test]
        public void ConnectionsAreCapped()
        {
            var clock = new FakeClock();
            var srv = Server(clock);
            var clients = new List<Socket>();
            for (int i = 0; i < CheckoutServer.MaxConnections + 4; i++)
            {
                var c = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp);
                _clients.Add(c);
                c.Connect(IPAddress.Loopback, srv.Port); // the backlog holds them all
                clients.Add(c);
            }
            Until(srv, () => srv.ConnectionCount == CheckoutServer.MaxConnections, "MaxConnections taken");
            PumpFor(srv, 20);
            Assert.That(srv.ConnectionCount, Is.EqualTo(CheckoutServer.MaxConnections), "at most MaxConnections are taken");
            clock.Now += CheckoutServer.DeadlineS + 0.1;
            srv.Pump(); // the idle ones are dropped (and the waiting ones taken in the same pump)...
            Until(srv, () => srv.ConnectionCount > 0, "the waiting connections are taken");
            Assert.That(srv.ConnectionCount, Is.EqualTo(4), "...and the 4 waiting ones taken next");
            var last = clients[clients.Count - 1];
            Send(last, Head(srv, "GET", "status"));
            Assert.That(Answer(srv, last, "a connection that waited").Status, Is.EqualTo(200), "served once taken");
        }

        // ---- closing: the linger ------------------------------------------------------------------------------

        [Test]
        public void AnAnswerToAnUnreadBodyLingersUntilTheClientCloses()
        {
            var clock = new FakeClock();
            var srv = Server(clock);
            var c = Connect(srv);
            Send(c, Head(srv, "POST", "tx", 99999) + new string('x', 20000)); // a body the server never reads
            var got = Answer(srv, c, "the 413");
            Assert.That(got.Status, Is.EqualTo(413), "answered before the body");
            Until(srv, () => srv.ConnectionStates().SequenceEqual(new[] { CheckoutServer.ConnState.Linger }), "lingering");
            Send(c, new string('y', 1000)); // the client still sends: drained, not a reset
            PumpFor(srv, 10);
            Assert.That(srv.ConnectionStates(), Is.EqualTo(new[] { CheckoutServer.ConnState.Linger }), "still draining");
            var rest = new byte[16];
            Assert.That(c.Receive(rest), Is.EqualTo(0), "the server sent its FIN after the answer");
            c.Shutdown(SocketShutdown.Send);
            Until(srv, () => srv.ConnectionCount == 0, "closed when the client closes");
        }

        [Test]
        public void TheLingerEndsAfterLingerS()
        {
            var clock = new FakeClock();
            var srv = Server(clock);
            var c = Connect(srv);
            Send(c, Head(srv, "POST", "tx", 99999) + "x");
            Assert.That(Answer(srv, c, "the 413").Status, Is.EqualTo(413));
            Until(srv, () => srv.ConnectionStates().SequenceEqual(new[] { CheckoutServer.ConnState.Linger }), "lingering");
            clock.Now += CheckoutServer.LingerS - 0.1;
            PumpFor(srv, 3);
            Assert.That(srv.ConnectionCount, Is.EqualTo(1), "held for LingerS");
            clock.Now += 0.2;
            srv.Pump();
            Assert.That(srv.ConnectionCount, Is.EqualTo(0), "closed after LingerS");
        }

        [Test]
        public void TheLingerEndsAfterLingerMaxBytes()
        {
            var srv = Server();
            var c = Connect(srv);
            Send(c, Head(srv, "POST", "tx", 99999));
            Assert.That(Answer(srv, c, "the 413").Status, Is.EqualTo(413));
            Until(srv, () => srv.ConnectionStates().SequenceEqual(new[] { CheckoutServer.ConnState.Linger }), "lingering");
            Send(c, new string('z', CheckoutServer.LingerMaxBytes + 10));
            Until(srv, () => srv.ConnectionCount == 0, "closed after LingerMaxBytes");
        }

        [Test]
        public void AnswerToACompleteRequestClosesAtOnce()
        {
            var srv = Server();
            var c = Connect(srv);
            Send(c, Head(srv, "GET", "status"));
            Assert.That(Answer(srv, c, "status").Status, Is.EqualTo(200));
            Until(srv, () => srv.ConnectionCount == 0, "closed once written");
            Assert.That(Closed(srv, c), Is.True);
        }

        [Test]
        public void AClientThatClosesItsSideAfterTheRequestGetsTheAnswer()
        {
            var srv = Server();
            var c = Connect(srv);
            Send(c, Head(srv, "GET", ""));
            c.Shutdown(SocketShutdown.Send);
            var got = Answer(srv, c, "the page");
            Assert.That((got.Status, got.Text), Is.EqualTo((200, "<index.html>")));
            Assert.That(got.Headers["content-type"], Is.EqualTo("text/html; charset=utf-8"));
        }

        [Test]
        public void AClientThatClosesBeforeItsHeadIsCompleteGetsNoAnswer()
        {
            var srv = Server();
            var c = Connect(srv);
            Send(c, "GET /c/" + srv.Secret + "/ HTTP/1.1\r\n");
            c.Shutdown(SocketShutdown.Send);
            Until(srv, () => srv.ConnectionCount == 0, "closed");
            Assert.That(c.Available, Is.EqualTo(0), "no answer");
        }

        [Test]
        public void AnEmptyRequestLineIsClosedWithoutAnAnswer()
        {
            var srv = Server();
            var c = Connect(srv);
            Send(c, "\r\n");
            Until(srv, () => srv.ConnectionCount == 0, "closed");
            Assert.That(Closed(srv, c), Is.True);
            Assert.That(c.Available, Is.EqualTo(0), "no answer");
        }

        // ---- the answer's shape ---------------------------------------------------------------------------------

        [Test]
        public void EveryAnswerCarriesTheSecurityHeaders()
        {
            var srv = Server();
            foreach (var (request, status) in new[]
                     {
                         (Head(srv, "GET", ""), 200), (Head(srv, "GET", "nope"), 404), (Head(srv, "OPTIONS", ""), 501),
                         ("GET /c/" + srv.Secret + "/ HTTP/1.1\r\nHost: evil.example\r\n\r\n", 403),
                         ("GET /c/" + srv.Secret + "/ HTTP/2.0\r\n\r\n", 505), ("GET / x HTTP/1.1\r\n\r\n", 400),
                     })
            {
                var c = Connect(srv);
                Send(c, request);
                var got = Answer(srv, c, request);
                Assert.That(got.Status, Is.EqualTo(status), request);
                Assert.That(got.Headers["cache-control"], Is.EqualTo("no-store"));
                Assert.That(got.Headers["referrer-policy"], Is.EqualTo("no-referrer"));
                Assert.That(got.Headers["x-content-type-options"], Is.EqualTo("nosniff"));
                Assert.That(got.Headers["connection"], Is.EqualTo("close"));
                Assert.That(got.Headers.Keys.Where(k => k.StartsWith("access-control-", StringComparison.Ordinal)), Is.Empty);
            }
        }

        [Test]
        public void HeadIsA501WithoutABody()
        {
            var srv = Server();
            var c = Connect(srv);
            Send(c, Head(srv, "HEAD", ""));
            var raw = new MemoryStream();
            var chunk = new byte[4096];
            var sw = Stopwatch.StartNew();
            while (sw.Elapsed.TotalSeconds < 5)
            {
                srv.Pump();
                if (c.Poll(0, SelectMode.SelectRead))
                {
                    int got = c.Available == 0 ? 0 : c.Receive(chunk);
                    if (got == 0) break;
                    raw.Write(chunk, 0, got);
                }
                Thread.Sleep(1);
            }
            var text = Encoding.ASCII.GetString(raw.ToArray());
            Assert.That(text, Does.StartWith("HTTP/1.1 501 Not Implemented\r\n"));
            Assert.That(text, Does.EndWith("\r\n\r\n"), "no body");
            Assert.That(text, Does.Contain("Content-Length: 18\r\n"), "the length of the body it would have");
        }

        [Test]
        public void HeadersAreReadAsEmailParserReadsThem()
        {
            Dictionary<string, string> H(params string[] lines) => CheckoutServer.Headers(lines);
            Assert.That(H("Host: a\r", "host: b\r"), Is.EqualTo(new Dictionary<string, string> { { "host", "a" } }), "the first counts");
            Assert.That(H("Host:\t a \r")["host"], Is.EqualTo("a "), "leading blanks go, trailing spaces stay");
            Assert.That(H("X-A: 1\rHost: a\r")["host"], Is.EqualTo("a"), "a bare CR ends a header");
            Assert.That(H("X-A: 1\r\rHost: a\r").ContainsKey("host"), Is.False, "a blank line ends the block");
            Assert.That(H("Host: a\r", " b\r")["host"], Is.EqualTo("a\r\n b"), "a folded value keeps its line ends");
            Assert.That(H("From x\r", "Host: a\r")["host"], Is.EqualTo("a"), "an envelope From line is skipped");
            Assert.That(H(": x\r", "Host: a\r")["host"], Is.EqualTo("a"), "an empty name is skipped");
            Assert.That(H("Bad Name: x\r", "Host: a\r").ContainsKey("host"), Is.False, "a non-header line ends the block");
            Assert.That(H("HOST: a")["host"], Is.EqualTo("a"), "names are lowercased; a bare LF line");
        }

        [TestCase("de-DE")]
        [TestCase("tr-TR")]
        [TestCase("ar-SA")]
        public void AnswersAreCultureInvariant(string culture)
        {
            var saved = CultureInfo.CurrentCulture;
            try
            {
                CultureInfo.CurrentCulture = new CultureInfo(culture);
                var srv = Server(o => o.SessionInfo = new JsonObject { { "price", 4.5 }, { "chain_id", 12345678L } });
                Assert.That(srv.Url, Does.Match("^http://127\\.0\\.0\\.1:[0-9]+/c/"));
                var c = Connect(srv);
                Send(c, Head(srv, "GET", "session.json"));
                var got = Answer(srv, c, "session.json");
                Assert.That(got.Status, Is.EqualTo(200));
                Assert.That(got.Text, Is.EqualTo("{\"price\": 4.5, \"chain_id\": 12345678}"));
                Assert.That(got.Headers["content-length"], Is.EqualTo(got.Body.Length.ToString(CultureInfo.InvariantCulture)));
            }
            finally
            {
                CultureInfo.CurrentCulture = saved;
            }
        }

        [Test]
        public void TheContentLengthIsReadAsPythonsInt()
        {
            var calls = new List<string>();
            var srv = Server(o => o.OnTx = (tx, sender) => calls.Add(tx));
            var body = "{\"tx_hash\": \"" + Tx + "\"}";
            var n = body.Length.ToString(CultureInfo.InvariantCulture);
            foreach (var (length, status) in new[]
                     {
                         (" " + body.Length.ToString(CultureInfo.InvariantCulture) + " ", 202),
                         ("0" + body.Length.ToString(CultureInfo.InvariantCulture), 202),
                         (n.Substring(0, 1) + "_" + n.Substring(1), 202), // 1_23: Python's int() reads 123
                         ("-5", 400), ("0", 400), ("4097", 413), ("99999999999999999999999999", 413),
                         ("1.5", 400), ("0x10", 400), ("", 400), (new string('9', 5000), 400),
                     })
            {
                var c = Connect(srv);
                var head = "POST /c/" + srv.Secret + "/tx HTTP/1.1\r\nHost: 127.0.0.1:" + srv.Port.ToString(CultureInfo.InvariantCulture)
                           + "\r\nContent-Length: " + length + "\r\n\r\n";
                Send(c, head + (status == 202 ? body : ""));
                Assert.That(Answer(srv, c, length).Status, Is.EqualTo(status), "Content-Length: " + (length.Length > 40 ? "5000 digits" : length));
            }
            Assert.That(calls, Is.EqualTo(new[] { Tx, Tx, Tx }));
        }
    }
}
