// No Python twin: replays every Tier 1 topic through Tier1 (conformance/js/tier1.test.ts's checks).
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using NUnit.Framework;
using Scfg.Core;

namespace Scfg.Core.Tests
{
    public class Tier1Tests
    {
        private const int ShownDiffs = 20;

        private static IEnumerable<string> Topics() => Vectors.Tier1Topics();

        [TestCaseSource(nameof(Topics))]
        public void Topic(string topic)
        {
            var failures = Tier1.RunTopic(topic);
            Assert.That(failures, Is.Empty, Report(topic, failures));
        }

        /// <summary>Up to <see cref="ShownDiffs"/> failure messages, then how many more there were.</summary>
        internal static string Report(string what, List<string> failures)
        {
            var shown = failures.Take(ShownDiffs).ToList();
            var more = failures.Count - shown.Count;
            var tail = more > 0 ? "\n... and " + more.ToString(CultureInfo.InvariantCulture) + " more" : "";
            return what + ": " + failures.Count.ToString(CultureInfo.InvariantCulture) + " failing case(s)\n"
                   + string.Join("\n", shown) + tail;
        }

        private static JsonObject Case(string fn, string key, object? value) =>
            new JsonObject { { "fn", fn }, { "in", new JsonObject() }, { key, value } };

        [Test]
        public void RunnerRejectsTheWrongErrorClass()
        {
            Tier1.Funcs["__plain"] = _ => throw new InvalidOperationException("boom");
            Tier1.Funcs["__value"] = _ => throw new ValueError("bad");
            Tier1.Funcs["__type"] = _ => throw new TypeError("bad");
            Tier1.Funcs["__config"] = _ => throw new ConfigError("bad", "a.b");
            Tier1.Funcs["__key"] = _ => throw new KeyError("k");
            Tier1.Funcs["__arith"] = _ => throw new ArithmeticError("big");
            Tier1.Funcs["__one"] = _ => 1L;
            try
            {
                Assert.That(Tier1.RunCase(Case("__plain", "error", "invalid")), Does.Contain("InvalidOperationException"));
                Assert.That(Tier1.RunCase(Case("__key", "error", "invalid")), Does.Contain("KeyError"));
                Assert.That(Tier1.RunCase(Case("__arith", "error", "invalid")), Does.Contain("ArithmeticError"));
                Assert.That(Tier1.RunCase(Case("__value", "error", "invalid")), Is.Null);
                Assert.That(Tier1.RunCase(Case("__type", "error", "invalid")), Is.Null);
                Assert.That(Tier1.RunCase(Case("__config", "error", "a.b")), Is.Null);
                Assert.That(Tier1.RunCase(Case("__config", "error", "a.c")), Is.Not.Null);
                Assert.That(Tier1.RunCase(Case("__config", "error", "invalid")), Is.Not.Null);
                Assert.That(Tier1.RunCase(Case("__value", "error", "a.b")), Is.Not.Null);
                Assert.That(Tier1.RunCase(Case("__one", "out", 1L)), Is.Null);
                Assert.That(Tier1.RunCase(Case("__one", "out", 1.0)), Is.Null); // JSON equality: 1 == 1.0
                Assert.That(Tier1.RunCase(Case("__one", "out", "1")), Is.Not.Null);
                Assert.That(Tier1.RunCase(Case("__one", "out", true)), Is.Not.Null);
                Assert.That(Tier1.RunCase(Case("__one", "error", "invalid")), Is.Not.Null);
                Assert.That(Tier1.RunCase(Case("__value", "out", 1L)), Is.Not.Null);
            }
            finally
            {
                foreach (var k in new[] { "__plain", "__value", "__type", "__config", "__key", "__arith", "__one" })
                    Tier1.Funcs.Remove(k);
            }
        }

        [Test]
        public void AnUnimplementedFnIsReportedNotPassed()
        {
            Assert.That(Tier1.RunCase(Case("__nope", "out", 1L)), Does.Contain("no C# adapter"));
        }
    }
}
