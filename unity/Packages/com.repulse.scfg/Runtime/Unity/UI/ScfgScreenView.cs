// What the package's two screens share (ScfgSheetView, ScfgRestoreView), the UI Toolkit twin of Godot's
// ui/screen_base.gd: a full-screen element that dims the game and takes its pointer input, with the light sheet docked
// on the right (the whole width on a portrait or narrow screen), focus wiring for keyboard and gamepad, and small
// builders for labels, buttons, options and fields.
//
// A screen rebuilds its content from Tick() (every frame, from its host) when the subclass's Signature() changes.
//
// Focus: each build lists its focusable controls in visual order. Up and down move along that list, left and right
// within a row, and the ends stay put, so the focus never leaves the screen for the game behind it. This is wired by
// hand on NavigationMoveEvent (sent by UI Toolkit's own input and by the Input System's UI module alike), not left to
// UI Toolkit's geometric navigation. NavigationCancelEvent (Escape, gamepad B) calls OnCancel().
//
// Styling: the classes in Resources/ScfgUI/ScfgSheet.uss. The accent is the USS variable --scfg-accent when a style
// sheet sets it (SCFG.SheetStyle), else Configure's Accent applied inline; --scfg-ink is the text color.
#nullable enable
using System;
using System.Collections.Generic;
using System.Globalization;
using UnityEngine;
using UnityEngine.UIElements;

namespace Scfg
{
    /// <summary>The base of the package's screens (see the file header).</summary>
    public abstract class ScfgScreenView : IScfgScreen
    {
        private static readonly CustomStyleProperty<Color> AccentProperty = new CustomStyleProperty<Color>("--scfg-accent");

        public event Action<string>? Closed;

        /// <summary>The full-screen root: add it to a panel (the default host gives it a UIDocument of its own).</summary>
        public VisualElement Root { get; }

        /// <summary>The docked sheet inside <see cref="Root"/>.</summary>
        protected VisualElement Sheet { get; }

        /// <summary>The sheet's content, rebuilt when the signature changes.</summary>
        protected VisualElement Layout { get; }

        public bool Ended { get; private set; }

        /// <summary>The host sets it when the screen's GameObject is destroyed.</summary>
        public bool Gone { get; internal set; }

        private readonly Dictionary<string, VisualElement> _controls = new Dictionary<string, VisualElement>(StringComparer.Ordinal);
        private readonly Dictionary<string, Action> _actions = new Dictionary<string, Action>(StringComparer.Ordinal);
        private readonly List<VisualElement> _order = new List<VisualElement>();
        private readonly Dictionary<VisualElement, string> _rows = new Dictionary<VisualElement, string>();
        private readonly List<VisualElement> _accentFill = new List<VisualElement>();
        private readonly List<VisualElement> _accentLine = new List<VisualElement>();
        private string? _signature;
        private bool _accentFromStyle;
        private int _builds;

        protected ScfgScreenView()
        {
            Root = new VisualElement { name = "scfg-screen", focusable = false, pickingMode = PickingMode.Position };
            Root.AddToClassList("scfg-screen");
            Root.style.position = Position.Absolute;
            Root.style.left = 0;
            Root.style.top = 0;
            Root.style.right = 0;
            Root.style.bottom = 0;
            var styles = ScfgDefaultSheetHost.LoadStyleSheet();
            if (styles != null) Root.styleSheets.Add(styles);
            var extra = ScfgDefaultSheetHost.SheetStyle;
            if (extra != null) Root.styleSheets.Add(extra);
            Sheet = new VisualElement { name = "scfg-sheet" };
            Sheet.AddToClassList("scfg-sheet");
            Root.Add(Sheet);
            Layout = new VisualElement { name = "scfg-layout" };
            Layout.AddToClassList("scfg-layout");
            Sheet.Add(Layout);
            Root.RegisterCallback<NavigationMoveEvent>(OnNavigationMove, TrickleDown.TrickleDown);
            Root.RegisterCallback<NavigationCancelEvent>(OnNavigationCancel, TrickleDown.TrickleDown);
            Root.RegisterCallback<CustomStyleResolvedEvent>(OnCustomStyle);
            Root.RegisterCallback<GeometryChangedEvent>(_ => DockSheet());
        }

        // --- the subclass's part -----------------------------------------------------------------------

        protected abstract bool ReadyToShow();

        /// <summary>What the current build shows: a change rebuilds the screen.</summary>
        protected abstract string Signature();

        protected abstract void Build();

        /// <summary>The key of the control to focus after a build.</summary>
        protected abstract string Primary();

        /// <summary>Whether a build moved to another step (the focus then goes to the primary control).</summary>
        protected virtual bool Moved(string? before, string after) => before == null;

        /// <summary>Copies the controller's values into the fields the player is not typing in.</summary>
        protected virtual void SyncFields() { }

        protected abstract void OnCancel();

        // --- the frame -------------------------------------------------------------------------------

        /// <summary>One frame: rebuild when the signature changed, then sync the fields.</summary>
        public void Tick()
        {
            if (Ended || !ReadyToShow()) return;
            var sig = Signature();
            if (sig != _signature)
            {
                var before = _signature;
                var keep = FocusedKey();
                _signature = sig;
                Rebuild();
                Focus(Moved(before, sig) ? "" : keep);
            }
            SyncFields();
        }

        /// <summary>How many times the screen was built (tests).</summary>
        public int Builds => _builds;

        private void Rebuild()
        {
            _controls.Clear();
            _actions.Clear();
            _order.Clear();
            _rows.Clear();
            _accentFill.Clear();
            _accentLine.Clear();
            Layout.Clear();
            Build();
            _builds++;
            ApplyAccent();
        }

        /// <summary>The player is done: raise Closed once.</summary>
        public void CloseWith(string how)
        {
            if (Ended) return;
            Ended = true;
            Closed?.Invoke(how);
        }

        // --- registry, for focus and for tests ---------------------------------------------------------

        /// <summary>The control registered under <paramref name="key"/> in the current build, or null.</summary>
        public VisualElement? Control(string key) => _controls.TryGetValue(key, out var c) ? c : null;

        public IReadOnlyCollection<string> Keys => _controls.Keys;

        /// <summary>The focusable controls of the current build, in visual order.</summary>
        public IReadOnlyList<VisualElement> FocusOrder => _order;

        /// <summary>Runs the action of the enabled control <paramref name="key"/> (a button press); false if there is none.</summary>
        public bool Press(string key)
        {
            if (!_actions.TryGetValue(key, out var action) || !_controls.TryGetValue(key, out var c)) return false;
            if (!c.enabledInHierarchy || c.style.display == DisplayStyle.None) return false;
            action();
            return true;
        }

        /// <summary>The text of the label (or button) <paramref name="key"/>, or null.</summary>
        public string? TextOf(string key) => Control(key) is TextElement t ? t.text : null;

        protected T Reg<T>(T c, string key, string? row = null) where T : VisualElement
        {
            _controls[key] = c;
            if (c.focusable)
            {
                _order.Add(c);
                if (row != null) _rows[c] = row;
            }
            return c;
        }

        /// <summary>Takes a registered control out of the focus order (a hidden one keeps its key).</summary>
        protected void Unfocus(VisualElement c)
        {
            c.focusable = false;
            _order.Remove(c);
        }

        private string FocusedKey()
        {
            var focused = Root.focusController?.focusedElement as VisualElement;
            if (focused == null) return "";
            foreach (var kv in _controls)
                if (kv.Value == focused || kv.Value.Contains(focused)) return kv.Key;
            return "";
        }

        /// <summary>The focused control's key in the current build ("" when the focus is elsewhere).</summary>
        public string Focused => FocusedKey();

        private void Focus(string keep)
        {
            VisualElement? c = keep.Length > 0 ? Control(keep) : null;
            if (c == null || !_order.Contains(c)) c = Control(Primary());
            if ((c == null || !_order.Contains(c)) && _order.Count > 0) c = _order[0];
            c?.Focus();
        }

        // --- navigation ------------------------------------------------------------------------------

        private void OnNavigationMove(NavigationMoveEvent evt)
        {
            if (Ended) return;
            var focused = Root.focusController?.focusedElement as VisualElement;
            int i = focused == null ? -1 : IndexOf(focused);
            var dir = evt.direction;
            if (i < 0)
            {
                if (_order.Count > 0) _order[0].Focus();
                Swallow(evt);
                return;
            }
            var current = _order[i];
            bool inField = current is TextField;
            VisualElement target = current;
            switch (dir)
            {
                case NavigationMoveEvent.Direction.Up:
                case NavigationMoveEvent.Direction.Previous:
                    target = i > 0 ? _order[i - 1] : current;
                    break;
                case NavigationMoveEvent.Direction.Down:
                case NavigationMoveEvent.Direction.Next:
                    target = i < _order.Count - 1 ? _order[i + 1] : current;
                    break;
                case NavigationMoveEvent.Direction.Left:
                    if (inField) return; // the caret moves
                    target = SameRow(i, i - 1) ? _order[i - 1] : current;
                    break;
                case NavigationMoveEvent.Direction.Right:
                    if (inField) return;
                    target = SameRow(i, i + 1) ? _order[i + 1] : current;
                    break;
            }
            if (target != current) target.Focus();
            Swallow(evt);
        }

        private int IndexOf(VisualElement focused)
        {
            for (int i = 0; i < _order.Count; i++)
                if (_order[i] == focused || _order[i].Contains(focused)) return i;
            return -1;
        }

        private bool SameRow(int a, int b) =>
            b >= 0 && b < _order.Count && _rows.TryGetValue(_order[a], out var ra) && _rows.TryGetValue(_order[b], out var rb) && ra == rb;

        private void Swallow(EventBase evt)
        {
            evt.StopPropagation();
            Root.focusController?.IgnoreEvent(evt);
        }

        private void OnNavigationCancel(NavigationCancelEvent evt)
        {
            if (Ended) return;
            Swallow(evt);
            OnCancel();
        }

        // --- look ------------------------------------------------------------------------------------

        private void OnCustomStyle(CustomStyleResolvedEvent evt)
        {
            _accentFromStyle = evt.customStyle.TryGetValue(AccentProperty, out _);
            ApplyAccent();
        }

        /// <summary>The accent: the style sheet's --scfg-accent if set, else Configure's.</summary>
        protected Color AccentColor()
        {
            if (Root.customStyle.TryGetValue(AccentProperty, out var c)) return c;
            return ParseColor(SCFG.Accent);
        }

        private void ApplyAccent()
        {
            if (_accentFromStyle) return; // the USS var(--scfg-accent) rules color them
            var accent = ParseColor(SCFG.Accent);
            var on = ParseColor(Scfg.Core.Config.TextOn(SCFG.Accent));
            foreach (var e in _accentFill)
            {
                e.style.backgroundColor = accent;
                if (e is TextElement) e.style.color = on;
            }
            foreach (var e in _accentLine)
            {
                e.style.borderLeftColor = accent;
                e.style.borderRightColor = accent;
                e.style.borderTopColor = accent;
                e.style.borderBottomColor = accent;
            }
        }

        public static Color ParseColor(string hex) =>
            ColorUtility.TryParseHtmlString(hex, out var c) ? c : new Color(0.055f, 0.486f, 0.455f);

        /// <summary>A docked sheet 520 design pixels wide, or the whole width on a portrait or narrow screen.</summary>
        private void DockSheet()
        {
            var size = Root.layout.size;
            if (float.IsNaN(size.x) || size.x <= 0) return;
            bool full = size.x < size.y || 520f * 1.6f > size.x;
            Sheet.EnableInClassList("scfg-sheet--full", full);
        }

        // --- builders --------------------------------------------------------------------------------

        protected static VisualElement Box(string cls)
        {
            var b = new VisualElement();
            b.AddToClassList(cls);
            return b;
        }

        protected static VisualElement Row(string cls = "scfg-row")
        {
            var b = new VisualElement();
            b.AddToClassList(cls);
            return b;
        }

        protected static Label Text(string text, string cls = "scfg-text")
        {
            var l = new Label(text);
            l.AddToClassList("scfg-text");
            if (cls != "scfg-text") l.AddToClassList(cls);
            l.enableRichText = false;
            return l;
        }

        /// <summary>An address, amount or hash (the Copy buttons next to it copy it; it is not in the focus order).</summary>
        protected static Label Mono(string text) => Text(text, "scfg-mono");

        protected Button Button(string key, string text, string cls, Action onPress, string? row = null)
        {
            var b = new Button(onPress) { text = text };
            b.AddToClassList("scfg-button");
            if (cls.Length > 0) b.AddToClassList(cls);
            b.enableRichText = false;
            if (cls == "scfg-button--accent" || cls == "scfg-button--primary") _accentFill.Add(b);
            _actions[key] = onPress;
            return Reg(b, key, row);
        }

        /// <summary>A selectable option: a title, an optional sub line, and the accent outline when selected.</summary>
        protected Button Option(string key, string text, string sub, bool selected, Action onPress, string? row = null)
        {
            var b = new Button(onPress);
            b.AddToClassList("scfg-option");
            if (selected)
            {
                b.AddToClassList("scfg-option--selected");
                _accentLine.Add(b);
            }
            var title = Text(text, "scfg-option__text");
            title.pickingMode = PickingMode.Ignore;
            b.Add(title);
            if (sub.Length > 0)
            {
                var s = Text(sub, "scfg-option__sub");
                s.pickingMode = PickingMode.Ignore;
                b.Add(s);
            }
            _actions[key] = onPress;
            return Reg(b, key, row);
        }

        /// <summary>A text field that takes only <paramref name="allowed"/> characters, at most <paramref name="maxLength"/>.</summary>
        protected TextField Field(string key, string value, int maxLength, string allowed, Action<string> onText, Action onSubmit,
                                  string? row = null)
        {
            var f = new TextField { maxLength = maxLength, isDelayed = false };
            f.AddToClassList("scfg-field");
            f.SetValueWithoutNotify(value);
            f.RegisterValueChangedCallback(evt =>
            {
                var clean = Filter(evt.newValue ?? "", allowed, maxLength);
                if (clean != evt.newValue) f.SetValueWithoutNotify(clean);
                onText(clean);
            });
            f.RegisterCallback<KeyDownEvent>(evt =>
            {
                if (evt.keyCode == KeyCode.Return || evt.keyCode == KeyCode.KeypadEnter)
                {
                    evt.StopPropagation();
                    onSubmit();
                }
            }, TrickleDown.TrickleDown);
            _actions[key] = onSubmit;
            return Reg(f, key, row);
        }

        internal static string Filter(string text, string allowed, int maxLength)
        {
            var output = new System.Text.StringBuilder();
            foreach (var ch in text)
            {
                if (output.Length >= maxLength) break;
                if (allowed.IndexOf(ch) >= 0) output.Append(ch);
            }
            return output.ToString();
        }

        protected static VisualElement Rule() => Box("scfg-rule");

        protected VisualElement Dot(bool filled)
        {
            var d = Box("scfg-dot");
            if (filled)
            {
                d.AddToClassList("scfg-dot--on");
                _accentFill.Add(d);
            }
            return d;
        }

        protected VisualElement StatusRow(string text, string tone, string key)
        {
            var row = Row("scfg-status");
            var dot = Box("scfg-status__dot");
            dot.AddToClassList("scfg-tone--" + tone);
            if (tone == "accent") _accentFill.Add(dot);
            row.Add(dot);
            var l = Text(text, "scfg-status__text");
            l.name = "text";
            row.Add(l);
            return Reg(row, key);
        }

        /// <summary>The configured logo, or null.</summary>
        protected static VisualElement? Logo()
        {
            var tex = SCFG.LogoTexture;
            if (tex == null) return null;
            var img = new Image { image = tex, scaleMode = ScaleMode.ScaleToFit };
            img.AddToClassList("scfg-logo");
            return img;
        }

        protected VisualElement ScrollingBody(VisualElement parent)
        {
            var scroll = new ScrollView(ScrollViewMode.Vertical);
            scroll.AddToClassList("scfg-body");
            scroll.horizontalScrollerVisibility = ScrollerVisibility.Hidden;
            parent.Add(scroll);
            return scroll.contentContainer;
        }

        protected static string Int(long n) => n.ToString(CultureInfo.InvariantCulture);
    }
}
