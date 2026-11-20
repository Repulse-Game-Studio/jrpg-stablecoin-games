// No Python twin: the fakes of WorkerTests, twins of the FakeEngine, fakeTimer and deferred() helpers of
// rpgmaker/src/core/worker.test.ts and of godot/tests/test_worker.gd's FakeEngine, FakeTimer and Gate, plus
// HeldTransport (test_pump_worker.py's FakeBridge).
using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Scfg.Core;

namespace Scfg.Core.Tests
{
    /// <summary>
    /// A manual <see cref="IScheduler"/>: <see cref="Schedule"/> records <c>(fn, seconds)</c>, <see cref="Fire"/> runs
    /// the latest live callback (the worker keeps at most one timer) and <see cref="RunDue"/> every live one.
    /// </summary>
    public sealed class FakeScheduler : IScheduler
    {
        public sealed class Entry
        {
            public Action Fn { get; }
            public double Seconds { get; }
            public bool Live { get; set; } = true;
            public Entry(Action fn, double seconds) { Fn = fn; Seconds = seconds; }
        }

        public readonly List<Entry> Entries = new List<Entry>();
        public int CancelCount;

        /// <summary>The seconds of every Schedule call, in order (worker.test.ts's <c>scheduled</c>).</summary>
        public List<double> Scheduled => Entries.Select(e => e.Seconds).ToList();

        /// <summary>The live timer the worker set last, or null (worker.test.ts's <c>pending</c>).</summary>
        public Entry? Pending => Entries.LastOrDefault(e => e.Live);

        public object Schedule(Action fn, double seconds)
        {
            var e = new Entry(fn, seconds);
            Entries.Add(e);
            return e;
        }

        public void Cancel(object handle)
        {
            CancelCount++;
            ((Entry)handle).Live = false;
        }

        /// <summary>Runs the pending callback, if any (it is spent first, as a fired timer is).</summary>
        public void Fire()
        {
            var p = Pending;
            if (p == null) return;
            p.Live = false;
            p.Fn();
        }

        /// <summary>Fires every live timer scheduled so far; returns how many fired.</summary>
        public int RunDue()
        {
            var due = Entries.Where(e => e.Live).ToList();
            foreach (var e in due)
            {
                e.Live = false;
                e.Fn();
            }
            return due.Count;
        }
    }

    /// <summary>An awaitable latch (worker.test.ts's <c>deferred()</c>, test_worker.gd's Gate).</summary>
    public sealed class Gate
    {
        private readonly TaskCompletionSource<object?> _tcs = new TaskCompletionSource<object?>();
        public Task Wait() => _tcs.Task;
        public void Open() => _tcs.TrySetResult(null);
        public void Fail(Exception e) => _tcs.TrySetException(e);
    }

    /// <summary>
    /// The Engine stand-in of worker.test.ts: counts ticks and reverifies, logs them, and lets a test replace the tick
    /// (<see cref="TickImpl"/>) or the reverify. The C# Engine is a sealed class, so the worker reaches these through
    /// its internal hooks; the commands still get a real <see cref="Scfg.Core.Engine"/> (over a transport with no
    /// endpoints), and read the fake's counters through their closures.
    /// </summary>
    public sealed class FakeEngine
    {
        public int Ticks;
        public int Reverified;
        public List<string> Statuses = new List<string> { "waiting" };
        public Func<Task<List<StepOutcome>>>? TickImpl;
        public Func<Task<List<string>>>? ReverifyImpl;
        public readonly List<string> Log = new List<string>();
        public readonly Engine Engine;

        /// <param name="engine">The real Engine the commands get; default <see cref="WorkerFixtures.IdleEngine"/>.</param>
        public FakeEngine(Engine? engine = null)
        {
            Engine = engine ?? WorkerFixtures.IdleEngine();
        }

        /// <summary>One RPC call through the Engine's own transport (refused as the worker says), on Base Sepolia.</summary>
        public Task<object?> Call(string method) => ((RpcClient)Engine.Rpc("base")).Call(method, new List<object?>());

        public async Task<List<StepOutcome>> Tick()
        {
            Ticks++;
            Log.Add("tick");
            if (TickImpl != null) return await TickImpl();
            return Statuses.Select(s => new StepOutcome("ps_1", s)).ToList();
        }

        public async Task<List<string>> Reverify()
        {
            Reverified++;
            Log.Add("reverify");
            if (ReverifyImpl != null) return await ReverifyImpl();
            return new List<string>();
        }

        public Snapshot Snapshot() =>
            new Snapshot(new JsonObject(), new JsonObject(), new[] { "tick" + Ticks.ToString(System.Globalization.CultureInfo.InvariantCulture) },
                         Dec.FromInt(0), Array.Empty<(string, string)>());

        public Worker Worker(IScheduler? scheduler = null, Func<double>? interval = null, Action<Exception>? onError = null) =>
            new Worker(Engine, scheduler ?? new FakeScheduler(), interval, onError, Tick, Reverify, Snapshot);
    }

    /// <summary>
    /// A transport that keeps every request in flight until the test answers it (test_pump_worker.py's FakeBridge):
    /// <see cref="Answer"/> completes request <c>i</c> (in the order sent), and a request the Engine cancels
    /// (<see cref="ICancellableTransport"/>) is recorded in <see cref="Cancelled"/>, its answer left pending.
    /// </summary>
    public sealed class HeldTransport : ICancellableTransport
    {
        public readonly List<(string Url, string Method, TaskCompletionSource<object?> Answer)> Sent =
            new List<(string, string, TaskCompletionSource<object?>)>();
        public readonly List<int> Cancelled = new List<int>();

        public Task<object?> PostJson(string url, JsonObject payload, double timeout) =>
            PostJson(url, payload, timeout, CancellationToken.None);

        public Task<object?> PostJson(string url, JsonObject payload, double timeout, CancellationToken abandon)
        {
            int i = Sent.Count;
            var answer = new TaskCompletionSource<object?>();
            Sent.Add((url, (string)payload["method"]!, answer));
            abandon.Register(() => Cancelled.Add(i));
            return answer.Task;
        }

        public IEnumerable<string> Methods => Sent.Select(s => s.Method);

        public void Answer(int i, object? result) => Sent[i].Answer.SetResult(Fakes.Ok(result));
    }

    public static class WorkerFixtures
    {
        public const string Wallet = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed";

        public static ConfigData BaseConfig()
        {
            var config = Config.BuildConfig(new JsonObject
            {
                { "wallet", Wallet }, { "chains", new List<object?> { "base" } }, { "testnet", true },
            });
            Config.AddItem(config, "chapter2", new JsonObject { { "code", 37L }, { "price", "4.99" }, { "token", "USDC" }, { "title", "Chapter 2" } });
            return config;
        }

        /// <summary>A real Engine whose transport knows no endpoint (the fake engine's commands never reach it).</summary>
        public static Engine IdleEngine() =>
            new Engine(BaseConfig(), new FakeTransport(new Dictionary<string, Handler>()), null, null, () => 1700000000);

        /// <summary>The two draws of one EVM StartSession (EngineTests' Draws).</summary>
        public static ScriptedEntropy Draws() => new ScriptedEntropy(new List<object?>
        {
            new JsonObject { { "fn", "randbelow" }, { "n", 100L }, { "value", 6L } },
            new JsonObject { { "fn", "token_hex" }, { "n", 8L }, { "value", "ea4702a60e526f21" } },
        });

        /// <summary>An EVM node at block 1000 with no transfers: what a StartSession and an idle tick ask for.</summary>
        public static object? IdleNode(string method, List<object?> prms) => method switch
        {
            "eth_blockNumber" => Fakes.Ok("0x3e8"),
            "eth_getLogs" => Fakes.Ok(new List<object?>()),
            _ => throw new InvalidOperationException("unexpected RPC method " + method),
        };
    }
}
