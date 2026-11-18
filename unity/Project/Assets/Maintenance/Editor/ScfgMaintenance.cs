// Maintainer tools of the package, in the dev project (games never get them): run with
//     Unity -batchmode -nographics -quit -projectPath unity/Project -executeMethod Scfg.Maintenance.ScfgMaintenance.MakePanelSettings
// The package folder is writable only here, through the dev project's file: dependency on it (an installed package is
// read-only).
#nullable enable
using UnityEditor;
using UnityEngine;
using UnityEngine.UIElements;

namespace Scfg.Maintenance
{
    public static class ScfgMaintenance
    {
        /// <summary>The package's PanelSettings asset (ScfgDefaultSheetHost.PanelResource).</summary>
        public const string PanelPath = "Packages/com.repulse.scfg/Runtime/Unity/Resources/ScfgUI/ScfgPanel.asset";

        /// <summary>The package's default theme, which the panel uses.</summary>
        public const string ThemePath = "Packages/com.repulse.scfg/Runtime/Unity/Resources/ScfgUI/ScfgTheme.tss";

        /// <summary>(Re)makes the package's PanelSettings asset with the default theme, set up as the package's panel.</summary>
        public static void MakePanelSettings()
        {
            var p = ScriptableObject.CreateInstance<PanelSettings>();
            p.themeStyleSheet = AssetDatabase.LoadAssetAtPath<ThemeStyleSheet>(ThemePath);
            ScfgDefaultSheetHost.ConfigurePanel(p);
            AssetDatabase.DeleteAsset(PanelPath);
            AssetDatabase.CreateAsset(p, PanelPath);
            AssetDatabase.SaveAssets();
            Debug.Log("scfg-maintenance: wrote " + PanelPath);
        }
    }
}
