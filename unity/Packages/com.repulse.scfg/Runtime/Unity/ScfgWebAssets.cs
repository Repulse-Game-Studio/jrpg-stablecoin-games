// No Python twin: the shared checkout page's files (plugin/game/scfg/web/), copied into Runtime/Unity/Resources/ScfgWeb/
// as .bytes TextAssets by tools/sync_web_page.py. The localhost page serves them (ScfgDesktop); a WebGL build loads them
// into its own page (ScfgWebPage).
#nullable enable
using UnityEngine;

namespace Scfg
{
    internal static class ScfgWebAssets
    {
        /// <summary>The Resources folder of the page's files.</summary>
        public const string Folder = "ScfgWeb/";

        /// <summary>One of the page's files ("app.js", "index.html"), or null.</summary>
        public static byte[]? Load(string name)
        {
            // Resources.Load strips the last extension: "app.js.bytes" is "ScfgWeb/app.js"
            var asset = Resources.Load<TextAsset>(Folder + name);
            return asset != null ? asset.bytes : null;
        }
    }
}
