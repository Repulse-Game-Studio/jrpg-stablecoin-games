// No Python twin: the lints that every Runtime/Core and Runtime/Desktop file must pass, and the Sockets lint of
// Runtime/Desktop.
//
// THE RULES LIVE HERE. Each lint is a table of (regex, why) below. A failure prints
// "relative/path.cs:line: <pattern> (<why>)". Comments are ignored, and so is the text of string literals
// (so "record not found" in a message is fine), but code inside the holes of an interpolated string
// ($"{a.Trim()}") IS scanned. Rules marked raw are matched against the line with literals kept (used for
// \d / \s inside a Regex). To opt a single line out of a ToString()/Format rule, end it with
// `// culture-ok: <reason>`, and out of a static-state rule with `// static-ok: <reason>`.
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
            /// <summary>When set, the rule fires only if its <c>type</c> group also matches this.</summary>
            public readonly Regex? Type;
            /// <summary>A static-state rule, waived by <c>// static-ok: reason</c>.</summary>
            public readonly bool Static;
            public Rule(string pattern, string why, bool raw = false, string? type = null, bool isStatic = false)
            {
                Pattern = pattern;
                Why = why;
                Raw = raw;
                Re = new Regex(pattern, RegexOptions.CultureInvariant);
                Type = type == null ? null : new Regex(type, RegexOptions.CultureInvariant);
                Static = isStatic;
            }

            public bool IsMatch(string line)
            {
                var m = Re.Match(line);
                return m.Success && (Type == null || Type.IsMatch(m.Groups["type"].Value));
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

        // Domain reload off (Unity's Enter Play Mode Options) keeps statics alive between play sessions, so no static
        // may be writable: a static field must be readonly or const, a static readonly field must not have a mutable
        // collection type (IReadOnlyList / IReadOnlyDictionary backed by a ReadOnlyCollection / ReadOnlyDictionary
        // instead), and a static property has no setter. A field declaration is a line of modifiers, `static`, a
        // type and a name followed by `=` or `;` (so a method, `=>` property, class, local function or `using
        // static` never matches). A tuple-typed field (a parenthesised type) is not recognised.
        private const string FieldModifiers = @"^\s*(?:(?:public|private|internal|protected|new|unsafe|volatile|readonly)\s+)*static\s+";
        private const string FieldName = @"(?<type>[^=;(){}]+?)\s+@?[A-Za-z_][A-Za-z0-9_]*";
        private const string FieldTail = FieldName + @"\s*(?:=(?![=>])|;)";
        private const string ReadOnlyView = @"(?<![A-Za-z0-9_])(?:IReadOnly[A-Za-z]*|IEnumerable)\s*<";
        private const string WritableLiteral =
            @"(?:new\s*\[\s*\]|new\s+[A-Za-z_][A-Za-z0-9_.]*\s*\[|new\s+(?:List|Dictionary|HashSet|SortedDictionary|SortedList|SortedSet|Queue|Stack|LinkedList|JsonObject)\b)";
        private const string MutableCollection =
            @"(?<![A-Za-z0-9_])(?:I?List|I?Dictionary|HashSet|ISet|I?Collection|SortedDictionary|SortedList|SortedSet|Queue|Stack|ConcurrentDictionary|ConcurrentBag|ConcurrentQueue|LinkedList|StringBuilder)\s*<|\[[\s,]*\]|(?<![A-Za-z0-9_])(?:JsonObject|StringBuilder|ArrayList|Hashtable)(?![A-Za-z0-9_])";

        private static readonly Rule[] StaticRules =
        {
            new Rule(@"^(?!.*\breadonly\b)" + FieldModifiers.Substring(1) + @"(?:(?:volatile|new|unsafe)\s+)*" + FieldTail,
                     "a static field must be readonly or const (domain reload off keeps it between play sessions)", isStatic: true),
            new Rule(@"^(?=.*\breadonly\b)" + FieldModifiers.Substring(1) + @"(?:(?:readonly|new|volatile)\s+)*" + FieldTail,
                     "a static readonly collection is still writable; expose IReadOnlyList / IReadOnlyDictionary over a ReadOnly wrapper",
                     type: MutableCollection, isStatic: true),
            new Rule(@"\bstatic\b[^=;(]*\{[^}]*\bset\s*;",
                     "a static property must not have a setter", isStatic: true),
            // A read-only type is no protection when the object behind it is an array or a List: the initializer must
            // wrap it (AsReadOnly, ReadOnlyCollection, ReadOnlyDictionary). One-line declarations only: an initializer
            // on the following lines is not seen.
            new Rule(@"^(?=.*\breadonly\b)(?!.*\b(?:AsReadOnly|ReadOnlyCollection|ReadOnlyDictionary)\b)" + FieldModifiers.Substring(1)
                     + @"(?:(?:readonly|new|volatile)\s+)*" + FieldName + @"\s*=\s*" + WritableLiteral,
                     "a read-only static over a writable literal (an array or collection literal); wrap it",
                     type: ReadOnlyView, isStatic: true),
            new Rule(@"^(?!.*\b(?:AsReadOnly|ReadOnlyCollection|ReadOnlyDictionary)\b)" + FieldModifiers.Substring(1)
                     + FieldName + @"\s*\{\s*get\s*;\s*\}\s*=\s*" + WritableLiteral,
                     "a read-only static property over a writable literal (an array or collection literal); wrap it",
                     type: ReadOnlyView, isStatic: true),
            new Rule(FieldModifiers + FieldName + @"\s*\{\s*get\s*;\s*\}",
                     "a get-only static property of a mutable collection type is still writable",
                     type: MutableCollection, isStatic: true),
        };

        // Runtime/Desktop only: the localhost server never waits on a socket. Its sockets are non-blocking (the
        // NonBlockingSockets test), accepted only after Poll(0), read only as far as Available, and written with the
        // out-SocketError overloads; nothing completes on the thread pool.
        private static readonly Rule[] SocketRules = R(
            (@"\b[A-Za-z_][A-Za-z0-9_]*Async\s*\(", "no *Async socket calls: they complete on the thread pool"),
            (@"\b(?:Begin|End)(?:Accept|Connect|Disconnect|Receive|ReceiveFrom|ReceiveMessageFrom|Send|SendFile|SendTo|Read|Write)\w*\s*\(",
                "no Begin/End (APM) calls: they complete on the thread pool"),
            (@"\bTcpListener\b", "use a non-blocking Socket (AcceptTcpClient blocks)"),
            (@"\bTcpClient\b", "use a non-blocking Socket"),
            (@"\bNetworkStream\b", "a NetworkStream blocks"),
            (@"\bThread\.Sleep\b", "never wait"),
            (@"\.Poll\(\s*(?!0\s*,)", "Poll only with a zero timeout: any other waits"),
            (@"\bSocket\.Select\(", "Socket.Select waits"),
            (@"\.Blocking\s*=(?!=|\s*false\b)", "sockets stay non-blocking"),
            (@"\.(?:Receive|Send)\((?![^;]*\bout\s+(?:SocketError|var)\b)",
                "Receive/Send with the out SocketError overload (WouldBlock is a value, not an exception)"),
            (@"\.Close\(\s*[^)\s]", "Close() without a timeout: a timeout makes Close wait (and Close(0) resets the connection)"),
            (@"\bLingerState\b", "a linger makes Close wait"),
            (@"\.(?:Connect|Disconnect)\(", "Connect and Disconnect wait"));

        private static readonly Rule[] AllRules =
            UnityRules.Concat(ThreadRules).Concat(ReflectionRules).Concat(CultureRules).Concat(StaticRules).ToArray();

        // A line is exempt from the ToString()/Format rules only through this marker in its raw text.
        private static readonly Regex CultureOk = new Regex(@"//\s*culture-ok:\s*\S", RegexOptions.CultureInvariant);
        // ... and from a static-state rule only through this one.
        private static readonly Regex StaticOk = new Regex(@"//\s*static-ok:\s*\S", RegexOptions.CultureInvariant);

        private static string RuntimeDir()
        {
            var root = Vectors.FindUp("unity/Packages/com.repulse.scfg/Runtime/Core");
            return Path.Combine(root, "unity", "Packages", "com.repulse.scfg", "Runtime");
        }

        private static List<string> Files(string dir)
        {
            var path = Path.Combine(RuntimeDir(), dir);
            var files = Directory.GetFiles(path, "*.cs", SearchOption.AllDirectories).ToList();
            files.Sort(StringComparer.Ordinal);
            Assert.That(files, Is.Not.Empty, "scanned no files under " + path);
            return files;
        }

        /// <summary>Every C# file of the package's runtime: Runtime/Core and Runtime/Desktop.</summary>
        private static List<string> RuntimeFiles() => Files("Core").Concat(Files("Desktop")).ToList();

        private static string Rel(string file) => Path.GetRelativePath(RuntimeDir(), file).Replace('\\', '/');

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

        private static List<(int Line, Rule Rule)> Violations(string text, Rule[] rules, bool honourMarkers)
        {
            var raw = text.Replace("\r\n", "\n").Split('\n');
            var code = Code(text);
            var kept = Code(text, keepStrings: true);
            var hits = new List<(int, Rule)>();
            for (int n = 0; n < code.Length; n++)
            {
                foreach (var rule in rules)
                {
                    if (!rule.IsMatch(rule.Raw ? kept[n] : code[n])) continue;
                    bool formatRule = !rule.Static && (rule.Pattern.Contains("ToString") || rule.Pattern.Contains("tring"));
                    if (honourMarkers && formatRule && CultureOk.IsMatch(raw[n])) continue;
                    if (honourMarkers && rule.Static && StaticOk.IsMatch(raw[n])) continue;
                    hits.Add((n + 1, rule));
                }
            }
            return hits;
        }

        private static void Scan(Rule[] rules, bool honourMarkers = false, List<string>? files = null)
        {
            var hits = new List<string>();
            foreach (var file in files ?? RuntimeFiles())
            {
                foreach (var (line, rule) in Violations(File.ReadAllText(file), rules, honourMarkers))
                    hits.Add($"{Rel(file)}:{line}: {rule.Pattern} ({rule.Why})");
            }
            Assert.That(hits, Is.Empty, "lint violations:\n" + string.Join("\n", hits));
        }

        [Test] public void NoUnityEngine() => Scan(UnityRules);
        [Test] public void NoThreadsOrTimers() => Scan(ThreadRules);
        [Test] public void NoReflection() => Scan(ReflectionRules);
        [Test] public void NoCultureSensitiveText() => Scan(CultureRules, honourMarkers: true);
        [Test] public void NoMutableStaticState() => Scan(StaticRules, honourMarkers: true);
        [Test] public void NoWaitingSockets() => Scan(SocketRules, files: Files("Desktop"));

        // A socket is non-blocking from the line it is made on: each `new Socket(` and each `.Accept(` of a file is
        // matched by a `.Blocking = false` in that file's code (not a comment or a string).
        private static readonly Regex MakesSocket = new Regex(@"\bnew\s+Socket\s*\(|\.Accept\s*\(", RegexOptions.CultureInvariant);
        private static readonly Regex SetsNonBlocking = new Regex(@"\.Blocking\s*=\s*false\b", RegexOptions.CultureInvariant);

        internal static bool SocketsAreNonBlocking(string text)
        {
            var code = string.Join("\n", Code(text));
            int made = MakesSocket.Matches(code).Count;
            return SetsNonBlocking.Matches(code).Count >= Math.Max(made, 1);
        }

        [Test]
        public void NonBlockingSockets()
        {
            var made = Files("Desktop").Where(f => MakesSocket.IsMatch(string.Join("\n", Code(File.ReadAllText(f))))).ToList();
            Assert.That(made, Is.Not.Empty, "Runtime/Desktop makes no socket: the lint has nothing to check");
            var bad = made.Where(f => !SocketsAreNonBlocking(File.ReadAllText(f))).Select(Rel).ToList();
            Assert.That(bad, Is.Empty, "a socket made without `.Blocking = false`: " + string.Join(", ", bad));
        }

        [TestCase("var s = new Socket(f, t, p);\ns.Blocking = false;\nvar p = s.Accept();\np.Blocking = false;", true)]
        [TestCase("var s = new Socket(f, t, p);\ns.Blocking = false;", true)]
        [TestCase("var s = new Socket(f, t, p);", false)]
        [TestCase("var s = new Socket(f, t, p);\n// s.Blocking = false;", false)]
        [TestCase("var s = new Socket(f, t, p);\nvar x = \"s.Blocking = false\";", false)]
        [TestCase("var s = new Socket(f, t, p);\ns.Blocking = false;\nvar p = s.Accept();", false)]
        [TestCase("var s = new Socket(f, t, p);\ns.Blocking = true;", false)]
        [TestCase("var x = 1;", false)]
        public void NonBlockingLintBites(string text, bool expected) =>
            Assert.That(SocketsAreNonBlocking(text), Is.EqualTo(expected));

        // Each case names a fragment of the ONE socket rule that must fire on it (empty = none may fire).
        [TestCase("var n = await s.ReceiveAsync(buf, SocketFlags.None);", "Async")]
        [TestCase("s.AcceptAsync(args);", "Async")]
        [TestCase("var r = s.BeginAccept(cb, null);", "APM")]
        [TestCase("var c = s.EndReceive(r);", "APM")]
        [TestCase("s.BeginSend(b, 0, n, SocketFlags.None, cb, null);", "APM")]
        [TestCase("var l = new TcpListener(IPAddress.Loopback, 0);", "TcpListener")]
        [TestCase("using var c = new TcpClient();", "TcpClient")]
        [TestCase("var st = new NetworkStream(s);", "NetworkStream")]
        [TestCase("Thread.Sleep(1);", "never wait")]
        [TestCase("if (s.Poll(1000, SelectMode.SelectRead)) x();", "Poll")]
        [TestCase("if (s.Poll(-1, SelectMode.SelectRead)) x();", "Poll")]
        [TestCase("if (s.Poll(0, SelectMode.SelectRead)) x();", "")]
        [TestCase("Socket.Select(r, null, null, 1000);", "Select")]
        [TestCase("s.Blocking = true;", "non-blocking")]
        [TestCase("s.Blocking = flag;", "non-blocking")]
        [TestCase("s.Blocking = false;", "")]
        [TestCase("s.Blocking=false;", "")]
        [TestCase("var b = s.Blocking == false || !s.Blocking;", "")]
        [TestCase("var n = s.Receive(buf);", "out SocketError")]
        [TestCase("var n = s.Receive(buf, 0, n, SocketFlags.None);", "out SocketError")]
        [TestCase("var n = s.Receive(buf, 0, n, SocketFlags.None, out SocketError e);", "")]
        [TestCase("var n = s.Send(buf, 0, n, SocketFlags.None, out var e);", "")]
        [TestCase("s.Send(buf);", "out SocketError")]
        [TestCase("s.Close(0);", "Close()")]
        [TestCase("s.Close(1000);", "Close()")]
        [TestCase("s.Close();", "")]
        [TestCase("s.LingerState = new LingerOption(true, 1);", "linger")]
        [TestCase("s.Connect(ep);", "Connect")]
        [TestCase("s.Disconnect(false);", "Connect")]
        [TestCase("var p = s.Accept();", "")]
        [TestCase("SendJson(c, 200, o); Send(c, 404, \"x\");", "")]
        [TestCase("// s.BeginAccept(cb, null); s.Close(0);", "")]
        public void SocketMatcherBites(string line, string ruleWhy)
        {
            var hits = Violations(line, SocketRules, honourMarkers: false).Select(h => h.Rule.Pattern + " | " + h.Rule.Why).ToList();
            if (ruleWhy.Length == 0)
            {
                Assert.That(hits, Is.Empty);
                return;
            }
            Assert.That(hits.Count, Is.EqualTo(1), "expected exactly one rule, got: " + string.Join(" | ", hits));
            Assert.That(hits[0], Does.Contain(ruleWhy));
        }

        [Test]
        public void EveryFileNamesItsTwin()
        {
            var bad = new List<string>();
            foreach (var file in RuntimeFiles())
            {
                var head = File.ReadLines(file).Take(5).ToList();
                if (!head.Any(l => l.Contains("Twin of scfg_core/") || l.Contains("No Python twin")))
                    bad.Add($"{Rel(file)}:1: missing 'Twin of scfg_core/' or 'No Python twin' in the first 5 lines");
            }
            Assert.That(bad, Is.Empty, string.Join("\n", bad));
        }

        [Test]
        public void TheLocalServerNamesItsTwin()
        {
            var file = Path.Combine(RuntimeDir(), "Desktop", "LocalServer.cs");
            Assert.That(File.ReadLines(file).Take(5).Any(l => l.Contains("Twin of scfg_core/local_server.py")), Is.True,
                        "Desktop/LocalServer.cs:1: missing 'Twin of scfg_core/local_server.py' in the first 5 lines");
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
            var bad = new List<string>();
            foreach (var file in RuntimeFiles())
                if (!EnablesNullableFirst(File.ReadAllText(file)))
                    bad.Add($"{Rel(file)}: '#nullable enable' must be the first line after the header comment");
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

        // The lints must actually bite. Each case names a fragment of the ONE rule that must fire on it (of its
        // pattern or its why; empty = no rule may fire), so a line caught by the wrong rule fails.
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
        // static state (the readonly rule's pattern starts with ^(?!, the collection rule's with ^(?=)
        [TestCase("private static int _n;", @"^(?!")]
        [TestCase("public static string Name = \"x\";", @"^(?!")]
        [TestCase("static Engine? _engine;", @"^(?!")]
        [TestCase("internal static volatile bool Flag = false;", @"^(?!")]
        [TestCase("public static readonly string[] A = { \"a\" };", @"^(?=")]
        [TestCase("private static readonly int[][] T =", @"^(?=")]
        [TestCase("static readonly Dictionary<string, int> D = new Dictionary<string, int>();", @"^(?=")]
        [TestCase("public static readonly List<int> L = new List<int>();", @"^(?=")]
        [TestCase("private static readonly HashSet<string> H = new HashSet<string>();", @"^(?=")]
        [TestCase("public static readonly JsonObject J = new JsonObject();", @"^(?=")]
        [TestCase("public readonly static IList<string> I = new List<string>();", @"^(?=")]
        [TestCase("public static readonly IDictionary<string, int> I = new Dictionary<string, int>();", @"^(?=")]
        [TestCase("public static readonly IReadOnlyList<string> R = Array.AsReadOnly(new[] { \"a\" });", "")]
        [TestCase("public static readonly IReadOnlyDictionary<string, int> R = new ReadOnlyDictionary<string, int>(d);", "")]
        [TestCase("public static readonly IReadOnlyCollection<string> R = Array.AsReadOnly(new[] { \"a\" });", "")]
        [TestCase("public static readonly ReadOnlyCollection<string> R = Array.AsReadOnly(new[] { \"a\" });", "")]
        [TestCase("private static readonly Regex Re = new Regex(\"a\");", "")]
        [TestCase("public static readonly Dec MinTip = Dec.Parse(\"0.50\")!;", "")]
        [TestCase("public const int N = 5;", "")]
        [TestCase("public static int F(int x = 1) => x;", "")]
        [TestCase("public static string[] Names() => new string[0];", "")]
        [TestCase("public static int P => 5;", "")]
        [TestCase("public static IReadOnlyList<string> P { get; } = Array.AsReadOnly(new[] { \"a\" });", "")]
        [TestCase("public static int P { get; set; }", @"set\s*;")]
        [TestCase("public static int P { get; private set; }", @"set\s*;")]
        [TestCase("using static System.Math;", "")]
        [TestCase("public static class Foo", "")]
        [TestCase("static int Local(int a) { return a; }", "")]
        [TestCase("Func<int, int> f = static x => x;", "")]
        [TestCase("var s = \"static int x;\";", "")]
        // a read-only type over a writable literal (one-line declarations only), and get-only static properties
        [TestCase("public static readonly IReadOnlyList<string> R = new[] { \"a\" };", "read-only static over")]
        [TestCase("public static readonly IReadOnlyList<string> R = new string[] { \"a\" };", "read-only static over")]
        [TestCase("public static readonly IReadOnlyList<int> R = new List<int> { 1 };", "read-only static over")]
        [TestCase("public static readonly IReadOnlyDictionary<string, int> R = new Dictionary<string, int>();", "read-only static over")]
        [TestCase("public static readonly IEnumerable<string> R = new HashSet<string>();", "read-only static over")]
        [TestCase("public static readonly IReadOnlyCollection<string> R = new string[2];", "read-only static over")]
        [TestCase("public static readonly IReadOnlyList<string> R = Array.AsReadOnly(new[] { \"a\" });", "")]
        [TestCase("public static readonly IReadOnlyDictionary<string, int> R = new ReadOnlyDictionary<string, int>(new Dictionary<string, int>());", "")]
        [TestCase("public static readonly IReadOnlyList<string> R = Build();", "")]
        [TestCase("public static IReadOnlyList<string> P { get; } = new[] { \"a\" };", "read-only static property over")]
        [TestCase("public static IReadOnlyList<string> P { get; } = new List<string>();", "read-only static property over")]
        [TestCase("public static IReadOnlyList<string> P { get; } = Array.AsReadOnly(new[] { \"a\" });", "")]
        [TestCase("public static List<string> P { get; } = new List<string>();", "get-only static property of a mutable")]
        [TestCase("public static string[] P { get; } = { \"a\" };", "get-only static property of a mutable")]
        [TestCase("internal static JsonObject P { get; } = new JsonObject();", "get-only static property of a mutable")]
        [TestCase("public static Dec P { get; } = Dec.Parse(\"1\")!;", "")]
        [TestCase("public static string Name { get; } = \"x\";", "")]
        public void MatcherBites(string line, string rulePattern)
        {
            var hits = Violations(line, AllRules, honourMarkers: false).Select(h => h.Rule.Pattern + " | " + h.Rule.Why).ToList();
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

        [Test]
        public void StaticOkNeedsAReasonAndWaivesOnlyTheStaticRules()
        {
            Assert.That(Violations("private static readonly string[] A = { }; // static-ok: private, never written", StaticRules, true), Is.Empty);
            Assert.That(Violations("private static int _n; // static-ok: test hook", StaticRules, true), Is.Empty);
            Assert.That(Violations("private static readonly string[] A = { }; // static-ok:", StaticRules, true), Is.Not.Empty);
            Assert.That(Violations("private static readonly string[] A = { }; // culture-ok: x", StaticRules, true), Is.Not.Empty);
            Assert.That(Violations("var s = n.ToString(); // static-ok: x", CultureRules, true), Is.Not.Empty);
        }
    }
}
