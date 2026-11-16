// Maintainer tools of the package (not for games): run from the dev project with
//     Unity -batchmode -nographics -quit -projectPath unity/Project -executeMethod Scfg.Editor.ScfgMaintenance.MakePanelSettings
#nullable enable
using UnityEditor;
using UnityEngine;
using UnityEngine.UIElements;

namespace Scfg.Editor
{
    public static class ScfgMaintenance
    {
        public const string PanelPath = "Packages/com.repulse.scfg/Runtime/Unity/Resources/ScfgUI/ScfgPanel.asset";

        /// <summary>(Re)makes the package's PanelSettings asset (ScfgDefaultSheetHost.PanelResource) with the default theme.</summary>
        public static void MakePanelSettings()
        {
            var p = ScriptableObject.CreateInstance<PanelSettings>();
            p.themeStyleSheet = AssetDatabase.LoadAssetAtPath<ThemeStyleSheet>("Packages/com.repulse.scfg/Runtime/Unity/Resources/ScfgUI/ScfgTheme.tss");
            p.sortingOrder = ScfgDefaultSheetHost.SortingOrder;
            p.scaleMode = PanelScaleMode.ScaleWithScreenSize;
            p.referenceResolution = new Vector2Int(960, 720);
            p.screenMatchMode = PanelScreenMatchMode.Expand;
            p.clearColor = false;
            AssetDatabase.DeleteAsset(PanelPath);
            AssetDatabase.CreateAsset(p, PanelPath);
            AssetDatabase.SaveAssets();
            Debug.Log("scfg: wrote " + PanelPath);
        }
    }
}
