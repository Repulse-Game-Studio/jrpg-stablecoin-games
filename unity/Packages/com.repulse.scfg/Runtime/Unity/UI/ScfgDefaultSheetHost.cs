// The default IScfgSheetHost (Godot's ui/default_sheet_host.gd): each screen gets its own GameObject with a UIDocument on
// the package's PanelSettings (sort order 1000, above the game's own UI Toolkit panels and never touching its Canvas,
// TextMeshPro or prefabs), ticked every frame by a small MonoBehaviour on unscaled time. Notify shows ScfgToast at the
// top right. Destroying a screen's GameObject from outside counts as "cancel" (IScfgScreen.Gone).
//
// Customisation: SheetStyle, a StyleSheet added after the package's own, can restyle the screens (the USS variables
// --scfg-accent and --scfg-ink, or any class in Resources/ScfgUI/ScfgSheet.uss); PanelSettings replaces the panel.
#nullable enable
using UnityEngine;
using UnityEngine.UIElements;

namespace Scfg
{
    /// <summary>The package's sheet host: UI Toolkit screens on a panel of their own (see the file header).</summary>
    public sealed class ScfgDefaultSheetHost : IScfgSheetHost
    {
        /// <summary>The sort order of the package's panel: above the game's own UI Toolkit panels.</summary>
        public const int SortingOrder = 1000;
        /// <summary>The Resources path of the screens' style sheet (Resources/ScfgUI/ScfgSheet.uss).</summary>
        public const string StyleResource = "ScfgUI/ScfgSheet";
        /// <summary>The Resources path of the package's theme (Resources/ScfgUI/ScfgTheme.tss).</summary>
        public const string ThemeResource = "ScfgUI/ScfgTheme";
        /// <summary>The panel's reference resolution: it scales with the screen size from this, matching on Expand.</summary>
        public static readonly Vector2Int ReferenceResolution = new Vector2Int(960, 720);
        /// <summary>
        /// The package's PanelSettings asset: an asset (not one made at runtime) carries the text settings and ICU data
        /// the editor assigns, so wrapped text and line breaking work in a build.
        /// </summary>
        public const string PanelResource = "ScfgUI/ScfgPanel";

        private static PanelSettings? _panel;
        private static StyleSheet? _styles;
        private static bool _stylesTried;
        private static int _layer;

        /// <summary>A style sheet applied after the package's own (USS override; see the file header).</summary>
        public static StyleSheet? SheetStyle { get; set; }

        /// <summary>The panel the screens are drawn on; default: the package's, created on first use.</summary>
        public static PanelSettings? PanelSettings { get; set; }

        internal static void ResetStatics()
        {
            ScfgObjects.Destroy(_panel);
            _panel = null;
            _styles = null;
            _stylesTried = false;
            _layer = 0;
            SheetStyle = null;
            PanelSettings = null;
            ScfgScreenTicker.Clear();
        }

        internal static StyleSheet? LoadStyleSheet()
        {
            if (!_stylesTried)
            {
                _stylesTried = true;
                _styles = Resources.Load<StyleSheet>(StyleResource);
            }
            return _styles;
        }

        internal static PanelSettings Panel()
        {
            if (PanelSettings != null) return PanelSettings;
            if (_panel != null) return _panel;
            var asset = Resources.Load<PanelSettings>(PanelResource);
            var p = asset != null ? Object.Instantiate(asset) : ScriptableObject.CreateInstance<PanelSettings>();
            p.name = "SCFG Panel";
            p.hideFlags = HideFlags.HideAndDontSave;
            if (p.themeStyleSheet == null) p.themeStyleSheet = Resources.Load<ThemeStyleSheet>(ThemeResource);
            ConfigurePanel(p);
            _panel = p;
            return p;
        }

        /// <summary>
        /// Sets <paramref name="panel"/> up as the package's panel is: <see cref="SortingOrder"/>, scaled with the screen
        /// size from <see cref="ReferenceResolution"/> (Expand), and no clear color, so the game shows through.
        /// </summary>
        public static void ConfigurePanel(PanelSettings panel)
        {
            panel.sortingOrder = SortingOrder;
            panel.scaleMode = PanelScaleMode.ScaleWithScreenSize;
            panel.referenceResolution = ReferenceResolution;
            panel.screenMatchMode = PanelScreenMatchMode.Expand;
            panel.clearColor = false;
        }

        /// <inheritdoc/>
        public IScfgScreen? OpenPayment(ScfgCheckoutController controller) => Open(new ScfgSheetView(controller), "SCFG Payment Sheet");

        /// <inheritdoc/>
        public IScfgScreen? OpenRestore(ScfgRestoreController controller) => Open(new ScfgRestoreView(controller), "SCFG Restore");

        private static ScfgScreenView Open(ScfgScreenView view, string name)
        {
            var doc = NewDocument(name, out var go);
            var ticker = go.AddComponent<ScfgScreenTicker>();
            ticker.View = view;
            try
            {
                doc.rootVisualElement.Add(view.Root);
                view.Tick();
            }
            catch
            {
                // a screen that cannot build is not left dimming the game: the flow then fails with sheet.unavailable
                ScfgScreenTicker.Forget(view);
                ScfgObjects.Destroy(go);
                throw;
            }
            return view;
        }

        private static UIDocument NewDocument(string name, out GameObject go)
        {
            go = new GameObject(name);
            go.SetActive(false);
            if (Application.isPlaying) Object.DontDestroyOnLoad(go);
            var doc = go.AddComponent<UIDocument>();
            doc.panelSettings = Panel();
            doc.sortingOrder = ++_layer;
            go.SetActive(true);
            return doc;
        }

        /// <inheritdoc/>
        public void Close(IScfgScreen screen)
        {
            if (screen is ScfgScreenView view && ScfgScreenTicker.Of(view) is ScfgScreenTicker t && t != null)
            {
                ScfgScreenTicker.Forget(view);
                ScfgObjects.Destroy(t.gameObject);
            }
        }

        /// <inheritdoc/>
        public void Notify(string text)
        {
            var doc = NewDocument("SCFG Notice", out var go);
            var toast = go.AddComponent<ScfgToast>();
            toast.Show(doc.rootVisualElement, text);
        }
    }

    /// <summary>Ticks a screen every frame and marks it Gone when its GameObject is destroyed.</summary>
    [AddComponentMenu("")]
    internal sealed class ScfgScreenTicker : MonoBehaviour
    {
        private static readonly System.Collections.Generic.Dictionary<ScfgScreenView, ScfgScreenTicker> Tickers =
            new System.Collections.Generic.Dictionary<ScfgScreenView, ScfgScreenTicker>();

        private ScfgScreenView? _view;

        public ScfgScreenView? View
        {
            get => _view;
            set
            {
                _view = value;
                if (value != null) Tickers[value] = this;
            }
        }

        public static ScfgScreenTicker? Of(ScfgScreenView view) => Tickers.TryGetValue(view, out var t) ? t : null;

        public static void Forget(ScfgScreenView view) => Tickers.Remove(view);

        public static void Clear() => Tickers.Clear();

        private void Update()
        {
            if (_view == null) return;
            try
            {
                _view.Tick();
            }
            catch (System.Exception e)
            {
                SCFG.ReportError("a screen's Tick threw:", e);
            }
        }

        private void OnDestroy()
        {
            if (_view == null) return;
            _view.Gone = true;
            Tickers.Remove(_view);
        }
    }

    /// <summary>A short notice at the top right (Godot's ui/toast.gd): shown for Seconds, faded out, then destroyed.</summary>
    [AddComponentMenu("")]
    internal sealed class ScfgToast : MonoBehaviour
    {
        public const float Seconds = 3f;
        public const float FadeSeconds = 0.3f;

        private VisualElement? _box;
        private float _age;

        /// <summary>The notice's text (the package's tests).</summary>
        internal string Text { get; private set; } = "";

        internal void Show(VisualElement root, string text)
        {
            Text = text;
            var screen = new VisualElement { pickingMode = PickingMode.Ignore };
            screen.AddToClassList("scfg-toast-layer");
            screen.style.position = Position.Absolute;
            screen.style.left = 0;
            screen.style.top = 0;
            screen.style.right = 0;
            screen.style.bottom = 0;
            var styles = ScfgDefaultSheetHost.LoadStyleSheet();
            if (styles != null) screen.styleSheets.Add(styles);
            if (ScfgDefaultSheetHost.SheetStyle != null) screen.styleSheets.Add(ScfgDefaultSheetHost.SheetStyle);
            _box = new VisualElement { pickingMode = PickingMode.Ignore, name = "scfg-toast" };
            _box.AddToClassList("scfg-toast");
            var label = new Label(text) { enableRichText = false, pickingMode = PickingMode.Ignore };
            label.AddToClassList("scfg-toast__text");
            _box.Add(label);
            screen.Add(_box);
            root.Add(screen);
        }

        private void Update()
        {
            _age += Time.unscaledDeltaTime;
            if (_box != null && _age > Seconds) _box.style.opacity = Mathf.Clamp01(1f - (_age - Seconds) / FadeSeconds);
            if (_age > Seconds + FadeSeconds) Destroy(gameObject);
        }
    }
}
