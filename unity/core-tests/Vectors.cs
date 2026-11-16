// No Python twin: locates and reads the shared conformance vectors.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;

namespace Scfg.Core.Tests
{
    public static class Vectors
    {
        /// <summary>Absolute path of conformance/vectors (env SCFG_VECTORS overrides).</summary>
        public static string Root { get; } = Locate();

        private static string Locate()
        {
            var env = Environment.GetEnvironmentVariable("SCFG_VECTORS");
            if (!string.IsNullOrEmpty(env))
            {
                if (!Directory.Exists(env)) throw new DirectoryNotFoundException("SCFG_VECTORS does not exist: " + env);
                return Path.GetFullPath(env);
            }
            return Path.Combine(FindUp("conformance/vectors"), "conformance", "vectors");
        }

        /// <summary>Walks up from the test assembly to the first directory that contains relDir.</summary>
        internal static string FindUp(string relDir)
        {
            var rel = relDir.Replace('/', Path.DirectorySeparatorChar);
            var dir = new DirectoryInfo(AppContext.BaseDirectory);
            while (dir != null)
            {
                if (Directory.Exists(Path.Combine(dir.FullName, rel))) return dir.FullName;
                dir = dir.Parent;
            }
            throw new DirectoryNotFoundException("no ancestor of " + AppContext.BaseDirectory + " contains " + relDir);
        }

        /// <summary>Topic names (file names without .json) under tier1, sorted ordinal.</summary>
        public static IEnumerable<string> Tier1Topics()
        {
            var names = Directory.GetFiles(Path.Combine(Root, "tier1"), "*.json")
                .Select(f => Path.GetFileNameWithoutExtension(f)).ToList();
            names.Sort(StringComparer.Ordinal);
            return names;
        }

        /// <summary>Scenario ids such as "smoke/evm_unlock_paid", sorted ordinal.</summary>
        public static IEnumerable<string> Tier2Ids()
        {
            var baseDir = Path.Combine(Root, "tier2");
            var ids = Directory.GetFiles(baseDir, "*.json", SearchOption.AllDirectories)
                .Select(f => Path.GetRelativePath(baseDir, f).Replace('\\', '/'))
                .Select(f => f.Substring(0, f.Length - ".json".Length)).ToList();
            ids.Sort(StringComparer.Ordinal);
            return ids;
        }

        /// <summary>Reads a UTF-8 file relative to the vectors root.</summary>
        public static string ReadText(string relPath) =>
            File.ReadAllText(Path.Combine(Root, relPath.Replace('/', Path.DirectorySeparatorChar)), new UTF8Encoding(false));
    }
}
