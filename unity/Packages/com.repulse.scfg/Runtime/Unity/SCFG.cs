// Stablecoin for Games (SCFG): USDC/USDT tips and unlock purchases for Unity, paid straight to the developer's wallet.
// No server, no accounts. This is the static facade, the Unity twin of godot/addons/scfg/scfg.gd (where this file is
// silent, the Ren'Py glue plugin/game/scfg/scfg_api.rpy is the reference):
//
//     SCFG.AddChain("myl2", new ScfgChain { ... });            // optional: your own EVM chain, before Configure
//     SCFG.Configure(new ScfgOptions { Wallet = "0x...", Testnet = true });
//     SCFG.AddItem("chapter2", new ScfgItem { Code = 1, Price = "4.99", Token = "USDC", Title = "Chapter 2" });
//     if (SCFG.Owns("chapter2")) ...
//     ScfgPaymentResult r = await SCFG.Pay(ScfgPayKind.Unlock, new ScfgPayRequest { Item = "chapter2" });
//     ScfgRestoreResult restored = await SCFG.Restore();
//
// The class is SCFG, not Scfg: C# cannot have a type Scfg next to the namespace Scfg (Scfg.Core, Scfg.Desktop).
//
// DEVELOPER ERRORS. Misuse never throws: it logs "scfg: <message>" with Debug.LogError, and a call that returns a
// result resolves Failed with the ConfigError code (the exception's class name, such as "TypeError", for an error
// without one) as Error. Configure, AddChain and AddItem return that code, or null. Advice (a token the wallet cannot
// receive yet, a corrupt state file moved aside) is a Debug.LogWarning in development builds only.
//
// STATE. Receipts and pending payments live in Application.persistentDataPath/scfg/state.json (ScfgStore), global to the
// install, never inside a save slot. It is read on first use. The Engine and the Worker start on the first frame after
// Configure (EnsureStarted, from the runner's pump); the Worker's timer is a UnityScheduler on unscaled real time, so
// payments keep being watched while the game is paused or at Time.timeScale = 0.
//
// THE PUMP (Pump, every frame from the hidden ScfgRunner), in Ren'Py's _periodic order: the scheduler (Worker passes);
// the screens closed since the last frame; drain the Worker's outcomes, give each to every open and background checkout
// controller and deliver its result; Poll() every controller (which pumps its localhost page) and register its callback;
// close background controllers whose session is final or older than the browser timeout; poll the restore controllers
// and deliver a finished one; warn once about unready routes; then mirror a new snapshot to disk.
//
// PERSISTENCE (Sync): receipt changes, sessions added or removed, and a session's state or hint_tx changing are written
// at once; other pending changes (scan progress) at most every PendingSaveInterval seconds, and once more at quit. A
// failed save is retried at that interval and reported once until a save succeeds. A state file that exists but cannot
// be read starts no payments this run and is never saved over.
//
// DOMAIN RELOAD OFF. Every static of this assembly lives in one State object; ResetStatics (SubsystemRegistration)
// closes what the last play session left open and starts from a fresh State, events included.
#nullable enable
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using Scfg.Core;
using UnityEngine;

namespace Scfg
{
    /// <summary>The SCFG facade (see the file header).</summary>
    public static partial class SCFG
    {
        public const string StateFileName = "state.json";
        public const double PendingSaveInterval = 10.0;
        /// <summary>Characters of a transaction hash (hex) or a Solana signature (base58).</summary>
        public const string TxChars = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
        /// <summary>The most characters a hash field takes (a pasted clipboard is cut there too).</summary>
        public const int TxMaxLength = 90;
        internal const double PollOpenS = 3.0;
        internal const double PollIdleS = 15.0;

        /// <summary>A final payment result that has no Pay callback (Ren'Py's on_result).</summary>
        public static event Action<ScfgPaymentResult>? PaymentResolved
        {
            add => S.PaymentResolved += value;
            remove => S.PaymentResolved -= value;
        }

        /// <summary>The stored receipts changed (a payment confirmed, a restore, ClearLocalData).</summary>
        public static event Action? ReceiptsChanged
        {
            add => S.ReceiptsChanged += value;
            remove => S.ReceiptsChanged -= value;
        }

        /// <summary>Show a short notification (through the sheet host) when a payment confirms or purchases are restored.</summary>
        public static bool Notify
        {
            get => S.Notify;
            set => S.Notify = value;
        }

        /// <summary>How the payment sheet and restore screen are shown (default: the package's UI Toolkit screens).</summary>
        public static IScfgSheetHost? SheetHost
        {
            get => S.SheetHost ??= new ScfgDefaultSheetHost();
            set
            {
                S.SheetHost = value;
                S.SheetHostSet = true;
            }
        }

        // --- state ---------------------------------------------------------------------------------------

        /// <summary>Every static of the facade, so that a reset is one assignment (domain reload off).</summary>
        internal sealed class State
        {
            public Action<ScfgPaymentResult>? PaymentResolved;
            public Action? ReceiptsChanged;
            public bool Notify;
            public IScfgSheetHost? SheetHost;
            public bool SheetHostSet;

            public ConfigData? Config;
            public readonly List<Chain> CustomChains = new List<Chain>();
            public Texture2D? Logo;
            public byte[]? LogoPng;
            public bool LogoPngTried;
            public Engine? Engine;
            public Worker? Worker;
            public Exception? StartError;
            public readonly Dictionary<string, ScfgCheckoutController> Checkouts = new Dictionary<string, ScfgCheckoutController>();
            public readonly Dictionary<string, ScfgCheckoutController> Background = new Dictionary<string, ScfgCheckoutController>();
            public readonly Dictionary<string, ScfgRestoreController> Restores = new Dictionary<string, ScfgRestoreController>();
            public readonly Dictionary<string, Action<ScfgPaymentResult>> Callbacks = new Dictionary<string, Action<ScfgPaymentResult>>();
            public HashSet<string> SeenPending = new HashSet<string>();
            public readonly List<ScreenWait> Waits = new List<ScreenWait>();
            public int NextId;
            public Snapshot? LastSnapshot;
            public double LastSave = double.NegativeInfinity;
            public string? WarnedUnready;
            public JsonObject? PersistedPending; // what was last mirrored (null: not read yet)
            public JsonObject? PersistedReceipts;
            public bool Dirty;
            public bool SaveFailing;
            public int ReceiptsRev;
            public Exception? StoreError;
            public Wipe? Wipe;
            public bool ShuttingDown;
            public bool Pumping;

            // test hooks (Setup); not part of the API
            public string? StorePath;
            public Func<double> Clock = () => Time.realtimeSinceStartupAsDouble;
            public Func<long> WallClock = () => DateTimeOffset.UtcNow.ToUnixTimeSeconds();
            public ITransport? Transport;
            public UnityWebRequestTransport? OwnTransport;
            public IScheduler? Scheduler;
            public UnityScheduler? OwnScheduler;
            public IScfgPlatform Platform = new UnityPlatform();
            public Func<ServerArgs, ICheckoutServer>? ServerFactory;
            public bool Quiet;
            public bool? DevBuild;
            public readonly List<string> Reported = new List<string>();
            public readonly List<string> Warnings = new List<string>();
        }

        internal sealed class Wipe
        {
            public Job? Job;
            public bool Done;
            public Snapshot? Before;
        }

        private static State S = new State();

        /// <summary>Test hook: the facade's state, for assertions.</summary>
        internal static State Current => S;

        // --- reports -------------------------------------------------------------------------------------

        /// <summary>A developer error: always logged.</summary>
        internal static void Report(string message)
        {
            var line = "scfg: " + message;
            if (S.Quiet) S.Reported.Add(line);
            else Debug.LogError(line);
        }

        /// <summary>Setup advice: development builds (and the editor) only.</summary>
        internal static void Warn(string message)
        {
            var line = "scfg: " + message;
            if (S.Quiet) S.Warnings.Add(line);
            else if (IsDevBuild) Debug.LogWarning(line);
        }

        /// <summary>An error from the running plugin (a worker pass, a save): logged with the exception.</summary>
        internal static void ReportError(string heading, Exception? error = null)
        {
            var line = "scfg: " + heading + (error == null ? "" : " " + error.GetType().Name + ": " + error.Message);
            if (S.Quiet)
            {
                S.Reported.Add(line);
                return;
            }
            Debug.LogError(line);
            if (error != null) Debug.LogException(error);
        }

        internal static bool IsDevBuild => S.DevBuild ?? Debug.isDebugBuild;

        /// <summary>A ConfigError's code, else the error's class name ("TypeError").</summary>
        internal static string ErrCode(Exception e) =>
            e is ConfigError ce && !string.IsNullOrEmpty(ce.Code) ? ce.Code : e.GetType().Name;

        private static bool IsMisuse(Exception e) => e is ValueError || e is TypeError || e is KeyError;

        // --- configuration -------------------------------------------------------------------------------

        /// <summary>
        /// Add an EVM chain that is not built in. Call it before Configure; its key can then be used in Configure's
        /// Chains, RpcUrls and Confirmations. Returns null, or the error code it reported.
        /// </summary>
        public static string? AddChain(string key, ScfgChain chain)
        {
            if (S.Config != null)
                return Misuse(new ConfigError("call SCFG.AddChain() before SCFG.Configure()", "configure.after_add_chain"));
            if (chain == null) return Misuse(new TypeError("AddChain(): chain must not be null"));
            try
            {
                S.CustomChains.Add(Scfg.Core.Config.BuildChain(chain.ToKwargs(key)));
                return null;
            }
            catch (Exception e) when (IsMisuse(e))
            {
                return Misuse(e);
            }
        }

        /// <summary>
        /// Set the payout wallet and options. Call it before the first frame that needs payments; it can be called again
        /// until payments have started (the frame after a successful Configure). Returns null, or the error code it
        /// reported.
        /// </summary>
        public static string? Configure(ScfgOptions options)
        {
            if (S.Worker != null || S.StartError != null)
                return Misuse(new ConfigError("SCFG.Configure() can't change the configuration once payments have started",
                    "configure.after_start"));
            if (options == null) return Misuse(new TypeError("Configure(): options must not be null"));
            var kw = options.ToKwargs();
            var custom = new List<object?>();
            foreach (var c in S.CustomChains) custom.Add(c);
            kw["custom_chains"] = custom;
            ConfigData cfg;
            try
            {
                cfg = Scfg.Core.Config.BuildConfig(kw);
            }
            catch (Exception e) when (IsMisuse(e))
            {
                return Misuse(e);
            }
            S.Config = cfg;
            S.Logo = options.Logo;
            S.LogoPng = null;
            S.LogoPngTried = false;
            EnsureRunner();
            return null;
        }

        /// <summary>Declare a paid unlock. Returns null, or the error code it reported.</summary>
        public static string? AddItem(string itemId, ScfgItem item)
        {
            var cfg = RequireConfig(true);
            if (cfg == null) return "configure.not_called";
            if (item == null) return Misuse(new TypeError("AddItem(): item must not be null"));
            try
            {
                Scfg.Core.Config.AddItem(cfg, itemId, item.ToKwargs());
                return null;
            }
            catch (Exception e) when (IsMisuse(e))
            {
                return Misuse(e);
            }
        }

        private static string Misuse(Exception e)
        {
            Report(e.Message);
            return ErrCode(e);
        }

        private static ConfigError NotConfigured() =>
            new ConfigError("call SCFG.Configure(...) first", "configure.not_called");

        /// <summary>The Config, or null after reporting configure.not_called (unless <paramref name="report"/> is false).</summary>
        private static ConfigData? RequireConfig(bool report)
        {
            if (S.Config == null && report) Report(NotConfigured().Message);
            return S.Config;
        }

        // --- for the screens ---------------------------------------------------------------------------

        /// <summary>The accent color ("#rrggbb"): Configure's, else the default.</summary>
        public static string Accent => S.Config?.Accent ?? Scfg.Core.Config.DefaultAccent;

        /// <summary>The configured logo, or null.</summary>
        public static Texture2D? LogoTexture => S.Logo;

        /// <summary>An item's title (the declared title, else the id itself).</summary>
        public static string ItemTitle(string? itemId)
        {
            if (itemId != null && S.Config != null && S.Config.Items.TryGetValue(itemId, out var item)) return item.Title;
            return itemId ?? "";
        }

        /// <summary>A chain's display name, else its key.</summary>
        public static string ChainName(string? key)
        {
            if (key != null && S.Config != null && S.Config.Registry.TryGetValue(key, out var chain)) return chain.Name;
            return key ?? "";
        }

        public static bool IsWeb => S.Platform.IsWeb;
        public static bool IsMobile => S.Platform.IsMobile;

        /// <summary>True where payments can run: every platform the package builds for (parity with Ren'Py and Godot).</summary>
        public static bool Supported() => true;

        internal static int ReceiptsRevision => S.ReceiptsRev;

        /// <summary>The logo as PNG bytes for the checkout page, or null (none, or it cannot be encoded).</summary>
        internal static byte[]? LogoPng()
        {
            if (S.LogoPngTried) return S.LogoPng;
            S.LogoPngTried = true;
            S.LogoPng = S.Logo != null ? ScfgImages.EncodePng(S.Logo) : null;
            return S.LogoPng;
        }

        // --- worker lifecycle --------------------------------------------------------------------------

        private static string StorePath =>
            S.StorePath ?? Path.Combine(Application.persistentDataPath, "scfg", StateFileName);

        private static double Interval() =>
            S.Checkouts.Count == 0 && S.Restores.Count == 0 ? PollIdleS : PollOpenS;

        private static IScheduler Scheduler()
        {
            if (S.Scheduler != null) return S.Scheduler;
            S.OwnScheduler ??= new UnityScheduler(() => S.Clock(), e => ReportError("worker error:", e));
            return S.OwnScheduler;
        }

        private static ITransport Transport()
        {
            if (S.Transport != null) return S.Transport;
            S.OwnTransport ??= new UnityWebRequestTransport();
            return S.OwnTransport;
        }

        private static void LogWorkerError(Exception e) => ReportError("worker error:", e);

        /// <summary>The running Worker, started on first use; null when not configured or when it cannot start (reported once).</summary>
        internal static Worker? EnsureStarted()
        {
            if (S.Worker != null || S.StartError != null || S.Config == null) return S.Worker;
            Stored();
            if (S.StoreError != null)
            {
                // Not corrupt, only unreadable for now: starting with empty stores would save them over the receipts.
                S.StartError = S.StoreError;
                ReportError("cannot read the stored receipts:", S.StoreError);
                return null;
            }
            Engine engine;
            Worker worker;
            try
            {
                engine = new Engine(S.Config, Transport(), S.PersistedPending, S.PersistedReceipts, () => S.WallClock(),
                    onError: LogWorkerError);
                worker = new Worker(engine, Scheduler(), Interval, LogWorkerError);
            }
            catch (Exception e)
            {
                S.StartError = e;
                ReportError("cannot start the payment engine:", e);
                return null;
            }
            S.Engine = engine;
            worker.Start();
            S.Worker = worker;
            return worker;
        }

        /// <summary>Why the Engine could not start (already reported once), as an error for a result.</summary>
        private static Exception StartFailure() => S.StartError ?? new RuntimeError("payments could not start");

        /// <summary>
        /// Quit (Application.quitting) or a domain-reload reset: every waiting Pay / Restore resolves without delivering
        /// anything, every controller (open or background) is closed so no localhost page keeps its port, the Worker
        /// stops, requests in flight end, and what is not on disk is flushed.
        /// </summary>
        internal static void Shutdown()
        {
            var s = S;
            s.ShuttingDown = true;
            foreach (var w in s.Waits.ToList()) w.Release();
            s.Waits.Clear();
            foreach (var c in s.Checkouts.Values.Concat(s.Background.Values).ToList()) SafeClose(c.Core);
            s.Checkouts.Clear();
            s.Background.Clear();
            s.Restores.Clear();
            if (s.Worker != null)
            {
                s.Worker.Stop();
                if (s.Engine != null) s.Engine.OnError = _ => { };
                if (s.Wipe == null) Sync(s.Worker.Snapshot);
            }
            if (s.Dirty) Save();
            s.OwnTransport?.Close();
            s.OwnScheduler?.Clear();
        }

        private static void SafeClose(CheckoutController c)
        {
            try
            {
                c.Close();
            }
            catch (Exception e)
            {
                ReportError("cannot close a checkout:", e);
            }
        }

        /// <summary>Domain reload off: forget the last play session (see the file header).</summary>
        [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.SubsystemRegistration)]
        internal static void ResetStatics()
        {
            if (!S.ShuttingDown && (S.Worker != null || S.Waits.Count > 0 || S.Checkouts.Count > 0 || S.Background.Count > 0))
            {
                var quiet = S.Quiet;
                S.Quiet = true; // nobody is listening for the old session's reports
                Shutdown();
                S.Quiet = quiet;
            }
            ScfgDefaultSheetHost.ResetStatics();
            ScfgRunner.ResetStatics();
            S = new State();
        }

        private static void EnsureRunner()
        {
            if (Application.isPlaying) ScfgRunner.Ensure();
        }

        // --- the pump ------------------------------------------------------------------------------------

        /// <summary>One frame of the pump (see the file header). ScfgRunner calls it; tests call it directly.</summary>
        internal static void Pump()
        {
            if (S.Pumping) return;
            S.Pumping = true;
            try
            {
                S.OwnScheduler?.Pump();
                PumpWaits();
                if (S.Config == null) return;
                var worker = EnsureStarted();
                if (worker == null) return;
                // Read before draining: every outcome of the pass that made this snapshot is queued.
                var outcomesSnap = worker.Snapshot;
                var controllers = S.Checkouts.Values.Concat(S.Background.Values).ToList();
                foreach (var outcome in worker.DrainOutcomes())
                {
                    foreach (var c in controllers) c.Core.OnOutcome(outcome);
                    if (outcome.Result != null) Deliver(outcome.Result);
                }
                foreach (var c in controllers)
                {
                    c.Core.Poll();
                    RegisterCallback(c);
                }
                PruneCallbacks(outcomesSnap);
                CloseFinishedBackground();
                PollRestores();
                var snap = worker.Snapshot;
                WarnUnready(snap.Unready);
                // ClearLocalData: the snapshots made before the Engine was wiped are skipped
                if (S.Wipe != null && ((S.Wipe.Job != null && S.Wipe.Job.Cancelled()) || (S.Wipe.Done && !ReferenceEquals(snap, S.Wipe.Before))))
                    S.Wipe = null;
                if (!ReferenceEquals(snap, S.LastSnapshot))
                {
                    S.LastSnapshot = snap;
                    if (S.Wipe == null) Sync(snap);
                }
                if (S.Dirty && S.Clock() - S.LastSave >= PendingSaveInterval)
                {
                    S.LastSave = S.Clock();
                    Save();
                }
            }
            finally
            {
                S.Pumping = false;
            }
        }

        private static void CloseFinishedBackground()
        {
            if (S.Background.Count == 0 || S.Engine == null || S.Config == null) return;
            double timeout = ToDouble(Py.Get(Py.Get(S.Config.Methods, "browser"), "timeout_min")) * 60.0;
            long now = S.Engine.Clock();
            foreach (var cid in S.Background.Keys.ToList())
            {
                var c = S.Background[cid];
                var session = c.Core.Session;
                bool expired = session == null || now - ToDouble(session.Get("created_at")) > timeout;
                var state = c.Core.State;
                if (expired || state == "paid" || state == "underpaid" || state == "expired" || state == "choose")
                {
                    SafeClose(c.Core);
                    S.Background.Remove(cid);
                }
            }
        }

        private static double ToDouble(object? v) => v is long l ? l : v is double d ? d : v is int i ? i : 0;

        private static void PollRestores()
        {
            foreach (var r in S.Restores.Values.ToList())
            {
                var before = r.Core.State;
                r.Core.Poll();
                if (r.Core.State != before && r.Core.State == "done") DeliverRestore(r);
            }
        }

        private static void WarnUnready(IReadOnlyList<(string Chain, string Token)> unready)
        {
            if (unready.Count == 0) return;
            var key = string.Join(";", unready.Select(u => u.Chain + "/" + u.Token));
            if (key == S.WarnedUnready) return;
            S.WarnedUnready = key;
            foreach (var (chain, token) in unready)
                Warn("your wallet can't receive " + token + " on " + chain + " yet (no token account). Receive any amount of "
                     + token + " there once; until then players aren't offered it.");
        }

        /// <summary>Remember the Pay callback of the controller's session, once per session (Godot's _register_callback).</summary>
        private static void RegisterCallback(ScfgCheckoutController c)
        {
            var sid = c.Core.SessionId;
            if (!string.IsNullOrEmpty(sid) && c.Callback != null && sid != c.CallbackSid
                && c.Core.State != "paid" && c.Core.State != "underpaid")
            {
                S.Callbacks[sid!] = c.Callback;
                c.CallbackSid = sid;
            }
        }

        /// <summary>Drop the callbacks of sessions that left the Engine since a snapshot listed them.</summary>
        private static void PruneCallbacks(Snapshot snap)
        {
            var seen = new HashSet<string>();
            foreach (var sid in S.Callbacks.Keys.ToList())
            {
                if (snap.Pending.ContainsKey(sid)) seen.Add(sid);
                else if (S.SeenPending.Contains(sid)) S.Callbacks.Remove(sid);
            }
            S.SeenPending = seen;
        }

        /// <summary>A session's result: to its Pay callback, else to PaymentResolved. A paid one is announced when Notify is on.</summary>
        private static void Deliver(PaymentResult result)
        {
            var typed = ScfgPaymentResult.FromCore(result);
            Action<ScfgPaymentResult>? cb = null;
            if (typed.SessionId != null) S.Callbacks.TryGetValue(typed.SessionId, out cb);
            if ((typed.Status == ScfgPaymentStatus.Paid || typed.Status == ScfgPaymentStatus.Underpaid) && typed.SessionId != null)
                S.Callbacks.Remove(typed.SessionId);
            if (typed.Status == ScfgPaymentStatus.Paid && S.Notify)
            {
                if (typed.Item != null && S.Config != null && S.Config.Items.TryGetValue(typed.Item, out var item))
                    NotifyPlayer(ScfgUiTexts.Format("payment_confirmed_item", item.Title));
                else
                    NotifyPlayer(ScfgUiTexts.Get("payment_confirmed"));
            }
            Resolve(cb, typed);
        }

        /// <summary>Ren'Py's _call(cb or on_result, result).</summary>
        private static void Resolve(Action<ScfgPaymentResult>? cb, ScfgPaymentResult typed)
        {
            if (cb != null) Call(cb, typed);
            else
            {
                var handlers = S.PaymentResolved;
                if (handlers != null) Call(handlers, typed);
            }
        }

        /// <summary>A game handler: its exception is logged, never thrown into the pump.</summary>
        private static void Call<T>(Action<T>? cb, T value)
        {
            if (cb == null) return;
            try
            {
                cb(value);
            }
            catch (Exception e)
            {
                Debug.LogException(e);
            }
        }

        private static void DeliverRestore(ScfgRestoreController r)
        {
            var typed = ScfgRestoreResult.FromCore(r.Core.Result());
            if (typed.Restored.Count > 0 && S.Notify) NotifyPlayer(ScfgUiTexts.Get("purchases_restored"));
            if (r.Callback != null && S.Restores.ContainsKey(r.Rid)) Call(r.Callback, typed);
        }

        private static void NotifyPlayer(string text)
        {
            try
            {
                SheetHost?.Notify(text);
            }
            catch (Exception e)
            {
                ReportError("the sheet host's Notify failed:", e);
            }
        }

        // --- screens closing -----------------------------------------------------------------------------

        /// <summary>Waits for a screen's Closed (Godot's _ScreenWait): picked up by the pump, or Release at quit.</summary>
        internal sealed class ScreenWait
        {
            public readonly IScfgScreen Screen;
            public string? Closed; // raised, not yet picked up
            public string? How; // the outcome, once ended
            public bool Gone;
            private readonly Action<ScreenWait> _end;

            public ScreenWait(IScfgScreen screen, Action<ScreenWait> end)
            {
                Screen = screen;
                _end = end;
                screen.Closed += OnClosed;
            }

            private void OnClosed(string how)
            {
                if (Closed == null) Closed = string.IsNullOrEmpty(how) ? "cancel" : how;
            }

            /// <summary>A frame: the screen closed, or vanished.</summary>
            public void Check()
            {
                if (How != null) return;
                if (Closed != null) End(Closed, false);
                else if (Screen.Gone) End("cancel", true);
            }

            public void Release() => End("shutdown", Screen.Gone);

            private void End(string how, bool gone)
            {
                if (How != null) return;
                How = how;
                Gone = gone;
                Screen.Closed -= OnClosed;
                _end(this);
            }
        }

        private static void PumpWaits()
        {
            if (S.Waits.Count == 0) return;
            foreach (var w in S.Waits.ToList())
            {
                w.Check();
                if (w.How != null) S.Waits.Remove(w);
            }
        }

        private static string NewId(string prefix)
        {
            S.NextId++;
            return prefix + S.NextId.ToString(System.Globalization.CultureInfo.InvariantCulture);
        }
    }
}
