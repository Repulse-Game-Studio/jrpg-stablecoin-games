// Unity ignores a link.xml inside a package, so this hands the package's Runtime/link.xml to the managed linker of every
// player build (Mono and IL2CPP, any stripping level): it keeps Scfg.Core and Scfg.Desktop whole and the
// RNGCryptoServiceProvider the core constructs directly (see the file's own comment).
#nullable enable
using System.IO;
using UnityEditor;
using UnityEditor.Build;
using UnityEditor.Build.Reporting;
using UnityEditor.UnityLinker;

namespace Scfg.Editor
{
    /// <summary>Hands the package's Runtime/link.xml to the managed linker of every player build (see the file header).</summary>
    public sealed class ScfgLinkXml : IUnityLinkerProcessor
    {
        /// <inheritdoc/>
        public int callbackOrder => 0;

        /// <summary>The package's Runtime/link.xml as a full path, or null when the package cannot be found.</summary>
        public static string? PackageLinkXml()
        {
            var info = UnityEditor.PackageManager.PackageInfo.FindForAssembly(typeof(ScfgLinkXml).Assembly);
            var root = info != null ? info.resolvedPath : null;
            if (string.IsNullOrEmpty(root)) return null;
            var path = Path.GetFullPath(Path.Combine(root!, "Runtime", "link.xml"));
            return File.Exists(path) ? path : null;
        }

        /// <inheritdoc/>
        public string GenerateAdditionalLinkXmlFile(BuildReport report, UnityLinkerBuildPipelineData data)
        {
            var path = PackageLinkXml();
            if (path == null)
            {
                UnityEngine.Debug.LogError("scfg: the package's Runtime/link.xml was not found; IL2CPP may strip the payment core");
                return "";
            }
            return path;
        }
    }
}
