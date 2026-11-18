// No Python twin: the WebGL player's calls into its page (Godot's ScfgWebPage.BrowserJs, JavaScriptBridge.eval), the C#
// side of ScfgWeb.jslib. IScfgWebJs is what ScfgWebPage and ScfgWebMirror use; the tests give them a fake. Only a WebGL
// player has the real one (ScfgBrowserJs.Create returns null elsewhere, including the editor).
#nullable enable
#if UNITY_WEBGL && !UNITY_EDITOR
using System;
using System.Reflection;
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

        /// <summary>Does persistentDataPath outlive the tab (did Unity's IndexedDB database open)?</summary>
        bool FsPersistent();

        /// <summary>Is a checkout overlay shown in the page (an element [data-scfg-overlay] that is not hidden)?</summary>
        bool OverlayShown();

        /// <summary>
        /// WebGLInput.captureAllKeyboardInput: the player takes every key of the page (true, Unity's default), or only
        /// those sent to its canvas while it has focus (false).
        /// </summary>
        bool CaptureAllKeyboardInput { get; set; }
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
        [DllImport("__Internal")] private static extern int ScfgWebOverlayShown();

        public bool Load(string code) => ScfgWebLoad(code) != 0;
        public string? Call(string function, string argsJson) => ScfgWebCall(function, argsJson);
        public string? Storage(string op, string key, string? value) => ScfgWebStorage(op, key, value ?? "");
        public void SyncFs() => ScfgWebSyncFs();
        public bool FsPersistent() => ScfgWebFsPersistent() != 0;
        public bool OverlayShown() => ScfgWebOverlayShown() != 0;

        // UnityEngine.WebGLInput through reflection, never by name: a WebGL player built by BuildPipeline.BuildPlayer from
        // an editor whose active platform is another (a Linux-active CI project) compiles without the WebGL module's
        // reference, and a direct use is CS1069 there. Runtime/link.xml keeps the type through Managed Stripping High.
        private static readonly PropertyInfo? Capture = Type.GetType("UnityEngine.WebGLInput, UnityEngine.WebGLModule")
            ?.GetProperty("captureAllKeyboardInput", BindingFlags.Public | BindingFlags.Static);
        private static bool _warned;

        public bool CaptureAllKeyboardInput
        {
            get => Capture?.GetValue(null) is bool on ? on : true;
            set
            {
                if (Capture != null)
                {
                    Capture.SetValue(null, value);
                }
                else if (!_warned)
                {
                    _warned = true;
                    UnityEngine.Debug.LogWarning("scfg: UnityEngine.WebGLInput.captureAllKeyboardInput was not found; "
                                                 + "the page's text fields may not get the keys the game takes");
                }
            }
        }
#else
        public bool Load(string code) => false;
        public string? Call(string function, string argsJson) => null;
        public string? Storage(string op, string key, string? value) => null;
        public void SyncFs() { }
        public bool FsPersistent() => true;
        public bool OverlayShown() => false;
        public bool CaptureAllKeyboardInput { get => true; set { } }
#endif
    }
}
