// No Python twin: the lints that every Runtime/Core file must pass.
//
// THE RULES LIVE HERE. Each lint is a table of (regex, why) below. A failure prints
// "relative/path.cs:line: <pattern> (<why>)". Comments are ignored, and so is the text of string literals
// (so "record not found" in a message is fine), but code inside the holes of an interpolated string
// ($"{a.Trim()}") IS scanned. Rules marked raw are matched against the line with literals kept (used for
// \d / \s inside a Regex). To opt a single line out of a ToString()/Format rule, end it with
// `// culture-ok: <reason>`.
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
            public readonly bool Raw;
            public readonly Regex Re;
            public Rule(string pattern, string why, bool raw = false)
            {
                Pattern = pattern;
                Why = why;
                Raw = raw;
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
            (@"\bTask\.Factory\b", "no task scheduling; Task<T> is only a promise"),
            (@"\bnew\s+Task\b", "no task scheduling; use TaskCompletionSource as a promise"),
            (@"\bConfigureAwait\b", "no sync-context games"),
            (@"System\.Threading\.Thread\b", "no threads"),
            (@"\bThread\.", "no threads (Sleep, Yield, ...)"),
            (@"\bnew\s+Thread\b", "no threads"),
            (@"\bThreadPool\b", "no threads"),
            (@"System\.Timers", "no timers"),
            (@"System\.Threading\.Timer\b", "no timers"),
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
            (@"\b[Cc]har\.IsWhiteSpace\b", "use Py"),
            (@"\b[Cc]har\.IsDigit\b", "use Py"),
            (@"\b[Cc]har\.(?:IsLetter|IsLetterOrDigit|IsUpper|IsLower|IsNumber|IsPunctuation|IsSymbol|IsSeparator)\b",
                "Unicode/culture tables differ from Python; use Py"),
            (@"\.ToLower\(", "culture-sensitive; use Py"),
            (@"\.ToUpper\(", "culture-sensitive; use Py"),
            (@"\.ToString\(\s*\)", "pass CultureInfo.InvariantCulture, or end the line with // culture-ok: <reason>"),
            (@"\.ToString\(\s*""(?![^)]*InvariantCulture)", "a format string needs CultureInfo.InvariantCulture"),
            (@"\b[Ss]tring\.Format\((?!\s*CultureInfo\.InvariantCulture)", "pass CultureInfo.InvariantCulture first"),
            // BCL numeric types only, keyword and CLR spellings (R6, R6b): the core's own Parse APIs (Dec.Parse)
            // are culture-free.
            (@"\b(s?byte|u?short|u?int|u?long|float|double|decimal|BigInteger|S?Byte|U?Int(16|32|64)|Single|Double|Decimal)\.(Try)?Parse\((?!.*InvariantCulture)",
                "Parse/TryParse must pass CultureInfo.InvariantCulture"),
            (@"\.(?:StartsWith|EndsWith)\((?!.*StringComparison\.Ordinal\b)", "culture-sensitive by default; pass StringComparison.Ordinal"),
            (@"\.(?:Last)?IndexOf\((?!.*StringComparison\.Ordinal\b)", "culture-sensitive by default; pass StringComparison.Ordinal"),
            (@"\b[Ss]tring\.Compare\((?!.*StringComparison\.Ordinal\b)", "use string.CompareOrdinal or StringComparison.Ordinal"),
            (@"\.CompareTo\(", "culture-sensitive for strings; use string.CompareOrdinal"),
            (@"\bStringComparison\.CurrentCulture", "use Ordinal"),
            (@"\bStringComparison\.InvariantCulture", "use Ordinal"))
            .Concat(new[]
            {
                new Rule(@"\bRegex\b.*\\[dDsS]", "\\d and \\s are Unicode-aware in .NET; spell the class out or use Py", raw: true),
            })
            .ToArray();

        private static readonly Rule[] AllRules =
            UnityRules.Concat(ThreadRules).Concat(ReflectionRules).Concat(CultureRules).ToArray();

        // A line is exempt from the ToString()/Format rules only through this marker in its raw text.
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

        private sealed class Frame
        {
            public bool IsString, Verbatim, Interp;
            public int Depth; // brace depth inside an interpolation hole
        }

        /// <summary>
        /// Returns the lines with comments removed. String and char literal contents are blanked unless
        /// keepStrings; code inside interpolation holes is always kept.
        /// </summary>
        internal static string[] Code(string text, bool keepStrings = false)
        {
            var t = text.Replace("\r\n", "\n");
            var o = new StringBuilder(t.Length);
            var stack = new Stack<Frame>();
            bool inBlock = false;
            int i = 0;
            void Lit(string s) { if (keepStrings) o.Append(s); }
            while (i < t.Length)
            {
                char c = t[i];
                char n1 = i + 1 < t.Length ? t[i + 1] : '\0';
                var top = stack.Count > 0 ? stack.Peek() : null;
                if (inBlock)
                {
                    if (c == '*' && n1 == '/') { inBlock = false; i += 2; }
                    else { if (c == '\n') o.Append('\n'); i++; }
                    continue;
                }
                if (top != null && top.IsString)
                {
                    if (c == '\n')
                    {
                        o.Append('\n'); i++;
                        if (!top.Verbatim) stack.Pop(); // unterminated
                        continue;
                    }
                    if (top.Verbatim && c == '"' && n1 == '"') { Lit("\"\""); i += 2; continue; }
                    if (!top.Verbatim && c == '\\' && n1 != '\n' && n1 != '\0') { Lit("\\" + n1); i += 2; continue; }
                    if (c == '"') { stack.Pop(); o.Append('"'); i++; continue; }
                    if (top.Interp)
                    {
                        if ((c == '{' && n1 == '{') || (c == '}' && n1 == '}')) { Lit(new string(c, 2)); i += 2; continue; }
                        if (c == '{') { o.Append('{'); stack.Push(new Frame()); i++; continue; }
                    }
                    Lit(c.ToString());
                    i++;
                    continue;
                }
                // code: top level or inside an interpolation hole
                if (c == '/' && n1 == '/') { while (i < t.Length && t[i] != '\n') i++; continue; }
                if (c == '/' && n1 == '*') { inBlock = true; i += 2; continue; }
                int j = i;
                bool dollar = false, at = false;
                while (j < t.Length && j - i < 2 && (t[j] == '$' || t[j] == '@'))
                {
                    if (t[j] == '$') dollar = true; else at = true;
                    j++;
                }
                if (j < t.Length && t[j] == '"')
                {
                    stack.Push(new Frame { IsString = true, Verbatim = at, Interp = dollar });
                    o.Append('"');
                    i = j + 1;
                    continue;
                }
                if (c == '\'')
                {
                    o.Append('\'');
                    i++;
                    while (i < t.Length && t[i] != '\'' && t[i] != '\n')
                    {
                        if (t[i] == '\\') i++;
                        i++;
                    }
                    o.Append('\'');
                    i++;
                    continue;
                }
                if (top != null)
                {
                    if (c == '{') top.Depth++;
                    else if (c == '}')
                    {
                        if (top.Depth == 0) { stack.Pop(); o.Append('}'); i++; continue; }
                        top.Depth--;
                    }
                }
                o.Append(c);
                i++;
            }
            return o.ToString().Split('\n');
        }

        private static List<(int Line, Rule Rule)> Violations(string text, Rule[] rules, bool honourCultureOk)
        {
            var raw = text.Replace("\r\n", "\n").Split('\n');
            var code = Code(text);
            var kept = Code(text, keepStrings: true);
            var hits = new List<(int, Rule)>();
            for (int n = 0; n < code.Length; n++)
            {
                foreach (var rule in rules)
                {
                    if (!rule.Re.IsMatch(rule.Raw ? kept[n] : code[n])) continue;
                    bool formatRule = rule.Pattern.Contains("ToString") || rule.Pattern.Contains("tring");
                    if (honourCultureOk && formatRule && CultureOk.IsMatch(raw[n])) continue;
                    hits.Add((n + 1, rule));
                }
            }
            return hits;
        }

        private static void Scan(Rule[] rules, bool honourCultureOk = false)
        {
            var core = CoreDir();
            var hits = new List<string>();
            foreach (var file in CoreFiles())
            {
                var rel = Path.GetRelativePath(core, file).Replace('\\', '/');
                foreach (var (line, rule) in Violations(File.ReadAllText(file), rules, honourCultureOk))
                    hits.Add($"{rel}:{line}: {rule.Pattern} ({rule.Why})");
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

        // Unity has no project-level nullable setting, so each file turns annotations on itself. The directive must
        // come after the header comment and before any other line (a using, the namespace).
        private static bool EnablesNullableFirst(string text)
        {
            foreach (var raw in text.Replace("\r", "").Split('\n'))
            {
                var line = raw.TrimStart('\uFEFF').Trim();
                if (line.Length == 0 || line.StartsWith("//", StringComparison.Ordinal)) continue;
                return line == "#nullable enable";
            }
            return false;
        }

        [Test]
        public void EveryFileEnablesNullable()
        {
            var core = CoreDir();
            var bad = new List<string>();
            foreach (var file in CoreFiles())
                if (!EnablesNullableFirst(File.ReadAllText(file)))
                    bad.Add($"{Path.GetRelativePath(core, file).Replace('\\', '/')}: '#nullable enable' must be the first line after the header comment");
            Assert.That(bad, Is.Empty, string.Join("\n", bad));
        }

        [TestCase("// header\n#nullable enable\nusing System;\n", true)]
        [TestCase("\uFEFF// header\r\n#nullable enable\r\nnamespace A {}\r\n", true)]
        [TestCase("// header\nusing System;\n#nullable enable\n", false)]
        [TestCase("// header\nusing System;\n", false)]
        [TestCase("// header\n#nullable disable\nusing System;\n", false)]
        [TestCase("", false)]
        public void NullableLintBites(string text, bool expected) =>
            Assert.That(EnablesNullableFirst(text), Is.EqualTo(expected));

        [Test]
        public void VectorsFound()
        {
            Assert.That(Vectors.Tier1Topics().Count(), Is.EqualTo(18));
            Assert.That(Vectors.Tier2Ids().Count(), Is.GreaterThan(100));
        }

        // The lints must actually bite. Each case names a fragment of the ONE rule that must fire on it
        // (empty = no rule may fire), so a line caught by the wrong rule fails.
        [TestCase("using UnityEngine;", "UnityEngine")]
        [TestCase("var x = \"record Not found\"; // dynamic d", "")]
        [TestCase("public record Foo(int A);", "record")]
        [TestCase("dynamic d = 1;", "dynamic")]
        [TestCase("/* a.Trim() */ var s = 1;", "")]
        [TestCase("var s = a.Trim();", @"\.Trim\(")]
        [TestCase("var s = a.ToString();", @"\.ToString\(\s*\)")]
        [TestCase("var s = a.ToString(CultureInfo.InvariantCulture);", "")]
        [TestCase("var s = a.ToString(\"F2\");", @"ToString\(\s*""")]
        [TestCase("var s = a.ToString(\"F2\", CultureInfo.InvariantCulture);", "")]
        [TestCase("var s = string.Format(\"{0}\", a);", @"tring\.Format")]
        [TestCase("var s = string.Format(CultureInfo.InvariantCulture, \"{0}\", a);", "")]
        [TestCase("var d = double.Parse(t);", "Parse")]
        [TestCase("var d = double.Parse(t, CultureInfo.InvariantCulture);", "")]
        [TestCase("var d = int.Parse(t);", "Parse")]
        [TestCase("var d = decimal.Parse(t);", "Parse")]
        [TestCase("var d = float.Parse(t);", "Parse")]
        [TestCase("var d = ulong.Parse(t);", "Parse")]
        [TestCase("var ok = int.TryParse(t, out var v);", "Parse")]
        [TestCase("var ok = BigInteger.TryParse(t, NumberStyles.None, CultureInfo.InvariantCulture, out var v);", "")]
        [TestCase("var b = BigInteger.Parse(t);", "Parse")]
        [TestCase("var n = Int32.Parse(t);", "Parse")] // the CLR spellings too (R6b)
        [TestCase("var d = Double.Parse(t);", "Parse")]
        [TestCase("var ok = UInt64.TryParse(t, out x);", "Parse")]
        [TestCase("var n = Int64.Parse(t, CultureInfo.InvariantCulture);", "")]
        [TestCase("var d = Dec.Parse(t);", "")] // the core's own Parse APIs are culture-free (R6)
        [TestCase("var d = Dec.TryParse(t, out var v);", "")]
        [TestCase("var b = s.StartsWith(\"x\");", "StartsWith")]
        [TestCase("var b = s.StartsWith(\"x\", StringComparison.Ordinal);", "")]
        [TestCase("var b = s.StartsWith(\"x\", StringComparison.OrdinalIgnoreCase);", "StartsWith")]
        [TestCase("var b = s.EndsWith(\"x\");", "EndsWith")]
        [TestCase("var b = s.EndsWith(\"x\", StringComparison.Ordinal);", "")]
        [TestCase("var b = s.IndexOf(\"x\");", "IndexOf")]
        [TestCase("var b = s.LastIndexOf(\"x\");", "IndexOf")]
        [TestCase("var b = s.IndexOf(\"x\", StringComparison.Ordinal);", "")]
        [TestCase("var b = string.Compare(a, b);", "Compare")]
        [TestCase("var b = string.Compare(a, b, StringComparison.Ordinal);", "")]
        [TestCase("var b = a.CompareTo(b);", "CompareTo")]
        [TestCase("var b = char.IsLetter(c);", "IsLetter")]
        [TestCase("var b = char.IsUpper(c);", "IsLetter")]
        [TestCase("var b = char.ToLower(c);", @"\.ToLower\(")]
        [TestCase("var b = char.ToLowerInvariant(c);", "")]
        [TestCase("var r = new Regex(@\"\\d+\");", @"\\[dDsS]")]
        [TestCase("var r = new Regex(\"\\\\s*\");", @"\\[dDsS]")]
        [TestCase("var s = \"\\\\d\";", "")]
        [TestCase("var s = $\"{a.Trim()}\";", @"\.Trim\(")]
        [TestCase("var s = $\"{x.ToString()}\";", @"\.ToString\(\s*\)")]
        [TestCase("var s = $\"a.Trim() {n}\";", "")]
        [TestCase("var s = $\"{{a.Trim()}}\";", "")]
        [TestCase("var s = $\"{(a ? \"x\" : \"y\")}\"; var t = b.Trim();", @"\.Trim\(")]
        [TestCase("var s = $@\"{(a ? \"x\" : \"y\")} \"\"q\"\"\"; var t = b.Trim();", @"\.Trim\(")]
        [TestCase("Thread.Sleep(5);", @"\bThread\.")]
        [TestCase("Thread.Yield();", @"\bThread\.")]
        [TestCase("var t = new System.Threading.Timer(cb);", "Threading")]
        [TestCase("var t = new System.Timers.Timer();", "System")]
        [TestCase("Task.Factory.StartNew(f);", "Factory")]
        [TestCase("var t = new Task(f);", @"new\s+Task")]
        [TestCase("var t = new TaskCompletionSource<int>();", "")]
        public void MatcherBites(string line, string rulePattern)
        {
            var hits = Violations(line, AllRules, honourCultureOk: false).Select(h => h.Rule.Pattern).ToList();
            if (rulePattern.Length == 0)
            {
                Assert.That(hits, Is.Empty);
                return;
            }
            Assert.That(hits.Count, Is.EqualTo(1), "expected exactly one rule, got: " + string.Join(" | ", hits));
            Assert.That(hits[0], Does.Contain(rulePattern));
        }

        [Test]
        public void CultureOkNeedsAReason()
        {
            Assert.That(Violations("var s = n.ToString(); // culture-ok: ints only", CultureRules, true), Is.Empty);
            Assert.That(Violations("var s = n.ToString(); // culture-ok:", CultureRules, true), Is.Not.Empty);
        }
    }
}
