// No Python twin: the hidden MonoBehaviour that drives the facade (Godot's autoload _process). Created on first use with
// DontDestroyOnLoad and HideFlags.HideAndDontSave; its Update pumps SCFG every frame (Update runs at Time.timeScale = 0
// too). Application.quitting shuts the facade down: waiting calls resolve without delivering, pages close, the state
// is flushed.
#nullable enable
using UnityEngine;

namespace Scfg
{
    [AddComponentMenu("")]
    internal sealed class ScfgRunner : MonoBehaviour
    {
        private static ScfgRunner? _instance;
        private static bool _hooked;

        /// <summary>The runner, made on first use (play mode only).</summary>
        public static void Ensure()
        {
            if (_instance != null) return;
            if (!_hooked)
            {
                Application.quitting += OnQuitting;
                _hooked = true;
            }
            var go = new GameObject("SCFG Runner") { hideFlags = HideFlags.HideAndDontSave };
            DontDestroyOnLoad(go);
            _instance = go.AddComponent<ScfgRunner>();
        }

        private static void OnQuitting() => SCFG.Shutdown();

        /// <summary>Domain reload off: unhook and drop the last session's runner.</summary>
        internal static void ResetStatics()
        {
            if (_hooked) Application.quitting -= OnQuitting;
            _hooked = false;
            if (_instance != null) Destroy(_instance.gameObject);
            _instance = null;
        }

        private void Update() => SCFG.Pump();

        private void OnDestroy()
        {
            if (_instance == this) _instance = null;
        }
    }
}
