// The package's samples (Packages/com.repulse.scfg/Samples~) in the dev project, for tools/check_unity_samples.py, which
// copies them into Assets/Samples/ (git-ignored) first, as the Package Manager's Import button does:
//
//     Unity -batchmode -nographics -quit -projectPath unity/Project -executeMethod ScfgSamplesCheck.Check -logFile <log>
//     Unity ... -executeMethod ScfgSamplesCheck.MakeScenes      (maintainers: writes each sample's scene)
//
// Check opens every sample scene and fails (exit 1) when it is missing, has a missing script, or lacks the sample's
// component; the scripts compiled, or the editor would not have run this. MakeScenes writes Assets/Samples/<name>/<name>.unity:
// a camera and one GameObject with the sample's component (the tool copies the scene back into Samples~).
#nullable enable
using System;
using System.IO;
using System.Linq;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;

public static class ScfgSamplesCheck
{
    /// <summary>Each sample: its folder under Samples~ and its component's full type name.</summary>
    public static readonly (string Name, string Type)[] Samples =
    {
        ("Showcase", "Scfg.Samples.Showcase.ScfgShowcase"),
        ("Demo", "Scfg.Samples.Demo.ScfgDemo"),
    };

    private static string ScenePath(string name) => "Assets/Samples/" + name + "/" + name + ".unity";

    private static Type? Find(string fullName) =>
        AppDomain.CurrentDomain.GetAssemblies().Select(a => a.GetType(fullName, false)).FirstOrDefault(t => t != null);

    public static void Check()
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
            Debug.Log("scfg-samples: " + name + " ok");
        }
        // CI runs Check as game-ci/unity-builder's build method, and game-ci/cli fails any build whose log lacks this
        // exact text (UnityBuildValidation.validateBuild, CLI v0.1.76). Only on success.
        if (failures == 0) Debug.Log("Build succeeded!");
        EditorApplication.Exit(failures == 0 ? 0 : 1);
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
