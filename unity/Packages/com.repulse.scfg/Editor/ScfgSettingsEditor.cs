// The SCFG Settings asset in the editor: its inspector (the default fields plus a Validate button that runs the core's
// validation and shows the code and message) and the build step that puts the project's asset into the player's
// preloaded assets for that build only, so ScfgSettings.Find sees it at startup.
#nullable enable
using System.Collections.Generic;
using System.Linq;
using UnityEditor;
using UnityEditor.Build;
using UnityEditor.Build.Reporting;
using UnityEngine;

namespace Scfg.Editor
{
    [CustomEditor(typeof(ScfgSettings))]
    public sealed class ScfgSettingsEditor : UnityEditor.Editor
    {
        private string? _verdict;
        private MessageType _verdictType;

        public override void OnInspectorGUI()
        {
            var settings = (ScfgSettings)target;
            EditorGUILayout.HelpBox("Applied before the first scene loads, through SCFG.Configure and SCFG.AddItem. A "
                                    + "SCFG.Configure call from your code replaces it.", MessageType.None);
            DrawDefaultInspector();
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
            return ("Valid: the configuration and " + items + (items == 1 ? " item" : " items") + " pass the checks SCFG runs at startup.",
                MessageType.Info);
        }
    }

    /// <summary>Puts the project's SCFG Settings asset into the build's preloaded assets, and takes it out again after.</summary>
    public sealed class ScfgSettingsBuild : IPreprocessBuildWithReport, IPostprocessBuildWithReport
    {
        public int callbackOrder => 0;

        private static ScfgSettings? _added;

        public void OnPreprocessBuild(BuildReport report)
        {
            var settings = ScfgSettings.Find();
            if (settings == null) return;
            var preloaded = PlayerSettings.GetPreloadedAssets().ToList();
            if (preloaded.Contains(settings)) return; // ours from an earlier step, or the developer's own entry
            preloaded.Add(settings);
            PlayerSettings.SetPreloadedAssets(preloaded.ToArray());
            _added = settings;
        }

        public void OnPostprocessBuild(BuildReport report) => Remove();

        /// <summary>Takes back what OnPreprocessBuild added (and any reference left to a deleted asset).</summary>
        public static void Remove()
        {
            var preloaded = PlayerSettings.GetPreloadedAssets();
            var kept = new List<Object>();
            foreach (var o in preloaded)
                if (o != null && !ReferenceEquals(o, _added)) kept.Add(o);
            _added = null;
            if (kept.Count != preloaded.Length) PlayerSettings.SetPreloadedAssets(kept.ToArray());
        }
    }
}
