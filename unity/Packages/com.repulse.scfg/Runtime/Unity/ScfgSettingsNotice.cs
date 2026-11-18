// No Python twin: the line an SCFG Settings asset without a wallet is worth, logged only if the game has not configured
// SCFG by the end of its first frame (Unreal testbed F11, the same in Unity): an asset with items and no wallet is the
// opt-out of a game that configures from code (ScfgSettings.Apply(wallet) with a wallet supplied at runtime, or
// SCFG.Configure), and saying so before that code ran was noise at every start.
#nullable enable
using UnityEngine;

namespace Scfg
{
    /// <summary>Logs the no-wallet line at the end of the first frame unless SCFG is configured by then.</summary>
    internal sealed class ScfgSettingsNotice : MonoBehaviour
    {
        private string _asset = "";

        /// <summary>Arms the check (play mode only: it needs a frame).</summary>
        internal static void Arm(string assetName)
        {
            if (!Application.isPlaying) return;
            var holder = new GameObject("SCFG settings notice") { hideFlags = HideFlags.HideAndDontSave };
            DontDestroyOnLoad(holder);
            holder.AddComponent<ScfgSettingsNotice>()._asset = assetName;
        }

        // LateUpdate of the first frame: after every Awake and Start of the first scene, where games configure.
        private void LateUpdate()
        {
            if (SCFG.Current.Config == null)
                Debug.Log("scfg: the SCFG Settings asset \"" + _asset + "\" has no wallet, so it was not applied at "
                          + "startup, and nothing configured SCFG in the first frame: set a wallet there, or call "
                          + "settings.Apply(wallet) or SCFG.Configure from code.");
            Destroy(gameObject);
        }
    }
}
