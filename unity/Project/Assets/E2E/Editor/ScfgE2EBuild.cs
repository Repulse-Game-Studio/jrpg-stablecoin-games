// Builds the e2e's Linux player (tools/run_unity_e2e.py) and its WebGL player (tools/run_unity_web_e2e.py, BuildWebGL):
//
//     Unity -batchmode -nographics -quit -projectPath unity/Project -executeMethod Scfg.E2E.ScfgE2EBuild.BuildLinux \
//           -scfgBackend mono|il2cpp -scfgOut <dir> -logFile <log>
//
// A release (non-development) StandaloneLinux64 player of the empty Assets/E2E/E2E.unity scene (the driver bootstraps
// itself from SCFG_E2E_CONFIG). IL2CPP builds use Managed Stripping Level High, to prove the core and the glue survive
// it with only the package's link.xml (handed over by Scfg.Editor.ScfgLinkXml). The player settings it changes are put
// back afterwards, so ProjectSettings.asset stays as committed. Exit code 0 on success, 1 otherwise; a success also logs
// "Build succeeded!" for GameCI (GameCiSuccess).
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

        /// <summary>
        /// Logged on success only. CI builds through game-ci/unity-builder, whose game-ci/cli fails any build whose log
        /// lacks this exact text (UnityBuildValidation.validateBuild, CLI v0.1.76), as GameCI's own build script prints it.
        /// </summary>
        public const string GameCiSuccess = "Build succeeded!";

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
            if (code == 0) Debug.Log(GameCiSuccess);
            EditorApplication.Exit(code);
        }

        /// <summary>
        /// The e2e's WebGL player (tools/run_unity_web_e2e.py): <c>-executeMethod Scfg.E2E.ScfgE2EBuild.BuildWebGL -scfgOut &lt;dir&gt;</c>.
        /// IL2CPP (WebGL's only backend) with Managed Stripping Level High, uncompressed (any static server serves it), no
        /// data caching, single-threaded; the editor's default template. Exit code 0 on success, 1 otherwise.
        /// </summary>
        public static void BuildWebGL()
        {
            int code = 1;
            try
            {
                code = BuildWeb(Arg("-scfgOut") ?? Path.GetFullPath("Build/e2e-web")) ? 0 : 1;
            }
            catch (Exception e)
            {
                Debug.LogException(e);
            }
            if (code == 0) Debug.Log(GameCiSuccess);
            EditorApplication.Exit(code);
        }

        public static bool BuildWeb(string outDir)
        {
            EnsureScene();
            var target = NamedBuildTarget.WebGL;
            var oldStripping = PlayerSettings.GetManagedStrippingLevel(target);
            var oldProduct = PlayerSettings.productName;
            var oldCompany = PlayerSettings.companyName;
            var oldCompression = PlayerSettings.WebGL.compressionFormat;
            var oldCaching = PlayerSettings.WebGL.dataCaching;
            var oldThreads = PlayerSettings.WebGL.threadsSupport;
            var oldFallback = PlayerSettings.WebGL.decompressionFallback;
            try
            {
                PlayerSettings.SetManagedStrippingLevel(target, ManagedStrippingLevel.High);
                PlayerSettings.productName = Product;
                PlayerSettings.companyName = Company;
                PlayerSettings.WebGL.compressionFormat = WebGLCompressionFormat.Disabled;
                PlayerSettings.WebGL.decompressionFallback = false;
                PlayerSettings.WebGL.dataCaching = false;
                PlayerSettings.WebGL.threadsSupport = false;
                Directory.CreateDirectory(outDir);
                var options = new BuildPlayerOptions
                {
                    scenes = new[] { Scene },
                    locationPathName = outDir,
                    target = BuildTarget.WebGL,
                    options = BuildOptions.None,
                };
                return Report(BuildPipeline.BuildPlayer(options), "webgl", outDir);
            }
            finally
            {
                PlayerSettings.SetManagedStrippingLevel(target, oldStripping);
                PlayerSettings.productName = oldProduct;
                PlayerSettings.companyName = oldCompany;
                PlayerSettings.WebGL.compressionFormat = oldCompression;
                PlayerSettings.WebGL.decompressionFallback = oldFallback;
                PlayerSettings.WebGL.dataCaching = oldCaching;
                PlayerSettings.WebGL.threadsSupport = oldThreads;
                AssetDatabase.SaveAssets();
            }
        }

        private static void EnsureScene()
        {
            if (File.Exists(Scene)) return;
            var scene = EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);
            EditorSceneManager.SaveScene(scene, Scene);
        }

        private static bool Report(BuildReport report, string what, string where)
        {
            var summary = report.summary;
            Debug.Log("scfg-e2e build: " + summary.result + " (" + what + ", " + summary.totalErrors + " errors, "
                      + summary.totalWarnings + " warnings) -> " + where);
            foreach (var step in report.steps)
            foreach (var m in step.messages.Where(m => m.type == LogType.Error || m.type == LogType.Exception))
                Debug.Log("scfg-e2e build error: " + m.content);
            return summary.result == BuildResult.Succeeded;
        }

        public static bool Build(string backend, string outDir)
        {
            EnsureScene();
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
                return Report(BuildPipeline.BuildPlayer(options), backend, options.locationPathName);
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
