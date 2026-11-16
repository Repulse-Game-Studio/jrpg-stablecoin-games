// No Python twin: the fakes of the checkout tests, twins of rpgmaker/test/checkout_fakes.ts (immediateSubmitter,
// DeferredSubmitter, FakeServer, PageWithPump), of tests/unit/fakes.py's immediate_submitter and of the FakeChain /
// FakeSolanaChain of rpgmaker/test/chain_fakes.ts and solana_fakes.ts. The C# Engine's RPC objects are concrete
// (RpcClient, SolanaRpc), so the fake chains are JSON-RPC nodes behind ITransport instead of RPC stand-ins.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Numerics;
using System.Threading.Tasks;
using Scfg.Core;
using Scfg.Core.Solana;

namespace Scfg.Core.Tests
{
    /// <summary>rpc_fakes.ts's FakeClock: whole seconds, set and advanced by the test.</summary>
    public sealed class FakeClock
    {
        public long Now;
        public FakeClock(long now = 100) { Now = now; }
        public long Call() => Now;
        public void Advance(long seconds) => Now += seconds;
    }

    /// <summary>
    /// An in-memory EVM node (chain_fakes.ts's FakeChain), answering the JSON-RPC calls a session start and the
    /// watcher make: <c>Head</c>, <c>Hashes</c> (block -&gt; hash; unlisted blocks get <c>0x</c> + the block number as 64
    /// hex digits), <c>Logs</c> (raw logs eth_getLogs filters by range and address), <c>Receipts</c> (tx -&gt; receipt) and
    /// <c>Down</c> (every call is a TransportError, so the RpcClient raises RpcUnavailable).
    /// </summary>
    public sealed class FakeEvmNode : ITransport
    {
        public long Head;
        public readonly Dictionary<long, string> Hashes = new Dictionary<long, string>();
        public readonly List<JsonObject> Logs = new List<JsonObject>();
        public readonly Dictionary<string, JsonObject> Receipts = new Dictionary<string, JsonObject>(StringComparer.Ordinal);
        public bool Down;
        public readonly List<string> Calls = new List<string>();

        public FakeEvmNode(long head = 1000) { Head = head; }

        /// <summary>A non-negative value as 64 hex digits (BigInteger's "x" may add a leading sign digit).</summary>
        public static string Hex64(BigInteger n)
        {
            var s = n.ToString("x64", CultureInfo.InvariantCulture);
            return s.Substring(s.Length - 64);
        }

        public string BlockHash(long n) => Hashes.TryGetValue(n, out var h) ? h : "0x" + Hex64(n);

        public Task<object?> PostJson(string url, JsonObject payload, double timeout)
        {
            var method = (string)payload["method"]!;
            var prms = (List<object?>)payload["params"]!;
            Calls.Add(method);
            if (Down) return Task.FromException<object?>(new TransportError("fake outage"));
            try
            {
                return Task.FromResult<object?>(Fakes.Ok(Answer(method, prms), (long)payload["id"]!));
            }
            catch (Exception e)
            {
                return Task.FromException<object?>(e);
            }
        }

        private object? Answer(string method, List<object?> prms)
        {
            switch (method)
            {
                case "eth_blockNumber":
                    return Rpc.Hex(Head);
                case "eth_getBlockByNumber":
                {
                    long n = Fakes.H(prms[0]);
                    return n <= Head ? new JsonObject { { "hash", BlockHash(n) }, { "number", Rpc.Hex(n) } } : null;
                }
                case "eth_getTransactionReceipt":
                    return Receipts.TryGetValue((string)prms[0]!, out var r) ? r : null;
                case "eth_getLogs":
                {
                    var filter = (JsonObject)prms[0]!;
                    long from = Fakes.H(filter["fromBlock"]), to = Fakes.H(filter["toBlock"]);
                    var address = ((string)filter["address"]!).ToLowerInvariant();
                    return Logs.Where(lg =>
                    {
                        long b = Fakes.H(lg["blockNumber"]);
                        return from <= b && b <= to && ((string)lg["address"]!).ToLowerInvariant() == address;
                    }).Cast<object?>().ToList();
                }
                default:
                    throw new InvalidOperationException("unexpected RPC method " + method);
            }
        }

        /// <summary>chain_fakes.ts's transferLog: a raw ERC-20 Transfer log.</summary>
        public static JsonObject TransferLog(string token, string sender, string recipient, BigInteger value, long block,
                                             string txHash, long logIndex = 0, string? blockHash = null) => new JsonObject
        {
            { "address", token.ToLowerInvariant() },
            { "topics", new List<object?> { Evm.TransferTopic, Evm.AddressTopic(sender), Evm.AddressTopic(recipient) } },
            { "data", "0x" + Hex64(value) },
            { "transactionHash", txHash },
            { "logIndex", Rpc.Hex(logIndex) },
            { "blockNumber", Rpc.Hex(block) },
            { "blockHash", blockHash ?? "0x" + Hex64(block) },
        };
    }

    /// <summary>
    /// An in-memory Solana node (solana_fakes.ts's FakeSolanaChain), as far as a session start asks: <c>Slot</c>,
    /// <c>Addresses</c> (address -&gt; signature infos, newest first), <c>Accounts</c> and <c>TokenBalances</c>
    /// (getAccountInfo), <c>Lamports</c> (getBalance), <c>Blockhash</c>, no prioritization fees, and <c>Down</c>. Any
    /// other method fails the call.
    /// </summary>
    public sealed class FakeSolanaNode : ITransport
    {
        public long Slot;
        public readonly Dictionary<string, List<JsonObject>> Addresses = new Dictionary<string, List<JsonObject>>(StringComparer.Ordinal);
        public readonly HashSet<string> Accounts = new HashSet<string>(StringComparer.Ordinal);
        /// <summary>Token account -&gt; balance in base units (getAccountInfo, jsonParsed).</summary>
        public readonly Dictionary<string, long> TokenBalances = new Dictionary<string, long>(StringComparer.Ordinal);
        /// <summary>Address -&gt; lamports (getBalance; 0 when unlisted).</summary>
        public readonly Dictionary<string, long> Lamports = new Dictionary<string, long>(StringComparer.Ordinal);
        public string Blockhash = Base58.B58Encode(Enumerable.Repeat((byte)7, 32).ToArray());
        public bool Down;
        public readonly List<string> Calls = new List<string>();
        /// <summary>Every request, method and params.</summary>
        public readonly List<(string Method, List<object?> Params)> Requests = new List<(string, List<object?>)>();

        public FakeSolanaNode(long slot = 1000) { Slot = slot; }

        public Task<object?> PostJson(string url, JsonObject payload, double timeout)
        {
            var method = (string)payload["method"]!;
            var prms = (List<object?>)payload["params"]!;
            Calls.Add(method);
            Requests.Add((method, prms));
            if (Down) return Task.FromException<object?>(new TransportError("fake outage"));
            try
            {
                return Task.FromResult<object?>(Fakes.Ok(Answer(method, prms), (long)payload["id"]!));
            }
            catch (Exception e)
            {
                return Task.FromException<object?>(e);
            }
        }

        private object? Answer(string method, List<object?> prms)
        {
            switch (method)
            {
                case "getSlot":
                    return Slot;
                case "getSignaturesForAddress":
                {
                    var limit = (long)((JsonObject)prms[1]!)["limit"]!;
                    var list = Addresses.TryGetValue((string)prms[0]!, out var l) ? l : new List<JsonObject>();
                    return list.Take((int)limit).Cast<object?>().ToList();
                }
                case "getAccountInfo":
                {
                    var address = (string)prms[0]!;
                    object? value = null;
                    if (TokenBalances.TryGetValue(address, out var amount))
                    {
                        value = new JsonObject
                        {
                            { "lamports", 2039280L },
                            { "data", new JsonObject { { "parsed", new JsonObject { { "info", new JsonObject
                            {
                                { "tokenAmount", new JsonObject { { "amount", amount.ToString(CultureInfo.InvariantCulture) }, { "decimals", 6L } } },
                            } } } } } },
                        };
                    }
                    else if (Accounts.Contains(address))
                    {
                        value = new JsonObject { { "lamports", 1L } };
                    }
                    return new JsonObject { { "context", new JsonObject { { "slot", Slot } } }, { "value", value } };
                }
                case "getBalance":
                    return new JsonObject
                    {
                        { "context", new JsonObject { { "slot", Slot } } },
                        { "value", Lamports.TryGetValue((string)prms[0]!, out var lamports) ? lamports : 0L },
                    };
                case "getLatestBlockhash":
                    return new JsonObject
                    {
                        { "context", new JsonObject { { "slot", Slot } } },
                        { "value", new JsonObject { { "blockhash", Blockhash }, { "lastValidBlockHeight", Slot + 150 } } },
                    };
                case "getRecentPrioritizationFees":
                    return new List<object?>();
                default:
                    throw new InvalidOperationException("unexpected RPC method " + method);
            }
        }
    }

    /// <summary>
    /// The engine of a checkout test over fake nodes, one per chain (the TS setup's <c>fakes</c> map): a chain's node is
    /// made on first use, by the test or by the Engine's RPC factory, so a test can take a node down before the Engine
    /// first asks it. The RpcClients read a clock that jumps an hour on every read, so an outage leaves no cooldown
    /// behind for the next call (the TS FakeChain is the RPC object and has none).
    /// </summary>
    public sealed class CheckoutRig
    {
        public readonly ConfigData Config;
        public readonly FakeClock Clock;
        public readonly Engine Engine;
        public readonly Dictionary<string, FakeEvmNode> EvmNodes = new Dictionary<string, FakeEvmNode>(StringComparer.Ordinal);
        public readonly FakeSolanaNode Sol = new FakeSolanaNode(1000);
        private long _rpcNow;

        public CheckoutRig(ConfigData config, long now = 100)
        {
            Config = config;
            Clock = new FakeClock(now);
            Engine = new Engine(config, new FakeTransport(new Dictionary<string, Handler>()), null, null, Clock.Call, MakeRpc);
        }

        public FakeEvmNode Evm(string key)
        {
            if (!EvmNodes.TryGetValue(key, out var node)) EvmNodes[key] = node = new FakeEvmNode(1000);
            return node;
        }

        private long RpcClock() => _rpcNow += 3600;

        private object MakeRpc(Chain chain) => chain.Family == "solana"
            ? new SolanaRpc(new RpcClient(chain.Rpc, Sol, clock: RpcClock))
            : (object)new RpcClient(chain.Rpc, Evm(chain.Key), logRange: chain.MaxLogRange, clock: RpcClock);

        /// <summary>One tick, every outcome handed to the controller (checkout_steps.test.ts's deliver).</summary>
        public async Task Deliver(CheckoutController ctl)
        {
            foreach (var o in await Engine.Tick()) ctl.OnOutcome(o);
        }
    }

    /// <summary>
    /// Worker.Submit stand-in that runs each command at once (fakes.immediate_submitter, checkout_fakes.ts's
    /// immediateSubmitter). The fake nodes answer with completed tasks, so every job is done when Submit returns;
    /// <see cref="Idle"/> waits for any that is not, as the TS tests' <c>await submit.idle()</c>.
    /// </summary>
    public sealed class ImmediateSubmitter
    {
        private readonly Engine _engine;
        private readonly List<Task> _inflight = new List<Task>();
        public readonly List<Job> Jobs = new List<Job>();

        public ImmediateSubmitter(Engine engine) { _engine = engine; }

        public Job Submit(Func<Engine, Task<object?>> command)
        {
            var job = new Job();
            job.SetRunningOrNotifyCancel();
            Jobs.Add(job);
            Task<object?> task;
            try
            {
                task = command(_engine);
            }
            catch (Exception e)
            {
                job.SetException(e);
                return job;
            }
            _inflight.Add(Settle(task, job));
            return job;
        }

        private static async Task Settle(Task<object?> task, Job job)
        {
            try
            {
                job.SetResult(await task);
            }
            catch (Exception e)
            {
                job.SetException(e);
            }
        }

        public async Task Idle()
        {
            while (_inflight.Count > 0)
            {
                var batch = _inflight.ToList();
                _inflight.Clear();
                await Task.WhenAll(batch);
            }
        }
    }

    /// <summary>Worker.Submit stand-in that runs commands only when <see cref="Run"/> is called (DeferredSubmitter).</summary>
    public sealed class DeferredSubmitter
    {
        private readonly Engine _engine;
        public readonly List<(Func<Engine, Task<object?>> Fn, Job Job)> Queue = new List<(Func<Engine, Task<object?>>, Job)>();
        /// <summary>What the jobs' done callbacks threw (the controller's must throw nothing).</summary>
        public readonly List<Exception> CallbackErrors = new List<Exception>();

        public DeferredSubmitter(Engine engine) { _engine = engine; }

        public Job Call(Func<Engine, Task<object?>> command)
        {
            var job = new Job(CallbackErrors.Add);
            Queue.Add((command, job));
            return job;
        }

        /// <summary>Runs the queue until it is empty, commands queued meanwhile included (Python's run()).</summary>
        public async Task Run()
        {
            while (Queue.Count > 0)
            {
                var (fn, job) = Queue[0];
                Queue.RemoveAt(0);
                if (job.Done() || !job.SetRunningOrNotifyCancel()) continue;
                try
                {
                    job.SetResult(await fn(_engine));
                }
                catch (Exception e)
                {
                    job.SetException(e);
                }
            }
        }
    }

    /// <summary>The page a server factory returns in the tests (FakeServer, and PageWithPump's counters).</summary>
    public class FakeServer : ICheckoutServer
    {
        public const string PageUrl = "http://127.0.0.1:1234/c/secret/";

        public ServerArgs Args { get; }
        public JsonObject SessionInfo => Args.SessionInfo;
        public bool Started;
        public bool Stopped;
        public int Pumps;
        public int Reopens;

        public FakeServer(ServerArgs args) { Args = args; }

        public string Url => PageUrl;
        public bool Running { get; private set; }

        public virtual void Start()
        {
            Started = true;
            Running = true;
        }

        public void Stop()
        {
            Stopped = true;
            Running = false;
        }

        public void Reopen() => Reopens++;
        public void Pump() => Pumps++;
    }

    /// <summary>A page whose Start throws (no sockets): the factory returned it, Start failed.</summary>
    public sealed class FailingServer : FakeServer
    {
        public FailingServer(ServerArgs args) : base(args) { }
        public override void Start() => throw new System.Net.Sockets.SocketException(98);
    }
}
