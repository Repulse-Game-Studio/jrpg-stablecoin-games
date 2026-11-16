// Builds the e2e's Linux player (tools/run_unity_e2e.py):
//
//     Unity -batchmode -nographics -quit -projectPath unity/Project -executeMethod Scfg.E2E.ScfgE2EBuild.BuildLinux \
//           -scfgBackend mono|il2cpp -scfgOut <dir> -logFile <log>
//
// A release (non-development) StandaloneLinux64 player of the empty Assets/E2E/E2E.unity scene (the driver bootstraps
// itself from SCFG_E2E_CONFIG). IL2CPP builds use Managed Stripping Level High, to prove the core and the glue survive
// it with only the package's link.xml (handed over by Scfg.Editor.ScfgLinkXml). The player settings it changes are put
// back afterwards, so ProjectSettings.asset stays as committed. Exit code 0 on success, 1 otherwise.
#nullable enable
using System;
using System.IO;
using System.Linq;
using UnityEditor;
using UnityEditor.Build;
using UnityEditor.Build.Reporting;
using UnityEditor.SceneManagement;
using UnityEngine;

namespace Scfg.E2E
{
    public static class ScfgE2EBuild
    {
        public const string Scene = "Assets/E2E/E2E.unity";
        public const string Product = "SCFG E2E";
        public const string Company = "Repulse";
        public const string Executable = "scfg-e2e.x86_64";

        private static string? Arg(string name)
        {
            var args = Environment.GetCommandLineArgs();
            int i = Array.IndexOf(args, name);
            return i >= 0 && i + 1 < args.Length ? args[i + 1] : null;
        }

        public static void BuildLinux()
        {
            int code = 1;
            try
            {
                code = Build(Arg("-scfgBackend") ?? "mono", Arg("-scfgOut") ?? Path.GetFullPath("Build/e2e")) ? 0 : 1;
            }
            catch (Exception e)
            {
                Debug.LogException(e);
            }
            EditorApplication.Exit(code);
        }

        public static bool Build(string backend, string outDir)
        {
            if (!File.Exists(Scene))
            {
                var scene = EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);
                EditorSceneManager.SaveScene(scene, Scene);
            }
            var target = NamedBuildTarget.Standalone;
            var il2cpp = backend == "il2cpp";
            var oldBackend = PlayerSettings.GetScriptingBackend(target);
            var oldStripping = PlayerSettings.GetManagedStrippingLevel(target);
            var oldProduct = PlayerSettings.productName;
            var oldCompany = PlayerSettings.companyName;
            var oldDefines = PlayerSettings.GetScriptingDefineSymbols(target);
            try
            {
                PlayerSettings.SetScriptingBackend(target, il2cpp ? ScriptingImplementation.IL2CPP : ScriptingImplementation.Mono2x);
                PlayerSettings.SetManagedStrippingLevel(target, il2cpp ? ManagedStrippingLevel.High : ManagedStrippingLevel.Minimal);
                PlayerSettings.productName = Product;
                PlayerSettings.companyName = Company;
                Directory.CreateDirectory(outDir);
                var options = new BuildPlayerOptions
                {
                    scenes = new[] { Scene },
                    locationPathName = Path.Combine(outDir, Executable),
                    target = BuildTarget.StandaloneLinux64,
                    options = BuildOptions.None,
                };
                var report = BuildPipeline.BuildPlayer(options);
                var summary = report.summary;
                Debug.Log("scfg-e2e build: " + summary.result + " (" + backend + ", " + summary.totalErrors + " errors, "
                          + summary.totalWarnings + " warnings) -> " + options.locationPathName);
                foreach (var step in report.steps)
                foreach (var m in step.messages.Where(m => m.type == LogType.Error || m.type == LogType.Exception))
                    Debug.Log("scfg-e2e build error: " + m.content);
                return summary.result == BuildResult.Succeeded;
            }
            finally
            {
                PlayerSettings.SetScriptingBackend(target, oldBackend);
                PlayerSettings.SetManagedStrippingLevel(target, oldStripping);
                PlayerSettings.productName = oldProduct;
                PlayerSettings.companyName = oldCompany;
                PlayerSettings.SetScriptingDefineSymbols(target, oldDefines);
                AssetDatabase.SaveAssets();
            }
        }
    }
}
