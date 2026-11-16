// The QR view (Godot's ui/qr_view.gd): Paylinks.QrRects(text) drawn with Painter2D as dark runs on a light square, with
// the quiet zone (border 2) inside the element, scaled to whole pixels per module where it fits so modules stay crisp.
#nullable enable
using System.Collections.Generic;
using Scfg.Core;
using UnityEngine;
using UnityEngine.UIElements;

namespace Scfg
{
    public sealed class ScfgQrElement : VisualElement
    {
        public const int Border = 2;

        private int _size;
        private List<QrRun> _runs = new List<QrRun>();

        /// <summary>The text shown, or null.</summary>
        public string? Text { get; private set; }

        /// <summary>Modules per side, the quiet zone included (0 before SetText).</summary>
        public int Modules => _size;

        public IReadOnlyList<QrRun> Runs => _runs;

        public ScfgQrElement()
        {
            AddToClassList("scfg-qr");
            generateVisualContent += Draw;
            pickingMode = PickingMode.Ignore;
            tooltip = ScfgUiTexts.Get("qr_code");
        }

        /// <summary>Encodes <paramref name="text"/>; false (logged) when it cannot be encoded.</summary>
        public bool SetText(string text)
        {
            try
            {
                var (size, runs) = Paylinks.QrRects(text, Border);
                _size = size;
                _runs = runs;
                Text = text;
            }
            catch (System.Exception e)
            {
                SCFG.ReportError("cannot draw the QR code:", e);
                _size = 0;
                _runs = new List<QrRun>();
                Text = null;
                MarkDirtyRepaint();
                return false;
            }
            MarkDirtyRepaint();
            return true;
        }

        private void Draw(MeshGenerationContext ctx)
        {
            var rect = contentRect;
            float side = Mathf.Min(rect.width, rect.height);
            if (_size <= 0 || side <= 0) return;
            float m = side / _size;
            if (m >= 1f) m = Mathf.Floor(m);
            float ox = rect.x + (rect.width - m * _size) / 2f;
            float oy = rect.y + (rect.height - m * _size) / 2f;
            var p = ctx.painter2D;
            p.fillColor = Color.white;
            FillRect(p, ox, oy, m * _size, m * _size);
            p.fillColor = new Color(0.09f, 0.125f, 0.18f);
            foreach (var run in _runs) FillRect(p, ox + run.X * m, oy + run.Y * m, run.Width * m, m);
        }

        private static void FillRect(Painter2D p, float x, float y, float w, float h)
        {
            p.BeginPath();
            p.MoveTo(new Vector2(x, y));
            p.LineTo(new Vector2(x + w, y));
            p.LineTo(new Vector2(x + w, y + h));
            p.LineTo(new Vector2(x, y + h));
            p.ClosePath();
            p.Fill();
        }
    }
}
