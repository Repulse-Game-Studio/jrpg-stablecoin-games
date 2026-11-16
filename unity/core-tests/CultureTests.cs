// No Python twin: the conformance runners again under cultures whose formatting and casing differ from the
// invariant culture (tr-TR: dotted and dotless i; de-DE: the decimal comma), so a culture-sensitive conversion
// anywhere in the core shows up as a vector failure.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using NUnit.Framework;

namespace Scfg.Core.Tests
{
    public class CultureTests
    {
        private static readonly string[] Cultures = { "tr-TR", "de-DE" };

        /// <summary>
        /// Runs <paramref name="runner"/> (which returns its failure messages) under each of <see cref="Cultures"/>,
        /// restoring the thread's cultures afterwards even when it throws. A culture that does not take effect (a
        /// runtime in globalization-invariant mode) is itself a failure, so the test cannot pass vacuously.
        /// </summary>
        private static List<string> UnderEachCulture(Func<IEnumerable<string>> runner)
        {
            var failures = new List<string>();
            var culture = CultureInfo.CurrentCulture;
            var uiCulture = CultureInfo.CurrentUICulture;
            try
            {
                foreach (var name in Cultures)
                {
                    var c = new CultureInfo(name);
                    CultureInfo.CurrentCulture = c;
                    CultureInfo.CurrentUICulture = c;
                    if (!Bites(name)) failures.Add(name + ": the culture did not take effect");
                    failures.AddRange(runner().Select(f => name + " " + f));
                }
            }
            finally
            {
                CultureInfo.CurrentCulture = culture;
                CultureInfo.CurrentUICulture = uiCulture;
            }
            return failures;
        }

        // The culture-sensitive behaviour each culture is chosen for.
        private static bool Bites(string name)
        {
            switch (name)
            {
                case "tr-TR": return "i".ToUpper() == "İ";
                case "de-DE": return 1.5.ToString() == "1,5";
                default: return true;
            }
        }

        // One runner per tier.
        private static IEnumerable<string> Tier1Failures()
        {
            var topics = Tier1.Implemented.OrderBy(t => t, StringComparer.Ordinal).ToList();
            if (topics.Count == 0) yield return "no Tier 1 topic is implemented";
            foreach (var topic in topics)
                foreach (var f in Tier1.RunTopic(topic))
                    yield return topic + " " + f;
        }

        [Test]
        public void Tier1UnderTurkishAndGerman()
        {
            var before = CultureInfo.CurrentCulture.Name;
            var failures = UnderEachCulture(Tier1Failures);
            Assert.That(CultureInfo.CurrentCulture.Name, Is.EqualTo(before), "the culture was not restored");
            Assert.That(failures, Is.Empty, Tier1Tests.Report("Tier 1 under " + string.Join(", ", Cultures), failures));
        }

        // The replay transport answers with completed tasks, so a replay completes before Replay returns: reading the
        // result of a completed task does not wait, and the culture set on this thread is the one the core runs under.
        private static IEnumerable<string> Tier2Failures()
        {
            var ids = Vectors.Tier2Ids().Where(Tier2.Runs).ToList();
            if (ids.Count == 0) yield return "no Tier 2 vector is enabled";
            foreach (var id in ids)
            {
                var run = Tier2.Replay(Tier2Tests.Load(id));
                if (!run.IsCompleted)
                {
                    yield return id + ": the replay did not complete synchronously";
                    continue;
                }
                var diff = run.GetAwaiter().GetResult();
                if (diff != null) yield return id + ": " + diff;
            }
        }

        [Test]
        public void Tier2UnderTurkishAndGerman()
        {
            var before = CultureInfo.CurrentCulture.Name;
            var failures = UnderEachCulture(Tier2Failures);
            Assert.That(CultureInfo.CurrentCulture.Name, Is.EqualTo(before), "the culture was not restored");
            Assert.That(failures, Is.Empty, Tier1Tests.Report("Tier 2 under " + string.Join(", ", Cultures), failures));
        }

        [Test]
        public void TheCultureIsRestoredWhenTheRunnerThrows()
        {
            var before = CultureInfo.CurrentCulture.Name;
            var beforeUi = CultureInfo.CurrentUICulture.Name;
            IEnumerable<string> Boom()
            {
                throw new InvalidOperationException("boom");
            }
            Assert.Throws<InvalidOperationException>(() => UnderEachCulture(Boom));
            Assert.That(CultureInfo.CurrentCulture.Name, Is.EqualTo(before));
            Assert.That(CultureInfo.CurrentUICulture.Name, Is.EqualTo(beforeUi));
        }
    }
}
