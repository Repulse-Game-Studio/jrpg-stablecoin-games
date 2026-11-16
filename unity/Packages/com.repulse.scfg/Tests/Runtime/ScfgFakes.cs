// The Unity tests' fakes, twins of godot/tests/test_scfg_flows.gd's FakeHost / FakeSheet / fake platform and of
// unity/core-tests/CheckoutFakes.cs's FakeEvmNode (an in-memory EVM JSON-RPC node behind ITransport), plus ScfgRig, which
// resets the facade and points it at a temporary state file, a fake clock and these fakes.
#nullable enable
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Numerics;
using System.Threading.Tasks;
using Scfg.Core;

namespace Scfg.Tests
{
    /// <summary>An in-memory EVM node (see unity/core-tests/CheckoutFakes.cs).</summary>
    public sealed class FakeEvmNode : ITransport
    {
        public long Head;
        public readonly List<JsonObject> Logs = new List<JsonObject>();
        public readonly Dictionary<string, JsonObject> Receipts = new Dictionary<string, JsonObject>(StringComparer.Ordinal);
        public bool Down;
        public readonly List<string> Calls = new List<string>();
        /// <summary>When set, every answer waits for <see cref="Release"/> (a request in flight).</summary>
        public bool Hold;
        private readonly List<(TaskCompletionSource<object?> Tcs, Func<object?> Answer)> _held =
            new List<(TaskCompletionSource<object?>, Func<object?>)>();

        public FakeEvmNode(long head = 1000) { Head = head; }

        public static string Hex64(BigInteger n)
        {
            var s = n.ToString("x64", CultureInfo.InvariantCulture);
            return s.Substring(s.Length - 64);
        }

        public static long H(object? x) => (long)Rpc.HexInt(x);

        public Task<object?> PostJson(string url, JsonObject payload, double timeout)
        {
            var method = (string)payload["method"]!;
            var prms = (List<object?>)payload["params"]!;
            Calls.Add(method);
            if (Down) return Task.FromException<object?>(new TransportError("fake outage"));
            Func<object?> answer = () => new JsonObject { { "jsonrpc", "2.0" }, { "id", payload["id"] }, { "result", Answer(method, prms) } };
            if (Hold)
            {
                var tcs = new TaskCompletionSource<object?>();
                _held.Add((tcs, answer));
                return tcs.Task;
            }
            try
            {
                return Task.FromResult(answer());
            }
            catch (Exception e)
            {
                return Task.FromException<object?>(e);
            }
        }

        public int Held => _held.Count;

        public void Release()
        {
            var held = _held.ToList();
            _held.Clear();
            foreach (var (tcs, answer) in held) tcs.SetResult(answer());
        }

        private object? Answer(string method, List<object?> prms)
        {
            switch (method)
            {
                case "eth_blockNumber":
                    return Rpc.Hex(Head);
                case "eth_getBlockByNumber":
                {
                    long n = H(prms[0]);
                    return n <= Head ? new JsonObject { { "hash", "0x" + Hex64(n) }, { "number", Rpc.Hex(n) } } : null;
                }
                case "eth_getTransactionReceipt":
                    return Receipts.TryGetValue((string)prms[0]!, out var r) ? r : null;
                case "eth_getLogs":
                {
                    var filter = (JsonObject)prms[0]!;
                    long from = H(filter["fromBlock"]), to = H(filter["toBlock"]);
                    var address = ((string)filter["address"]!).ToLowerInvariant();
                    return Logs.Where(lg =>
                    {
                        long b = H(lg["blockNumber"]);
                        return from <= b && b <= to && ((string)lg["address"]!).ToLowerInvariant() == address;
                    }).Cast<object?>().ToList();
                }
                default:
                    throw new InvalidOperationException("unexpected RPC method " + method);
            }
        }

        public static JsonObject TransferLog(string token, string sender, string recipient, BigInteger value, long block,
                                             string txHash, long logIndex = 0) => new JsonObject
        {
            { "address", token.ToLowerInvariant() },
            { "topics", new List<object?> { Evm.TransferTopic, Evm.AddressTopic(sender), Evm.AddressTopic(recipient) } },
            { "data", "0x" + Hex64(value) },
            { "transactionHash", txHash },
            { "logIndex", Rpc.Hex(logIndex) },
            { "blockNumber", Rpc.Hex(block) },
            { "blockHash", "0x" + Hex64(block) },
        };
    }

    /// <summary>A screen the test closes by calling <see cref="Close"/>, or destroys with <see cref="Destroy"/>.</summary>
    public sealed class FakeScreen : IScfgScreen
    {
        public event Action<string>? Closed;
        public bool Gone { get; set; }
        public object Controller;
        public readonly string Kind;

        public FakeScreen(string kind, object controller)
        {
            Kind = kind;
            Controller = controller;
        }

        public void Close(string how) => Closed?.Invoke(how);
        public void Destroy() => Gone = true;
        public ScfgCheckoutController Checkout => (ScfgCheckoutController)Controller;
        public ScfgRestoreController Restore => (ScfgRestoreController)Controller;
    }

    /// <summary>The sheet host the flows show their screens through, recording what they ask for.</summary>
    public sealed class FakeHost : IScfgSheetHost
    {
        public readonly List<FakeScreen> Screens = new List<FakeScreen>();
        public readonly List<FakeScreen> ClosedScreens = new List<FakeScreen>();
        public readonly List<string> Notes = new List<string>();
        public readonly List<string> Journal = new List<string>();
        public bool Refuse;

        public IScfgScreen? OpenPayment(ScfgCheckoutController controller) => Open("payment", controller);
        public IScfgScreen? OpenRestore(ScfgRestoreController controller) => Open("restore", controller);

        private IScfgScreen? Open(string kind, object controller)
        {
            if (Refuse) return null;
            var s = new FakeScreen(kind, controller);
            Screens.Add(s);
            Journal.Add("open " + kind);
            return s;
        }

        public void Close(IScfgScreen screen)
        {
            ClosedScreens.Add((FakeScreen)screen);
            Journal.Add("close screen");
        }

        public void Notify(string text) => Notes.Add(text);

        public FakeScreen Last => Screens[Screens.Count - 1];
    }

    public sealed class FakePlatform : IScfgPlatform
    {
        public bool IsWeb { get; set; }
        public bool IsMobile { get; set; }
        public readonly List<string> Opened = new List<string>();
        public string Clipboard = "";
        public void OpenUrl(string url) => Opened.Add(url);
        public void ClipboardSet(string text) => Clipboard = text;
        public string ClipboardGet() => Clipboard;
    }

    /// <summary>A browser page that records what the controller asks of it.</summary>
    public sealed class RecordingServer : ICheckoutServer
    {
        public const string PageUrl = "http://127.0.0.1:1234/c/secret/";
        public readonly ServerArgs Args;
        public readonly List<string> Journal;
        public bool Running { get; private set; }
        public int Pumps;
        public RecordingServer(ServerArgs args, List<string> journal)
        {
            Args = args;
            Journal = journal;
        }
        public void Start() { Running = true; Journal.Add("server start"); }
        public void Stop() { if (Running) Journal.Add("server stop"); Running = false; }
        public string Url => PageUrl;
        public void Reopen() => Journal.Add("server reopen");
        public void Pump() => Pumps++;
    }

    /// <summary>
    /// The facade reset and pointed at fakes: a state file in a temporary directory, a manual monotonic clock driving
    /// its UnityScheduler, a wall clock, a FakeEvmNode transport, a FakeHost and a FakePlatform; reports recorded
    /// (quiet), the development-build lines on.
    /// </summary>
    public sealed class ScfgRig : IDisposable
    {
        public const string Dev = "0x52908400098527886E0F7030069857D2E4169EE7";
        public const string Payer = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed";
        public static readonly string Tx = "0x" + string.Concat(Enumerable.Repeat("ab", 32));
        public static string UsdcArb => Chains.Mainnet["arbitrum"].Tokens["USDC"].Address;

        public readonly string Dir;
        public readonly FakeEvmNode Node = new FakeEvmNode();
        public readonly FakeHost Host = new FakeHost();
        public readonly FakePlatform Platform = new FakePlatform();
        public readonly List<string> Journal = new List<string>();
        public readonly List<RecordingServer> Servers = new List<RecordingServer>();
        public double Now = 1000;
        public long Wall = 1_800_000_000;
        public string StorePath => Path.Combine(Dir, "scfg", "state.json");

        public ScfgRig(bool fakeServers = true)
        {
            Dir = Path.Combine(Path.GetTempPath(), "scfg-unity-tests", Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(Dir);
            Restart(fakeServers);
        }

        /// <summary>A fresh facade over the same state file (a new launch of the game).</summary>
        public void Restart(bool fakeServers = true)
        {
            SCFG.ResetStatics();
            var s = SCFG.Current;
            s.Quiet = true;
            s.DevBuild = true;
            s.StorePath = StorePath;
            s.Clock = () => Now;
            s.WallClock = () => Wall;
            s.Transport = Node;
            s.OwnScheduler = new UnityScheduler(() => Now, e => SCFG.ReportError("worker error:", e));
            s.Scheduler = s.OwnScheduler;
            s.Platform = Platform;
            s.SheetHost = Host;
            if (fakeServers)
                s.ServerFactory = args =>
                {
                    var server = new RecordingServer(args, Journal);
                    Servers.Add(server);
                    return server;
                };
        }

        public List<string> Reported => SCFG.Current.Reported;
        public List<string> Warnings => SCFG.Current.Warnings;

        /// <summary>The usual configuration: Dev on Arbitrum (mainnet), chapter2 at 4.99 USDC.</summary>
        public void Configure(ScfgMethods? methods = null)
        {
            var err = SCFG.Configure(new ScfgOptions { Wallet = Dev, Chains = new List<string> { "arbitrum" }, Methods = methods });
            if (err != null) throw new InvalidOperationException("configure: " + err + " " + string.Join("; ", Reported));
            err = SCFG.AddItem("chapter2", new ScfgItem { Code = 1, Price = "4.99", Token = "USDC", Title = "Chapter 2" });
            if (err != null) throw new InvalidOperationException("add_item: " + err);
        }

        /// <summary><paramref name="frames"/> frames of the pump, the clock moving by <paramref name="step"/> each.</summary>
        public void Pump(int frames = 1, double step = 0)
        {
            for (int i = 0; i < frames; i++)
            {
                Now += step;
                SCFG.Pump();
            }
        }

        /// <summary>Lets the Worker run every pass that is due now (a few frames).</summary>
        public void Settle() => Pump(4);

        /// <summary>Moves past the Worker's interval and lets it run a timer-driven pass.</summary>
        public void Tick() => Pump(3, 16);

        /// <summary>
        /// A transfer of the open session's exact units to Dev, mined in the next block (above what the watcher scanned),
        /// with the head moved <paramref name="confirmations"/> blocks past it.
        /// </summary>
        public void PaySession(CheckoutController ctl, long confirmations = 100, string? tx = null)
        {
            long block = Node.Head + 1;
            Node.Logs.Add(FakeEvmNode.TransferLog(UsdcArb, Payer, Dev, Ints.IntFrom(ctl.Session!["units"]), block, tx ?? Tx));
            Node.Head = block + confirmations;
        }

        public void Dispose()
        {
            SCFG.ResetStatics();
            try
            {
                Directory.Delete(Dir, true);
            }
            catch (IOException)
            {
            }
            catch (UnauthorizedAccessException)
            {
            }
        }
    }
}
