// The Unity e2e's demo game and its driver (twin of godot/demo/e2e/e2e_driver.gd), run by tools/run_unity_e2e.py in a
// Linux player (Mono or IL2CPP). Nothing here runs unless SCFG_E2E_CONFIG is set, so the dev project's play-mode tests
// never see it.
//
// THE GAME. A one-screen demo: "Chapter 2" (locked, or the thank-you once owned) with an Unlock button, and a Tip
// button (1 USDC). Unlock pauses the game (Time.timeScale = 0, as the docs tell developers) while `await SCFG.Pay(...)`
// runs. SCFG_E2E_CONFIG is a JSON file {"configure": {wallet, solana_wallet, testnet, chains, tokens}, "items": {...}};
// SCFG_E2E_CHAIN points one built-in chain at the local node ({"network", "key", "rpc", "confirmations", "tokens"}). A
// config that is not that, or that Configure refuses, prints why and quits with code 1 at once.
//
// THE DRIVER. JSON lines on stdin, read on a thread of its own and run on the main thread, one per frame; each reply is
// one line "SCFG-REPLY {"ok": ...}" or {"error": ...} on the runner's named pipe (SCFG_E2E_OUT; a player's own stdout
// goes to its log file). Commands: snapshot, click {name, in: "screen" | "game"},
// action {action: "ui_accept" | "ui_cancel"} (a navigation event to the focused control), owns {item}, user_dir,
// quit. A sheet is pressed the way a player presses it: its own button actions (ScfgScreenView.Press) or navigation
// events. Every error, exception, assert and warning the game logs is also printed as "SCFG-PROBLEM <type>: <text>",
// and "SCFG-QUITTING" is printed when the game quits on its own. The platform is a recording one: Copy and Open go to
// the snapshot's "copied" and "opened" lists (no real browser or clipboard).
#nullable enable
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Threading;
using Scfg.Core;
using UnityEngine;
using UnityEngine.UIElements;

namespace Scfg.E2E
{
    internal sealed class RecordingPlatform : IScfgPlatform
    {
        public readonly List<string> Opened = new List<string>();
        public readonly List<string> Copied = new List<string>();
        public bool IsWeb => false;
        public bool IsMobile => false;
        public void OpenUrl(string url) => Opened.Add(url);
        public void ClipboardSet(string text) => Copied.Add(text);
        public string ClipboardGet() => Copied.Count > 0 ? Copied[Copied.Count - 1] : "";
    }

    [AddComponentMenu("")]
    public sealed class E2EDriver : MonoBehaviour
    {
        public const string Item = "chapter2";
        public const string Unlocked = "Chapter 2: unlocked. Thank you for supporting the game!";
        public const string Locked = "Chapter 2: locked.";

        private static readonly object OutLock = new object();
        private static Stream? _stdout;
        private readonly ConcurrentQueue<string> _commands = new ConcurrentQueue<string>();
        private readonly RecordingPlatform _platform = new RecordingPlatform();
        private string? _thanks;
        private bool _quitting;

        [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.AfterSceneLoad)]
        private static void Boot()
        {
            if (string.IsNullOrEmpty(Environment.GetEnvironmentVariable("SCFG_E2E_CONFIG"))) return;
            var go = new GameObject("SCFG E2E");
            DontDestroyOnLoad(go);
            go.AddComponent<E2EDriver>();
        }

        /// <summary>
        /// One line to the runner: to the file (a named pipe) SCFG_E2E_OUT names. A player redirects its stdout (fd 1)
        /// into its log file, so stdout cannot carry the replies; without SCFG_E2E_OUT they go to /dev/stdout (the log).
        /// </summary>
        internal static void Out(string line)
        {
            lock (OutLock)
            {
                try
                {
                    var path = Environment.GetEnvironmentVariable("SCFG_E2E_OUT");
                    _stdout ??= new FileStream(string.IsNullOrEmpty(path) ? "/dev/stdout" : path!, FileMode.Open, FileAccess.Write,
                        FileShare.ReadWrite);
                    var bytes = new System.Text.UTF8Encoding(false).GetBytes(line + "\n");
                    _stdout.Write(bytes, 0, bytes.Length);
                    _stdout.Flush();
                }
                catch (IOException)
                {
                    // the runner is gone
                }
            }
        }

        private void Awake()
        {
            Application.logMessageReceivedThreaded += (text, stack, type) =>
            {
                if (type != LogType.Log) Out("SCFG-PROBLEM " + type + ": " + text.Replace('\n', ' '));
            };
            Application.targetFrameRate = 60;
            string? why = Setup();
            if (why != null)
            {
                Out("SCFG-PROBLEM Error: " + why);
                Quit(1);
                return;
            }
            var reader = new Thread(ReadStdin) { IsBackground = true, Name = "scfg-e2e-stdin" };
            reader.Start();
        }

        private void ReadStdin()
        {
            try
            {
                string? line;
                while ((line = Console.In.ReadLine()) != null)
                    if (line.Trim().Length > 0) _commands.Enqueue(line);
            }
            catch (ThreadAbortException)
            {
                // the player is shutting down
            }
            catch (Exception e)
            {
                Out("SCFG-PROBLEM Exception: stdin: " + e.Message);
            }
        }

        private void Quit(int code)
        {
            if (_quitting) return;
            _quitting = true;
            Out("SCFG-QUITTING");
            Application.Quit(code);
        }

        // --- the game ----------------------------------------------------------------------------------

        /// <summary>Reads the e2e config and configures SCFG; returns why it cannot, or null.</summary>
        private string? Setup()
        {
            object? doc;
            try
            {
                doc = Json.Loads(File.ReadAllText(Environment.GetEnvironmentVariable("SCFG_E2E_CONFIG")!));
            }
            catch (Exception e)
            {
                return "the e2e config is not readable JSON: " + e.Message;
            }
            if (!(doc is JsonObject config) || !(config.Get("configure") is JsonObject kw) || !(config.Get("items") is JsonObject items))
                return "the e2e config is not a JSON object with configure and items";
            var chain = Environment.GetEnvironmentVariable("SCFG_E2E_CHAIN");
            if (!string.IsNullOrEmpty(chain))
            {
                try
                {
                    Override((JsonObject)Json.Loads(chain!)!);
                }
                catch (Exception e)
                {
                    return "SCFG_E2E_CHAIN: " + e.Message;
                }
            }
            SCFG.Current.Platform = _platform;
            var options = new ScfgOptions
            {
                Wallet = kw.Get("wallet") as string,
                SolanaWallet = kw.Get("solana_wallet") as string,
                Testnet = kw.Get("testnet") is bool t && t,
                Chains = Strings(kw.Get("chains")),
                Tokens = Strings(kw.Get("tokens")),
            };
            if (SCFG.Configure(options) is string code) return "scfg: configure() refused the e2e config: " + code;
            foreach (var kv in items)
            {
                var it = (JsonObject)kv.Value!;
                var err = SCFG.AddItem(kv.Key, new ScfgItem
                {
                    Code = (int)(long)it["code"]!, Price = it.Get("price") as string, Token = it.Get("token") as string,
                    Title = it.Get("title") as string,
                });
                if (err != null) return "scfg: add_item() refused " + kv.Key + ": " + err;
            }
            return null;
        }

        private static List<string>? Strings(object? v) => v is List<object?> list ? list.Select(x => (string)x!).ToList() : null;

        private static void Override(JsonObject o)
        {
            bool testnet = o.Get("network") as string == "testnet";
            var key = (string)o["key"]!;
            var baseChain = Chains.GetChains(testnet)[key];
            var rpc = ((List<object?>)o["rpc"]!).Select(u => (string)u!).ToList();
            long? confirmations = o.Get("confirmations") as long?;
            Dictionary<string, Token>? tokens = null;
            if (o.Get("tokens") is JsonObject tk && tk.Count > 0)
            {
                tokens = new Dictionary<string, Token>();
                foreach (var kv in tk) tokens[kv.Key] = Chains.MakeToken((string)kv.Value!);
            }
            Chains.Override(testnet, baseChain.With(rpc, confirmations, tokens));
        }

        private bool Owned => SCFG.Owns(Item);

        private async void Unlock()
        {
            Time.timeScale = 0; // the game is paused while the sheet is open
            try
            {
                await SCFG.Pay(ScfgPayKind.Unlock, new ScfgPayRequest { Item = Item });
            }
            finally
            {
                Time.timeScale = 1;
            }
        }

        private async void Tip()
        {
            var r = await SCFG.Pay(ScfgPayKind.Tip, new ScfgPayRequest { Amount = "1.00", Token = "USDC" });
            if (r.Status == ScfgPaymentStatus.Paid) _thanks = "Thank you for the " + r.Amount + " USDC tip!";
        }

        private JsonObject GameView()
        {
            var buttons = new List<object?>();
            if (!Owned) buttons.Add(new JsonObject { { "name", "Unlock" }, { "text", "Unlock Chapter 2" }, { "disabled", false } });
            buttons.Add(new JsonObject { { "name", "Tip" }, { "text", "Tip the developer" }, { "disabled", false } });
            var texts = new List<object?> { Owned ? Unlocked : Locked };
            if (_thanks != null) texts.Add(_thanks);
            return new JsonObject { { "buttons", buttons }, { "texts", texts } };
        }

        // --- the driver --------------------------------------------------------------------------------

        private void Update()
        {
            if (_quitting || !_commands.TryDequeue(out var line)) return;
            object? reply;
            try
            {
                reply = Run((JsonObject)Json.Loads(line)!);
            }
            catch (Exception e)
            {
                Out("SCFG-REPLY " + Json.Dumps(new JsonObject { { "error", e.GetType().Name + ": " + e.Message } }));
                return;
            }
            Out("SCFG-REPLY " + Json.Dumps(new JsonObject { { "ok", reply } }));
        }

        private static IEnumerable<ScfgScreenView> Views() =>
            SCFG.Current.Waits.Select(w => w.Screen).OfType<ScfgScreenView>().Where(v => !v.Ended);

        private object? Run(JsonObject cmd)
        {
            switch (cmd["op"] as string)
            {
                case "snapshot":
                    return Snapshot();
                case "click":
                {
                    var name = (string)cmd["name"]!;
                    if (cmd.Get("in") as string == "game")
                    {
                        if (name == "Unlock" && !Owned) Unlock();
                        else if (name == "Tip") Tip();
                        else throw new InvalidOperationException("no game button " + name);
                        return true;
                    }
                    var view = Views().LastOrDefault() ?? throw new InvalidOperationException("no screen is open");
                    if (!view.Press(name)) throw new InvalidOperationException("no enabled button " + name + " on the screen");
                    return true;
                }
                case "action":
                {
                    var view = Views().LastOrDefault() ?? throw new InvalidOperationException("no screen is open");
                    var target = view.Root.focusController?.focusedElement as VisualElement ?? view.Root;
                    if (cmd.Get("action") as string == "ui_cancel")
                    {
                        using var e = NavigationCancelEvent.GetPooled();
                        target.SendEvent(e);
                    }
                    else
                    {
                        using var e = NavigationSubmitEvent.GetPooled();
                        target.SendEvent(e);
                    }
                    return true;
                }
                case "owns":
                    return SCFG.Owns((string)cmd["item"]!);
                case "user_dir":
                    return Application.persistentDataPath;
                case "quit":
                    Quit(0);
                    return true;
                default:
                    throw new InvalidOperationException("unknown op " + cmd.Get("op"));
            }
        }

        private JsonObject Snapshot()
        {
            var screens = new List<object?>();
            foreach (var v in Views())
            {
                var buttons = new List<object?>();
                foreach (var key in v.Keys)
                    if (v.Control(key) is Button b && b.style.display != DisplayStyle.None)
                        buttons.Add(new JsonObject
                        {
                            { "name", key }, { "text", ButtonText(b) }, { "disabled", !b.enabledInHierarchy },
                        });
                var texts = v.Root.Query<Label>().ToList().Where(l => !(l.parent is Button))
                    .Select(l => (object?)l.text).ToList();
                screens.Add(new JsonObject
                {
                    { "kind", v is ScfgSheetView ? "sheet" : "restore" }, { "buttons", buttons }, { "texts", texts },
                    { "focus", v.Focused },
                });
            }
            return new JsonObject
            {
                { "screens", screens }, { "game", GameView() }, { "paused", Time.timeScale == 0 },
                { "copied", _platform.Copied.Cast<object?>().ToList() }, { "opened", _platform.Opened.Cast<object?>().ToList() },
            };
        }

        private static string ButtonText(Button b)
        {
            if (!string.IsNullOrEmpty(b.text)) return b.text;
            var first = b.Q<Label>();
            return first != null ? first.text : "";
        }
    }
}
