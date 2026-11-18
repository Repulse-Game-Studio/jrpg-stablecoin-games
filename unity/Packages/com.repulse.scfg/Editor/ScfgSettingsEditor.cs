// The SCFG Settings asset in the editor: its inspector (the default fields plus a Validate button that runs the core's
// validation and shows the code and message; the verdict is cleared by any change or undo, so it never goes stale) and
// the build step that puts the project's asset into the player's preloaded assets for that build only, so
// ScfgSettings.Find sees it at startup.
//
// The build step owns every ScfgSettings entry of Player Settings > Preloaded Assets (do not add one yourself):
// - at a build's start, the list gets exactly the one asset ScfgSettings.Find() uses in the editor, so the player and
//   the editor's play mode apply the same asset; several assets fail the build (Find reports them in play mode too);
// - after the build (OnPostprocessBuild), after a build that ended without one (failed or cancelled: the first editor
//   tick once BuildPipeline.isBuildingPlayer is false) and at every editor load (a build cut short by a crash or a
//   batch-mode exit), every ScfgSettings entry is taken out again. Nothing to remember across a script reload, so
//   ProjectSettings.asset is left as it was in every case the editor survives, and fixed at its next start otherwise.
#nullable enable
using System.Collections.Generic;
using System.Linq;
using UnityEditor;
using UnityEditor.Build;
using UnityEditor.Build.Reporting;
using UnityEngine;

namespace Scfg.Editor
{
    /// <summary>The SCFG Settings inspector: the asset's fields and a Validate button (see the file header).</summary>
    [CustomEditor(typeof(ScfgSettings))]
    public sealed class ScfgSettingsEditor : UnityEditor.Editor
    {
        private string? _verdict;
        private MessageType _verdictType;

        private void OnEnable() => Undo.undoRedoPerformed += ClearVerdict;

        private void OnDisable() => Undo.undoRedoPerformed -= ClearVerdict;

        private void ClearVerdict()
        {
            _verdict = null;
            Repaint();
        }

        /// <inheritdoc/>
        public override void OnInspectorGUI()
        {
            var settings = (ScfgSettings)target;
            EditorGUILayout.HelpBox("Applied before the first scene loads, through SCFG.Configure and SCFG.AddItem. A "
                                    + "SCFG.Configure call from your code replaces it.", MessageType.None);
            if (DrawDefaultInspector()) _verdict = null; // the last verdict was about other values
            EditorGUILayout.Space();
            if (settings.testnet)
                EditorGUILayout.HelpBox("Testnet is on: players pay with test tokens. Turn it off for the release.", MessageType.Warning);
            if (GUILayout.Button("Validate"))
            {
                var (text, type) = Verdict(settings);
                _verdict = text;
                _verdictType = type;
            }
            if (_verdict != null) EditorGUILayout.HelpBox(_verdict, _verdictType);
        }

        /// <summary>What the Validate button shows for <paramref name="settings"/>.</summary>
        public static (string Text, MessageType Type) Verdict(ScfgSettings settings)
        {
            var problem = settings.Validate();
            if (problem != null) return (problem.Code + ": " + problem.Message, MessageType.Error);
            int items = settings.items?.Count(i => i != null) ?? 0;
            var valid = "Valid: the configuration and " + items + (items == 1 ? " item" : " items")
                        + " pass the checks SCFG runs at startup.";
            if (!settings.HasWallet)
                valid += " No wallet is set, so the asset is not applied at startup: the game configures SCFG from code"
                         + " (settings.Apply(wallet) applies it with a wallet supplied at runtime).";
            return (valid, MessageType.Info);
        }
    }

    /// <summary>Puts the project's SCFG Settings asset into the build's preloaded assets, and takes it out again after.</summary>
    public sealed class ScfgSettingsBuild : IPreprocessBuildWithReport, IPostprocessBuildWithReport
    {
        public int callbackOrder => 0;

        public void OnPreprocessBuild(BuildReport report)
        {
            var all = ScfgSettings.FindAll();
            if (all.Count > 1)
                throw new BuildFailedException(all.Count + " SCFG Settings assets are in the project ("
                                               + string.Join(", ", all.Select(ScfgSettings.Describe))
                                               + "): keep one, the one the game should apply.");
            var preloaded = PlayerSettings.GetPreloadedAssets() ?? new Object[0];
            var list = preloaded.Where(o => !(o is ScfgSettings)).ToList();
            if (all.Count == 1) list.Add(all[0]);
            if (!list.SequenceEqual(preloaded)) PlayerSettings.SetPreloadedAssets(list.ToArray());
            if (all.Count == 1)
            {
                EditorApplication.update -= AfterBuild;
                EditorApplication.update += AfterBuild;
            }
        }

        public void OnPostprocessBuild(BuildReport report) => CleanUp();

        // A failed or cancelled build has no postprocess: the editor ticks again once the build is over.
        private static void AfterBuild()
        {
            if (BuildPipeline.isBuildingPlayer) return;
            EditorApplication.update -= AfterBuild;
            CleanUp();
        }

        [InitializeOnLoadMethod]
        private static void OnEditorLoad() => EditorApplication.delayCall += () =>
        {
            if (!BuildPipeline.isBuildingPlayer) CleanUp();
        };

        /// <summary>Takes every SCFG Settings asset out of the preloaded assets (see the file header).</summary>
        public static void CleanUp()
        {
            var preloaded = PlayerSettings.GetPreloadedAssets() ?? new Object[0];
            var kept = new List<Object>();
            foreach (var o in preloaded)
                if (!(o is ScfgSettings)) kept.Add(o);
            if (kept.Count != preloaded.Length) PlayerSettings.SetPreloadedAssets(kept.ToArray());
        }
    }
}
