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

        // One runner per tier; task 10 adds Tier2Failures and a Tier2UnderTurkishAndGerman test beside this one.
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
