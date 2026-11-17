// No Python twin: the WebGL player's calls into its page (Godot's ScfgWebPage.BrowserJs, JavaScriptBridge.eval), the C#
// side of ScfgWeb.jslib. IScfgWebJs is what ScfgWebPage and ScfgWebMirror use; the tests give them a fake. Only a WebGL
// player has the real one (ScfgBrowserJs.Create returns null elsewhere, including the editor).
#nullable enable
#if UNITY_WEBGL && !UNITY_EDITOR
using System.Runtime.InteropServices;
#endif

namespace Scfg
{
    /// <summary>The page a WebGL player runs in (see ScfgWeb.jslib for each call's contract).</summary>
    public interface IScfgWebJs
    {
        /// <summary>Runs one of the checkout page's scripts in the page's global scope; false when it threw.</summary>
        bool Load(string code);

        /// <summary>
        /// <c>window.scfgBridge[function](...args)</c> with <paramref name="argsJson"/> a JSON array: the JSON text of the
        /// answer (null is "null"), or null when there is no such function or it threw.
        /// </summary>
        string? Call(string function, string argsJson);

        /// <summary>localStorage: "get" answers {"value": text or null}, "set" answers {}; either {"error": ...}.</summary>
        string? Storage(string op, string key, string? value);

        /// <summary>Queues a write of persistentDataPath to IndexedDB.</summary>
        void SyncFs();

        /// <summary>Does persistentDataPath outlive the tab (is IndexedDB there)?</summary>
        bool FsPersistent();
    }

    /// <summary>The real page (WebGL players only).</summary>
    internal sealed class ScfgBrowserJs : IScfgWebJs
    {
        /// <summary>The page this player runs in, or null when it is not a WebGL player.</summary>
        public static IScfgWebJs? Create()
        {
#if UNITY_WEBGL && !UNITY_EDITOR
            return new ScfgBrowserJs();
#else
            return null;
#endif
        }

#if UNITY_WEBGL && !UNITY_EDITOR
        [DllImport("__Internal")] private static extern int ScfgWebLoad(string code);
        [DllImport("__Internal")] private static extern string ScfgWebCall(string name, string argsJson);
        [DllImport("__Internal")] private static extern string ScfgWebStorage(string op, string key, string value);
        [DllImport("__Internal")] private static extern int ScfgWebSyncFs();
        [DllImport("__Internal")] private static extern int ScfgWebFsPersistent();

        public bool Load(string code) => ScfgWebLoad(code) != 0;
        public string? Call(string function, string argsJson) => ScfgWebCall(function, argsJson);
        public string? Storage(string op, string key, string? value) => ScfgWebStorage(op, key, value ?? "");
        public void SyncFs() => ScfgWebSyncFs();
        public bool FsPersistent() => ScfgWebFsPersistent() != 0;
#else
        public bool Load(string code) => false;
        public string? Call(string function, string argsJson) => null;
        public string? Storage(string op, string key, string? value) => null;
        public void SyncFs() { }
        public bool FsPersistent() => true;
#endif
    }
}
