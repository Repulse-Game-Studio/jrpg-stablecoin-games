// Twin of scfg_core/overlay.py: the browser checkout as an overlay inside the game's own web page (a Unity WebGL
// build). Ported from rpgmaker/src/core/overlay.ts, with the hardening it took from godot/addons/scfg/core/overlay.gd.
//
// On the web the game already runs in a browser, and wallet extensions inject into its page, so the checkout page
// (app.js, evm.js, solana.js) is shown over the game instead of being served on localhost. OverlayCheckout is an
// ICheckoutServer, so CheckoutController drives it like the localhost server. The page's messages are read by Pump(),
// which the controller calls every frame; they are validated like the local server's requests (PageMessages), and a
// tx hash stays a hint the watcher verifies on-chain.
//
// Everything the page (IOverlayUi) hands back is checked: a Take() that is not a list is no messages, a request id
// must be an exactly representable integer (a long, or a double that is integral and a safe integer, |x| <= 2**53 - 1,
// as JavaScript's Number.isSafeInteger; never a bool), and a reused id cancels the older build. Once Stop() ran (also
// from inside a callback) no callback is called again, not even for the rest of that batch, and no build is answered
// (a Job handed back by a BuildTx that stopped the overlay is cancelled at once). A BuildTx that throws or returns
// null, or a Job that ends without a JsonObject, is answered 503. The status is pushed when it changed by Python's ==.
// Every method is synchronous.
#nullable enable
using System;
using System.Collections.Generic;
using System.Linq;

namespace Scfg.Core
{
    /// <summary>The page the overlay is shown in (the twin of web_page.py's overlay methods; the glue's bridge).</summary>
    public interface IOverlayUi
    {
        void Open(string key, JsonObject sessionInfo, string? logoUrl);
        void Show(string key);
        void Close(string key);
        /// <summary>The messages the overlay's page posted since the last call: a list (anything else is no messages).</summary>
        object? Take(string key);
        void SetStatus(string key, JsonObject status);
        void Reply(string key, long id, int status, JsonObject body);
    }

    public sealed class OverlayCheckout : ICheckoutServer
    {
        /// <summary>What <see cref="Url"/> is: there is no page to open.</summary>
        public const string OverlayUrl = "overlay:checkout";

        /// <summary>The largest integer a double (and so the page's JavaScript) keeps exact: 2**53 - 1.</summary>
        private const double MaxSafeInteger = 9007199254740991.0;

        private sealed class Build
        {
            public readonly long Id;
            public Job Job;
            public double Deadline;
            public Build(long id, Job job, double deadline) { Id = id; Job = job; Deadline = deadline; }
        }

        public IOverlayUi Ui { get; }
        /// <summary>Which overlay of the page this is (the glue's "o&lt;n&gt;").</summary>
        public string Key { get; }
        public JsonObject SessionInfo { get; }
        /// <summary>A hint from the page, already through <see cref="PageMessages.ParseTxMessage"/>.</summary>
        public Action<string, string?> OnTx { get; }
        public Func<JsonObject> GetStatus { get; }
        /// <summary>The logo as a data: URL, or null.</summary>
        public string? LogoUrl { get; }
        /// <summary>Solana only: the unsigned transfer for the player's account, as a Job the overlay polls.</summary>
        public Func<string, Job>? BuildTx { get; }
        /// <summary>Seconds, monotonic (the build timeout). Default: a Stopwatch.</summary>
        public Func<double> Clock { get; }

        public string Url => OverlayUrl;
        public bool Running => _open;

        private readonly List<Build> _builds = new List<Build>(); // in the order the page asked, as Python's dict
        private JsonObject? _status;
        private bool _open;

        public OverlayCheckout(IOverlayUi ui, string key, JsonObject sessionInfo, Action<string, string?> onTx,
                               Func<JsonObject> getStatus, string? logoUrl = null, Func<string, Job>? buildTx = null,
                               Func<double>? clock = null)
        {
            Ui = ui ?? throw new TypeError("an OverlayCheckout needs a ui");
            Key = key ?? throw new TypeError("an OverlayCheckout needs a key");
            SessionInfo = sessionInfo ?? throw new TypeError("an OverlayCheckout needs session_info");
            OnTx = onTx ?? throw new TypeError("an OverlayCheckout needs on_tx");
            GetStatus = getStatus ?? throw new TypeError("an OverlayCheckout needs get_status");
            LogoUrl = logoUrl;
            BuildTx = buildTx;
            Clock = clock ?? Py.MonotonicSeconds;
        }

        /// <summary>The overlay for a CheckoutController's server factory: its <see cref="ServerArgs"/>.</summary>
        public OverlayCheckout(IOverlayUi ui, string key, ServerArgs args, string? logoUrl = null, Func<double>? clock = null)
            : this(ui, key, (args ?? throw new TypeError("an OverlayCheckout needs ServerArgs")).SessionInfo, args.OnTx,
                   args.GetStatus, logoUrl, args.BuildTx, clock)
        {
        }

        /// <summary>The builds waiting for their Job (for the tests).</summary>
        internal int PendingBuilds => _builds.Count;

        public void Start()
        {
            Ui.Open(Key, SessionInfo, LogoUrl);
            _open = true;
            PushStatus();
        }

        public void Reopen()
        {
            if (_open) Ui.Show(Key);
        }

        public void Stop()
        {
            if (!_open) return;
            _open = false;
            var builds = _builds.ToArray();
            _builds.Clear();
            foreach (var b in builds) b.Job.Cancel();
            Ui.Close(Key);
        }

        public void Pump()
        {
            if (!_open) return;
            if (Ui.Take(Key) is IReadOnlyList<object?> messages)
            {
                foreach (var message in messages.ToArray())
                {
                    Handle(message);
                    if (!_open) return; // a callback stopped the overlay: the rest of the batch is moot
                }
            }
            FinishBuilds();
            if (_open) PushStatus();
        }

        /// <summary>A page request id: a long, or an integral double within the safe integers (never a bool).</summary>
        private static bool RequestId(object? value, out long id)
        {
            switch (value)
            {
                case long l:
                    // beyond 2**53 the page's JavaScript cannot have sent it nor read the reply's id back exactly: only
                    // a hostile page makes one up, and it gets an answer it cannot match (no harm to the game)
                    id = l;
                    return true;
                case int i:
                    id = i;
                    return true;
                case double d when Math.Floor(d) == d && Math.Abs(d) <= MaxSafeInteger: // NaN and the infinities fail
                    id = (long)d;
                    return true;
                default:
                    id = 0;
                    return false;
            }
        }

        private void Handle(object? message)
        {
            if (!(message is JsonObject m)) return;
            var kind = m.Get("type") as string;
            if (kind == "tx")
            {
                (string TxHash, string? Sender) parsed;
                try
                {
                    parsed = PageMessages.ParseTxMessage(m.Get("body"));
                }
                catch (Exception e) when (e is ValueError || e is TypeError)
                {
                    return; // the page only posts what a wallet returned: nothing to answer
                }
                OnTx(parsed.TxHash, parsed.Sender);
                return;
            }
            if (kind != "build" || !RequestId(m.Get("id"), out var rid)) return;
            if (BuildTx == null)
            {
                Ui.Reply(Key, rid, 404, new JsonObject { { "error", "not_found" } });
                return;
            }
            string account;
            try
            {
                account = PageMessages.ParseBuildMessage(m.Get("body"));
            }
            catch (Exception e) when (e is ValueError || e is TypeError)
            {
                Ui.Reply(Key, rid, 400, new JsonObject { { "error", "bad account" } });
                return;
            }
            Job? job;
            try
            {
                job = BuildTx(account);
            }
            catch (Exception)
            {
                job = null; // the controller is gone, or its Worker stopped
            }
            if (!_open)
            {
                // BuildTx stopped the overlay (its controller closed): nobody answers this build, so it must not run
                job?.Cancel();
                return;
            }
            if (job == null)
            {
                Ui.Reply(Key, rid, 503, Unavailable());
                return;
            }
            double deadline = Clock() + PageMessages.BuildTimeoutS;
            var older = _builds.FirstOrDefault(b => b.Id == rid);
            if (older == null)
            {
                _builds.Add(new Build(rid, job, deadline));
                return;
            }
            var moot = older.Job; // the page reused an id: the older build is moot (its place in the order is kept)
            older.Job = job;
            older.Deadline = deadline;
            if (!ReferenceEquals(moot, job)) moot.Cancel(); // BuildTx may hand back the same Job: it is not moot
        }

        private void FinishBuilds()
        {
            foreach (var b in _builds.ToArray())
            {
                if (!_open) return; // a reply stopped the overlay
                if (b.Job.Done())
                {
                    _builds.Remove(b);
                    var value = b.Job.Exception() == null ? b.Job.Result() : null;
                    if (value is JsonObject body) Ui.Reply(Key, b.Id, 200, body);
                    else Ui.Reply(Key, b.Id, 503, Unavailable());
                }
                else if (Clock() >= b.Deadline)
                {
                    _builds.Remove(b);
                    b.Job.Cancel(); // don't build it later for a page that stopped waiting
                    Ui.Reply(Key, b.Id, 503, Unavailable());
                }
            }
        }

        private static JsonObject Unavailable() => new JsonObject { { "error", "unavailable" } };

        private void PushStatus()
        {
            var status = GetStatus();
            if (status == null) return; // not an object: nothing to push
            if (_status == null || !Py.Eq(status, _status))
            {
                _status = (JsonObject)Copy(status, 0)!; // a copy, so a later change to the controller's object is seen
                Ui.SetStatus(Key, status);
            }
        }

        /// <summary>
        /// A copy of the lists and dicts of a JSON value; any other value is kept as it is (immutable or opaque). Like
        /// <see cref="Py.Eq"/> it knows the shapes Json.Loads produces (JsonObject, <c>List&lt;object?&gt;</c>): another
        /// list or dict type is kept, not copied.
        /// </summary>
        private static object? Copy(object? v, int depth)
        {
            switch (v)
            {
                case List<object?> list:
                {
                    if (depth >= Json.MaxDepth) throw new ValueError("status nested deeper than Json.MaxDepth (or a circular reference)");
                    var o = new List<object?>(list.Count);
                    foreach (var x in list) o.Add(Copy(x, depth + 1));
                    return o;
                }
                case JsonObject obj:
                {
                    if (depth >= Json.MaxDepth) throw new ValueError("status nested deeper than Json.MaxDepth (or a circular reference)");
                    var o = new JsonObject();
                    foreach (var kv in obj) o[kv.Key] = Copy(kv.Value, depth + 1);
                    return o;
                }
                default:
                    return v;
            }
        }
    }
}
