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
    public sealed class ScfgDefaultSheetHost : IScfgSheetHost
    {
        public const int SortingOrder = 1000;
        public const string StyleResource = "ScfgUI/ScfgSheet";
        public const string ThemeResource = "ScfgUI/ScfgTheme";

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
            var p = ScriptableObject.CreateInstance<PanelSettings>();
            p.name = "SCFG Panel";
            p.hideFlags = HideFlags.HideAndDontSave;
            p.themeStyleSheet = Resources.Load<ThemeStyleSheet>(ThemeResource);
            p.sortingOrder = SortingOrder;
            p.scaleMode = PanelScaleMode.ScaleWithScreenSize;
            p.referenceResolution = new Vector2Int(960, 720);
            p.screenMatchMode = PanelScreenMatchMode.Expand;
            p.clearColor = false;
            _panel = p;
            return p;
        }

        public IScfgScreen? OpenPayment(ScfgCheckoutController controller) => Open(new ScfgSheetView(controller), "SCFG Payment Sheet");

        public IScfgScreen? OpenRestore(ScfgRestoreController controller) => Open(new ScfgRestoreView(controller), "SCFG Restore");

        private static ScfgScreenView Open(ScfgScreenView view, string name)
        {
            var doc = NewDocument(name, out var go);
            var ticker = go.AddComponent<ScfgScreenTicker>();
            ticker.View = view;
            doc.rootVisualElement.Add(view.Root);
            view.Tick();
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

        public void Close(IScfgScreen screen)
        {
            if (screen is ScfgScreenView view && ScfgScreenTicker.Of(view) is ScfgScreenTicker t && t != null)
            {
                ScfgScreenTicker.Forget(view);
                Destroy(t.gameObject);
            }
        }

        private static void Destroy(GameObject go)
        {
            if (Application.isPlaying) Object.Destroy(go);
            else Object.DestroyImmediate(go);
        }

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
                Debug.LogException(e);
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
    public sealed class ScfgToast : MonoBehaviour
    {
        public const float Seconds = 3f;
        public const float FadeSeconds = 0.3f;

        private VisualElement? _box;
        private float _age;

        /// <summary>The notice's text (tests).</summary>
        public string Text { get; private set; } = "";

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
