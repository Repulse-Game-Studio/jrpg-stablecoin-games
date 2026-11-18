// Twin of godot/tests/test_http_transport.gd: UnityWebRequestTransport against a raw-socket HTTP server on 127.0.0.1
// (HttpListener is not in every Unity profile), answering canned JSON-RPC: ok, an HTTP error with and without a
// JSON-RPC body, non-JSON, oversized, no answer (timeout) and a refused connection. The server runs on a thread of the
// test, never in package code.
#nullable enable
using System;
using System.Collections;
using System.Collections.Generic;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using NUnit.Framework;
using Scfg.Core;
using UnityEngine.TestTools;

namespace Scfg.Tests
{
    /// <summary>A one-route HTTP/1.1 server: every request gets <see cref="Answer"/> (status, body), or nothing when Silent.</summary>
    public sealed class CannedServer : IDisposable
    {
        private readonly TcpListener _listener;
        private readonly Thread _thread;
        private volatile bool _stop;
        public volatile int Requests;
        public volatile string LastBody = "";
        public volatile string LastHead = "";
        public Func<(int Status, byte[] Body)> Answer = () => (200, Encoding.UTF8.GetBytes("{}"));
        public volatile bool Silent;
        /// <summary>When set, the answer claims this Content-Length but stops after its body, the connection held open.</summary>
        public volatile int PartialOf;

        public CannedServer()
        {
            _listener = new TcpListener(IPAddress.Loopback, 0);
            _listener.Start();
            _thread = new Thread(Loop) { IsBackground = true };
            _thread.Start();
        }

        public int Port => ((IPEndPoint)_listener.LocalEndpoint).Port;
        public string Url => "http://127.0.0.1:" + Port + "/rpc";

        private void Loop()
        {
            var held = new List<TcpClient>();
            while (!_stop)
            {
                if (!_listener.Pending())
                {
                    Thread.Sleep(5);
                    continue;
                }
                var client = _listener.AcceptTcpClient();
                try
                {
                    var stream = client.GetStream();
                    var head = ReadHead(stream);
                    LastHead = head;
                    int length = 0;
                    foreach (var line in head.Split(new[] { "\r\n" }, StringSplitOptions.None))
                        if (line.StartsWith("Content-Length:", StringComparison.OrdinalIgnoreCase))
                            length = int.Parse(line.Substring(15).Trim());
                    var body = new byte[length];
                    int got = 0;
                    while (got < length) got += stream.Read(body, got, length - got);
                    LastBody = Encoding.UTF8.GetString(body);
                    Requests++;
                    if (Silent)
                    {
                        held.Add(client); // never answered, never closed: the client times out
                        continue;
                    }
                    var (status, answer) = Answer();
                    var reply = Encoding.ASCII.GetBytes("HTTP/1.1 " + status + " X\r\nContent-Type: application/json\r\nContent-Length: "
                                                       + (PartialOf > 0 ? PartialOf : answer.Length) + "\r\nConnection: close\r\n\r\n");
                    stream.Write(reply, 0, reply.Length);
                    stream.Write(answer, 0, answer.Length);
                    stream.Flush();
                    if (PartialOf > 0) held.Add(client); // the rest never comes: only the pump ends the request
                }
                catch (Exception)
                {
                    // a client that went away
                }
                if (!held.Contains(client)) client.Close();
            }
            foreach (var c in held) c.Close();
        }

        private static string ReadHead(NetworkStream stream)
        {
            var sb = new StringBuilder();
            var one = new byte[1];
            while (!sb.ToString().EndsWith("\r\n\r\n", StringComparison.Ordinal))
            {
                if (stream.Read(one, 0, 1) <= 0) break;
                sb.Append((char)one[0]);
            }
            return sb.ToString();
        }

        public void Dispose()
        {
            _stop = true;
            _thread.Join(2000);
            _listener.Stop();
        }
    }

    public sealed class TransportTests
    {
        private static readonly JsonObject Payload = new JsonObject
        {
            { "jsonrpc", "2.0" }, { "id", 1L }, { "method", "eth_blockNumber" }, { "params", new List<object?>() },
        };

        /// <summary>Waits (frames, unscaled) for <paramref name="task"/>, at most <paramref name="seconds"/>.</summary>
        internal static IEnumerator Wait(Task task, double seconds = 10)
        {
            var until = DateTime.UtcNow.AddSeconds(seconds);
            while (!task.IsCompleted && DateTime.UtcNow < until) yield return null;
            Assert.That(task.IsCompleted, Is.True, "the request did not finish in time");
        }

        private static Exception? Error(Task task) => task.Exception?.InnerException;

        [UnityTest]
        public IEnumerator A200JsonAnswerIsTheParsedBody()
        {
            using var server = new CannedServer
            {
                Answer = () => (200, Encoding.UTF8.GetBytes("{\"jsonrpc\": \"2.0\", \"id\": 1, \"result\": \"0x10\"}")),
            };
            var transport = new UnityWebRequestTransport();
            var t = transport.PostJson(server.Url, Payload, 5);
            yield return Wait(t);
            var answer = (JsonObject)t.Result!;
            Assert.That(answer["result"], Is.EqualTo("0x10"));
            Assert.That(server.LastBody, Is.EqualTo(Json.Dumps(Payload)));
            Assert.That(server.LastHead, Does.Contain("Content-Type: application/json"));
            Assert.That(server.LastHead, Does.Contain("User-Agent: scfg/0.1"));
            Assert.That(transport.Posts, Is.EqualTo(1));
            Assert.That(transport.InFlight, Is.EqualTo(0));
        }

        [UnityTest]
        public IEnumerator AnHttpErrorIsATransportErrorWithItsStatus()
        {
            using var server = new CannedServer { Answer = () => (500, Encoding.UTF8.GetBytes("oops")) };
            var t = new UnityWebRequestTransport().PostJson(server.Url, Payload, 5);
            yield return Wait(t);
            var e = Error(t) as TransportError;
            Assert.That(e, Is.Not.Null, Error(t)?.ToString());
            Assert.That(e!.Status, Is.EqualTo(500));
        }

        [UnityTest]
        public IEnumerator AnHttpErrorWithAJsonRpcErrorBodyIsTheBody()
        {
            using var server = new CannedServer
            {
                Answer = () => (400, Encoding.UTF8.GetBytes("{\"jsonrpc\": \"2.0\", \"id\": 1, \"error\": {\"code\": -32602, \"message\": \"bad\"}}")),
            };
            var t = new UnityWebRequestTransport().PostJson(server.Url, Payload, 5);
            yield return Wait(t);
            Assert.That(Error(t), Is.Null, Error(t)?.ToString());
            Assert.That(((JsonObject)t.Result!)["error"], Is.Not.Null);
        }

        [UnityTest]
        public IEnumerator ANonJsonBodyIsAnInvalidJsonResponse()
        {
            using var server = new CannedServer { Answer = () => (200, Encoding.UTF8.GetBytes("<html>")) };
            var t = new UnityWebRequestTransport().PostJson(server.Url, Payload, 5);
            yield return Wait(t);
            Assert.That(Error(t), Is.TypeOf<TransportError>());
            Assert.That(Error(t)!.Message, Is.EqualTo(Coio.InvalidJson));
        }

        [UnityTest]
        public IEnumerator AResponseOverTheSizeLimitIsATransportError()
        {
            using var server = new CannedServer { Answer = () => (200, Encoding.UTF8.GetBytes("[" + new string('1', 50000) + "]")) };
            var transport = new UnityWebRequestTransport { BodySizeLimit = 4096 };
            var t = transport.PostJson(server.Url, Payload, 5);
            yield return Wait(t);
            Assert.That(Error(t), Is.TypeOf<TransportError>());
            Assert.That(Error(t)!.Message, Is.EqualTo(Coio.ResponseTooLarge));
        }

        /// <summary>WebGL's way (a refused chunk does not stop the browser): the pump aborts a download past the cap.</summary>
        [UnityTest]
        public IEnumerator ThePumpAbortsADownloadPastTheCap()
        {
            using var server = new CannedServer { Answer = () => (200, new byte[8192]), PartialOf = 1 << 20 };
            var transport = new UnityWebRequestTransport();
            var t = transport.PostJson(server.Url, Payload, 30);
            yield return Downloaded(transport, 8192); // the first 8 KiB arrive
            transport.Pump();
            Assert.That(t.IsCompleted, Is.False, "under the cap: still downloading");
            transport.BodySizeLimit = 4096;
            transport.Pump();
            Assert.That(t.IsCompleted, Is.True, "aborted at once");
            Assert.That(Error(t), Is.TypeOf<TransportError>());
            Assert.That(Error(t)!.Message, Is.EqualTo(Coio.ResponseTooLarge));
            Assert.That(transport.InFlight, Is.EqualTo(0));
            transport.Pump(); // nothing left to do
        }

        /// <summary>
        /// The facade's half of the WebGL cap: SCFG.Pump pumps the transport it made (no test calls transport.Pump here),
        /// configured, so the whole pump runs.
        /// </summary>
        [UnityTest]
        public IEnumerator TheFacadesPumpAbortsItsOwnTransportsDownloadPastTheCap()
        {
            using var rig = new ScfgRig();
            rig.Configure();
            using var server = new CannedServer { Answer = () => (200, new byte[8192]), PartialOf = 1 << 20 };
            var transport = new UnityWebRequestTransport();
            SCFG.Current.OwnTransport = transport;
            var t = transport.PostJson(server.Url, Payload, 30);
            yield return Downloaded(transport, 8192);
            rig.Pump();
            Assert.That(t.IsCompleted, Is.False, "under the cap: still downloading");
            transport.BodySizeLimit = 4096;
            rig.Pump();
            Assert.That(SCFG.Current.Worker, Is.Not.Null, "the pump ran in full");
            Assert.That(t.IsCompleted, Is.True, "SCFG.Pump aborted it");
            Assert.That(Error(t), Is.TypeOf<TransportError>());
            Assert.That(Error(t)!.Message, Is.EqualTo(Coio.ResponseTooLarge));
            Assert.That(transport.InFlight, Is.EqualTo(0));
        }

        /// <summary>Waits (frames, at most 10 s) until a request of <paramref name="transport"/> has <paramref name="bytes"/>.</summary>
        private static IEnumerator Downloaded(UnityWebRequestTransport transport, ulong bytes)
        {
            var until = DateTime.UtcNow.AddSeconds(10);
            while (transport.MostDownloaded() < bytes && DateTime.UtcNow < until) yield return null;
            Assert.That(transport.MostDownloaded(), Is.GreaterThanOrEqualTo(bytes), "the download did not start in time");
        }

        [UnityTest]
        public IEnumerator NoAnswerIsATimeout()
        {
            using var server = new CannedServer { Silent = true };
            var t = new UnityWebRequestTransport().PostJson(server.Url, Payload, 0.5);
            yield return Wait(t, 15);
            Assert.That(Error(t), Is.TypeOf<TransportError>(), Error(t)?.ToString());
            Assert.That(((TransportError)Error(t)!).Status, Is.Null);
            Assert.That(Error(t)!.Message, Does.Contain("timed out"));
        }

        // U12 of the Unity testbed: UnityWebRequest's timeout leaves the name lookup out, so a DNS server that never
        // answered held a request about 20 s. The transport's own deadline (Pump) ends it at its timeout whatever it
        // waits on: here an address that never resolves nor connects, with the deadline's clock moved past 10 s.
        [UnityTest]
        public IEnumerator ThePumpEndsARequestAtItsTimeoutWhateverItWaitsOn()
        {
            double now = 1000;
            var transport = new UnityWebRequestTransport { Clock = () => now };
            using var server = new CannedServer { Silent = true }; // accepts, never answers: no UnityWebRequest timeout yet
            var t = transport.PostJson(server.Url, Payload, 10);
            yield return null;
            transport.Pump();
            Assert.That(t.IsCompleted, Is.False, "before its deadline");
            now += 9.9;
            transport.Pump();
            Assert.That(t.IsCompleted, Is.False);
            var real = DateTime.UtcNow;
            now += 0.2;
            transport.Pump();
            Assert.That(t.IsCompleted, Is.True, "at its deadline, at once");
            Assert.That((DateTime.UtcNow - real).TotalSeconds, Is.LessThan(1));
            Assert.That(Error(t), Is.TypeOf<TransportError>(), Error(t)?.ToString());
            Assert.That(Error(t)!.Message, Is.EqualTo("timed out after 10s"));
            Assert.That(transport.InFlight, Is.EqualTo(0));
        }

        // The same for a name that cannot be looked up, through the RPC client: the first endpoint ends at its 10 s and the
        // second at the 5 s left of the call's 15 s budget (Rpc.CallBudget), not at the resolver's own timeout each.
        [UnityTest]
        public IEnumerator AnUnresolvableRpcIsANetworkErrorAfterItsTimeoutPerEndpoint()
        {
            double now = 1000;
            var transport = new UnityWebRequestTransport { Clock = () => now };
            var rpc = new RpcClient(new List<string> { "https://scfg-never-resolves.invalid/a", "https://scfg-never-resolves.invalid/b" },
                                    transport, clock: () => (long)now);
            var call = rpc.Call("eth_blockNumber", new List<object?>());
            for (int endpoint = 0; endpoint < 2 && !call.IsCompleted; endpoint++)
            {
                yield return null;
                now += 10.1;
                transport.Pump();
                yield return null;
            }
            yield return Wait(call, 5);
            Assert.That(Error(call), Is.TypeOf<RpcUnavailable>(), Error(call)?.ToString());
        }

        [UnityTest]
        public IEnumerator ARefusedConnectionIsATransportError()
        {
            int port;
            using (var probe = new CannedServer()) port = probe.Port; // closed again: nothing listens there now
            var t = new UnityWebRequestTransport().PostJson("http://127.0.0.1:" + port + "/rpc", Payload, 5);
            yield return Wait(t);
            Assert.That(Error(t), Is.TypeOf<TransportError>(), Error(t)?.ToString());
            Assert.That(((TransportError)Error(t)!).Status, Is.Null);
        }

        [UnityTest]
        public IEnumerator CloseEndsRequestsInFlightWithHostGone()
        {
            using var server = new CannedServer { Silent = true };
            var transport = new UnityWebRequestTransport();
            var t = transport.PostJson(server.Url, Payload, 30);
            yield return null;
            transport.Close();
            Assert.That(t.IsCompleted, Is.True);
            Assert.That(Error(t), Is.TypeOf<HostGone>());
            Assert.That(transport.InFlight, Is.EqualTo(0));
            var later = transport.PostJson(server.Url, Payload, 5);
            Assert.That(Error(later), Is.TypeOf<HostGone>());
        }

        // Testbed U15: a request nobody waits for any more (its job abandoned, its tick yielding to a command) is aborted
        // when the Engine's AbandonInFlight token is cancelled, not left to run to its timeout.
        [UnityTest]
        public IEnumerator ACancelledTokenAbortsTheRequestInFlight()
        {
            using var server = new CannedServer { Silent = true };
            var transport = new UnityWebRequestTransport();
            var abandon = new CancellationTokenSource();
            var t = transport.PostJson(server.Url, Payload, 30, abandon.Token);
            yield return null;
            Assert.That(t.IsCompleted, Is.False);
            Assert.That(transport.InFlight, Is.EqualTo(1));
            abandon.Cancel();
            Assert.That(t.IsCompleted, Is.True, "aborted at once");
            Assert.That(Error(t), Is.TypeOf<Abandoned>(), Error(t)?.ToString());
            Assert.That(transport.InFlight, Is.EqualTo(0));
        }

        [UnityTest]
        public IEnumerator TheRpcClientWorksOverHttp()
        {
            using var server = new CannedServer
            {
                Answer = () => (200, Encoding.UTF8.GetBytes("{\"jsonrpc\": \"2.0\", \"id\": 1, \"result\": \"0x2a\"}")),
            };
            var rpc = new RpcClient(new[] { server.Url }, new UnityWebRequestTransport());
            var t = rpc.BlockNumber();
            yield return Wait(t);
            Assert.That(t.Result, Is.EqualTo(42L));
        }

        [Test]
        public void TimeoutsAreWholeSecondsRoundedUp()
        {
            Assert.That(UnityWebRequestTransport.WholeSeconds(0.2), Is.EqualTo(1));
            Assert.That(UnityWebRequestTransport.WholeSeconds(1.0), Is.EqualTo(1));
            Assert.That(UnityWebRequestTransport.WholeSeconds(1.01), Is.EqualTo(2));
            Assert.That(UnityWebRequestTransport.WholeSeconds(30), Is.EqualTo(30));
            Assert.That(UnityWebRequestTransport.WholeSeconds(double.NaN), Is.EqualTo(1));
        }
    }
}
