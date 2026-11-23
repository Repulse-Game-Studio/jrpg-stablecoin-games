// The package's samples (Packages/com.repulse.scfg/Samples~) in the dev project, for tools/check_unity_samples.py, which
// copies them into Assets/Samples/ (git-ignored) first, as the Package Manager's Import button does:
//
//     Unity -batchmode -nographics -quit -projectPath unity/Project -executeMethod Scfg.Maintenance.ScfgSamplesCheck.Check \
//           -logFile <log>
//     Unity ... -executeMethod Scfg.Maintenance.ScfgSamplesCheck.MakeScenes      (maintainers: writes each sample's scene)
//
// Check opens every sample scene and fails (exit 1) when it is missing, has a missing script, or lacks the sample's
// component, or when the package's panel settings the samples copy (ScfgDefaultSheetHost.PanelResource) do not load;
// the scripts compiled, or the editor would not have run this. It also runs each sample's Configure (the scene's
// testnet configuration) with SCFG Settings assets it must accept: a release asset (testnet off) with USDT, which no
// test network carries, and a testnet asset naming the Solana network while the scene has only an EVM wallet. Both
// were refused when the samples took every option of the asset. MakeScenes writes Assets/Samples/<name>/<name>.unity:
// a camera and one GameObject with the sample's component (the tool copies the scene back into Samples~).
#nullable enable
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;

namespace Scfg.Maintenance
{
    public static class ScfgSamplesCheck
    {
        /// <summary>
        /// Logged on success only. CI runs Check as game-ci/unity-builder's build method, and game-ci/cli fails any build
        /// whose log lacks this exact text (UnityBuildValidation.validateBuild, CLI v0.1.76).
        /// </summary>
        private const string GameCiSuccess = "Build succeeded!";

        /// <summary>A valid EVM address (the EIP-55 spec's example), never paid: Configure runs in the editor only.</summary>
        private const string EvmWallet = "0x52908400098527886E0F7030069857D2E4169EE7";

        /// <summary>Each sample: its folder under Samples~ and its component's full type name.</summary>
        public static readonly (string Name, string Type)[] Samples =
        {
            ("Showcase", "Scfg.Samples.Showcase.ScfgShowcase"),
            ("Demo", "Scfg.Samples.Demo.ScfgDemo"),
        };

        private static string ScenePath(string name) => "Assets/Samples/" + name + "/" + name + ".unity";

        /// <summary>A sample's component type by full name, from Unity's TypeCache (the loaded assemblies only, as UAC0005 asks).</summary>
        private static Type? Find(string fullName) =>
            TypeCache.GetTypesDerivedFrom<MonoBehaviour>().FirstOrDefault(t => t.FullName == fullName);

        public static void Check()
        {
            int failures = 0;
            if (Resources.Load<UnityEngine.UIElements.PanelSettings>(Scfg.ScfgDefaultSheetHost.PanelResource) == null)
            {
                Debug.LogError("scfg-samples: the package's panel settings " + Scfg.ScfgDefaultSheetHost.PanelResource + " do not load");
                failures++;
            }
            foreach (var (name, typeName) in Samples)
            {
                var type = Find(typeName);
                if (type == null)
                {
                    Debug.LogError("scfg-samples: " + typeName + " did not compile into the project");
                    failures++;
                    continue;
                }
                if (!File.Exists(ScenePath(name)))
                {
                    Debug.LogError("scfg-samples: no scene " + ScenePath(name));
                    failures++;
                    continue;
                }
                var scene = EditorSceneManager.OpenScene(ScenePath(name), OpenSceneMode.Single);
                int missing = scene.GetRootGameObjects().Sum(go =>
                    go.GetComponentsInChildren<Transform>(true).Sum(t => GameObjectUtility.GetMonoBehavioursWithMissingScriptCount(t.gameObject)));
                int found = scene.GetRootGameObjects().Sum(go => go.GetComponentsInChildren(type, true).Length);
                if (missing > 0 || found != 1)
                {
                    Debug.LogError("scfg-samples: " + ScenePath(name) + " has " + missing + " missing scripts and " + found + " " + type.Name);
                    failures++;
                    continue;
                }
                int refused = CheckConfigure(name, type);
                failures += refused;
                if (refused == 0) Debug.Log("scfg-samples: " + name + " ok");
            }
            if (failures == 0) Debug.Log(GameCiSuccess);
            EditorApplication.Exit(failures == 0 ? 0 : 1);
        }

        /// <summary>
        /// The sample's own Configure(ScfgSettings?, string? evm, string? solana) accepts each asset of
        /// <see cref="Assets"/> with an EVM wallet only; the number of assets it refused.
        /// </summary>
        internal static int CheckConfigure(string name, Type type)
        {
            var configure = type.GetMethod("Configure", BindingFlags.NonPublic | BindingFlags.Static, null,
                new[] { typeof(ScfgSettings), typeof(string), typeof(string) }, null);
            if (configure == null)
            {
                Debug.LogError("scfg-samples: " + name + " has no static Configure(ScfgSettings, string, string)");
                return 1;
            }
            int failures = 0;
            foreach (var (what, settings) in Assets())
            {
                try
                {
                    var err = configure.Invoke(null, new object?[] { settings, EvmWallet, null }) as string;
                    if (err != null)
                    {
                        Debug.LogError("scfg-samples: " + name + "'s Configure refused " + what + ": " + err);
                        failures++;
                    }
                }
                finally
                {
                    UnityEngine.Object.DestroyImmediate(settings);
                }
            }
            return failures;
        }

        /// <summary>SCFG Settings assets (in memory) the samples' testnet Configure must accept.</summary>
        private static IEnumerable<(string What, ScfgSettings Settings)> Assets()
        {
            var release = ScriptableObject.CreateInstance<ScfgSettings>();
            release.testnet = false;
            release.tokens = new List<string> { "USDT" };
            release.onrampChains = new List<string> { "ethereum" };
            yield return ("a release asset with USDT and Pay with card on Ethereum", release);
            var solanaOnly = ScriptableObject.CreateInstance<ScfgSettings>();
            solanaOnly.testnet = true;
            solanaOnly.chains = new List<string> { "solana", "base" };
            solanaOnly.onrampChains = new List<string> { "base" };
            yield return ("a testnet asset with Solana and Base, from a scene with an EVM wallet only", solanaOnly);
            var solana = ScriptableObject.CreateInstance<ScfgSettings>();
            solana.testnet = true;
            solana.chains = new List<string> { "solana" };
            yield return ("a testnet asset with Solana only, from a scene with an EVM wallet only", solana);
        }

        public static void MakeScenes()
        {
            int failures = 0;
            foreach (var (name, typeName) in Samples)
            {
                var type = Find(typeName);
                if (type == null)
                {
                    Debug.LogError("scfg-samples: " + typeName + " did not compile into the project");
                    failures++;
                    continue;
                }
                var scene = EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);
                var camera = new GameObject("Main Camera", typeof(Camera));
                camera.tag = "MainCamera";
                var cam = camera.GetComponent<Camera>();
                cam.clearFlags = CameraClearFlags.SolidColor;
                cam.backgroundColor = Color.black;
                new GameObject("SCFG " + name, type);
                if (!EditorSceneManager.SaveScene(scene, ScenePath(name))) failures++;
                else Debug.Log("scfg-samples: wrote " + ScenePath(name));
            }
            AssetDatabase.SaveAssets();
            EditorApplication.Exit(failures == 0 ? 0 : 1);
        }
    }
}
