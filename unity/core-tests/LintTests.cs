// No Python twin: the lints that every Runtime/Core file must pass.
//
// THE RULES LIVE HERE. Each lint is a table of (regex, why) below. A failure prints
// "relative/path.cs:line: <pattern> (<why>)". Comments are ignored, and so is the text inside string
// literals (so "record not found" in a message is fine). To opt a single line out of the ToString()
// rule, end it with `// culture-ok: <reason>`.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using NUnit.Framework;

namespace Scfg.Core.Tests
{
    public class LintTests
    {
        private sealed class Rule
        {
            public readonly string Pattern;
            public readonly string Why;
            public readonly Regex Re;
            public Rule(string pattern, string why)
            {
                Pattern = pattern;
                Why = why;
                Re = new Regex(pattern, RegexOptions.CultureInvariant);
            }
        }

        private static Rule[] R(params (string, string)[] rules) =>
            rules.Select(r => new Rule(r.Item1, r.Item2)).ToArray();

        private static readonly Rule[] UnityRules = R(
            (@"\busing\s+UnityEngine\b", "the core must not reference UnityEngine"));

        private static readonly Rule[] ThreadRules = R(
            (@"\bTask\.Run\b", "no threads"),
            (@"\bTask\.Delay\b", "no timers; the clock is injected"),
            (@"\bTask\.Yield\b", "no scheduler hops"),
            (@"\bConfigureAwait\b", "no sync-context games"),
            (@"System\.Threading\.Thread\b", "no threads"),
            (@"\bnew\s+Thread\b", "no threads"),
            (@"\bThreadPool\b", "no threads"),
            (@"System\.Timers", "no timers"),
            (@"\bnew\s+Timer\b", "no timers"),
            (@"\bParallel\.", "no threads"),
            (@"\basync\s+void\b", "async void swallows errors"));

        private static readonly Rule[] ReflectionRules = R(
            (@"System\.Reflection", "IL2CPP stripping breaks reflection"),
            (@"\bdynamic\b\s+[A-Za-z_]", "no dynamic"),
            (@"\bActivator\.", "no reflection"),
            (@"System\.Text\.Json", "use the core's own JSON"),
            (@"\bNewtonsoft\b", "use the core's own JSON"),
            (@"GetType\(\)\s*\.\s*Get", "no reflection"),
            (@"\brecord\s+(class\s+|struct\s+)?[A-Z]", "records need an IsExternalInit shim on netstandard2.1"));

        private static readonly Rule[] CultureRules = R(
            (@"\.Trim\(", "Python str.strip() differs; use Py"),
            (@"\.TrimStart\(", "use Py"),
            (@"\.TrimEnd\(", "use Py"),
            (@"\bchar\.IsWhiteSpace\b", "use Py"),
            (@"\bchar\.IsDigit\b", "use Py"),
            (@"\.ToLower\(", "culture-sensitive; use Py"),
            (@"\.ToUpper\(", "culture-sensitive; use Py"),
            (@"\.ToString\(\s*\)", "pass CultureInfo.InvariantCulture, or end the line with // culture-ok: <reason>"),
            (@"\b(double|long)\.Parse\((?!.*InvariantCulture)", "pass CultureInfo.InvariantCulture"),
            (@"\bStringComparison\.CurrentCulture", "use Ordinal"),
            (@"\bStringComparison\.InvariantCulture", "use Ordinal"));

        // A line is exempt from the ToString() rule only through this marker in its raw (uncommented) text.
        private static readonly Regex CultureOk = new Regex(@"//\s*culture-ok:\s*\S", RegexOptions.CultureInvariant);

        private static string CoreDir()
        {
            var root = Vectors.FindUp("unity/Packages/com.repulse.scfg/Runtime/Core");
            return Path.Combine(root, "unity", "Packages", "com.repulse.scfg", "Runtime", "Core");
        }

        private static List<string> CoreFiles()
        {
            var files = Directory.GetFiles(CoreDir(), "*.cs", SearchOption.AllDirectories).ToList();
            files.Sort(StringComparer.Ordinal);
            Assert.That(files, Is.Not.Empty, "scanned no files under " + CoreDir());
            return files;
        }

        /// <summary>Returns the lines with comments removed and string/char literal contents blanked.</summary>
        internal static string[] Code(string text)
        {
            var lines = text.Replace("\r\n", "\n").Split('\n');
            var result = new string[lines.Length];
            bool inBlock = false, inVerbatim = false;
            for (int n = 0; n < lines.Length; n++)
            {
                var s = lines[n];
                var sb = new StringBuilder();
                for (int i = 0; i < s.Length; i++)
                {
                    char c = s[i];
                    char next = i + 1 < s.Length ? s[i + 1] : '\0';
                    char next2 = i + 2 < s.Length ? s[i + 2] : '\0';
                    if (inBlock)
                    {
                        if (c == '*' && next == '/') { inBlock = false; i++; }
                    }
                    else if (inVerbatim)
                    {
                        if (c == '"' && next == '"') i++;
                        else if (c == '"') { inVerbatim = false; sb.Append('"'); }
                    }
                    else if (c == '/' && next == '/') break;
                    else if (c == '/' && next == '*') { inBlock = true; i++; }
                    else if ((c == '@' && next == '"') || ((c == '$' && next == '@' || c == '@' && next == '$') && next2 == '"'))
                    {
                        inVerbatim = true;
                        sb.Append('"');
                        i += next == '"' ? 1 : 2;
                    }
                    else if (c == '"' || c == '\'')
                    {
                        sb.Append(c);
                        i++;
                        while (i < s.Length && s[i] != c)
                        {
                            if (s[i] == '\\') i++;
                            i++;
                        }
                        sb.Append(c);
                    }
                    else sb.Append(c);
                }
                result[n] = sb.ToString();
            }
            return result;
        }

        private static void Scan(Rule[] rules, bool honourCultureOk = false)
        {
            var core = CoreDir();
            var hits = new List<string>();
            foreach (var file in CoreFiles())
            {
                var raw = File.ReadAllText(file).Replace("\r\n", "\n").Split('\n');
                var code = Code(string.Join("\n", raw));
                var rel = Path.GetRelativePath(core, file).Replace('\\', '/');
                for (int n = 0; n < code.Length; n++)
                {
                    foreach (var rule in rules)
                    {
                        if (!rule.Re.IsMatch(code[n])) continue;
                        if (honourCultureOk && rule.Pattern.Contains("ToString") && CultureOk.IsMatch(raw[n])) continue;
                        hits.Add($"{rel}:{n + 1}: {rule.Pattern} ({rule.Why})");
                    }
                }
            }
            Assert.That(hits, Is.Empty, "lint violations:\n" + string.Join("\n", hits));
        }

        [Test] public void NoUnityEngine() => Scan(UnityRules);
        [Test] public void NoThreadsOrTimers() => Scan(ThreadRules);
        [Test] public void NoReflection() => Scan(ReflectionRules);
        [Test] public void NoCultureSensitiveText() => Scan(CultureRules, honourCultureOk: true);

        [Test]
        public void EveryFileNamesItsTwin()
        {
            var core = CoreDir();
            var bad = new List<string>();
            foreach (var file in CoreFiles())
            {
                var head = File.ReadLines(file).Take(5).ToList();
                if (!head.Any(l => l.Contains("Twin of scfg_core/") || l.Contains("No Python twin")))
                    bad.Add($"{Path.GetRelativePath(core, file).Replace('\\', '/')}:1: missing 'Twin of scfg_core/' or 'No Python twin' in the first 5 lines");
            }
            Assert.That(bad, Is.Empty, string.Join("\n", bad));
        }

        [Test]
        public void VectorsFound()
        {
            Assert.That(Vectors.Tier1Topics().Count(), Is.EqualTo(18));
            Assert.That(Vectors.Tier2Ids().Count(), Is.GreaterThan(100));
        }

        // The lints must actually bite: these guard the matcher itself.
        [TestCase("using UnityEngine;", 1)]
        [TestCase("var x = \"record Not found\"; // dynamic d", 0)]
        [TestCase("public record Foo(int A);", 1)]
        [TestCase("dynamic d = 1;", 1)]
        [TestCase("/* a.Trim() */ var s = 1;", 0)]
        [TestCase("var s = a.Trim();", 1)]
        [TestCase("var s = a.ToString();", 1)]
        [TestCase("var s = a.ToString(CultureInfo.InvariantCulture);", 0)]
        [TestCase("var d = double.Parse(t);", 1)]
        [TestCase("var d = double.Parse(t, CultureInfo.InvariantCulture);", 0)]
        public void MatcherBites(string line, int expected)
        {
            var all = UnityRules.Concat(ThreadRules).Concat(ReflectionRules).Concat(CultureRules);
            var code = Code(line)[0];
            Assert.That(all.Count(r => r.Re.IsMatch(code)), Is.EqualTo(expected));
        }

        [Test]
        public void CultureOkNeedsAReason()
        {
            Assert.That(CultureOk.IsMatch("var s = n.ToString(); // culture-ok: ints only"), Is.True);
            Assert.That(CultureOk.IsMatch("var s = n.ToString(); // culture-ok:"), Is.False);
        }
    }
}
