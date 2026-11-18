// UnityScheduler, the Worker's timer source (Godot's core/tree_scheduler.gd), on a manual clock.
#nullable enable
using System;
using System.Collections.Generic;
using NUnit.Framework;

namespace Scfg.Tests
{
    public sealed class SchedulerTests
    {
        private double _now;
        private readonly List<Exception> _errors = new List<Exception>();

        [SetUp]
        public void Fresh()
        {
            _now = 0;
            _errors.Clear();
        }

        private UnityScheduler Make() => new UnityScheduler(() => _now, _errors.Add);

        [Test]
        public void NeverRunsInlineAndRunsWhenDue()
        {
            var s = Make();
            var ran = new List<string>();
            s.Schedule(() => ran.Add("a"), 0);
            s.Schedule(() => ran.Add("b"), 2);
            Assert.That(ran, Is.Empty);
            Assert.That(s.Pump(), Is.EqualTo(1));
            Assert.That(ran, Is.EqualTo(new[] { "a" }));
            _now = 1.9;
            Assert.That(s.Pump(), Is.EqualTo(0));
            _now = 2;
            s.Pump();
            Assert.That(ran, Is.EqualTo(new[] { "a", "b" }));
            Assert.That(s.Count, Is.EqualTo(0));
        }

        [Test]
        public void AZeroSecondEntryScheduledByACallbackWaitsForTheNextFrame()
        {
            var s = Make();
            int runs = 0;
            void Again()
            {
                runs++;
                s.Schedule(Again, 0);
            }
            s.Schedule(Again, 0);
            s.Pump();
            Assert.That(runs, Is.EqualTo(1));
            s.Pump();
            Assert.That(runs, Is.EqualTo(2));
        }

        [Test]
        public void CancelKeepsAnEntryFromRunningEvenWithinTheSameFrame()
        {
            var s = Make();
            object? second = null;
            var ran = new List<string>();
            s.Schedule(() => { ran.Add("first"); s.Cancel(second!); }, 0);
            second = s.Schedule(() => ran.Add("second"), 0);
            var third = s.Schedule(() => ran.Add("third"), 0);
            s.Cancel(third);
            s.Pump();
            Assert.That(ran, Is.EqualTo(new[] { "first" }));
        }

        [Test]
        public void DueEntriesRunOldestDueFirstAndAThrowDoesNotStopTheRest()
        {
            var s = Make();
            var ran = new List<string>();
            s.Schedule(() => ran.Add("late"), 3);
            s.Schedule(() => throw new InvalidOperationException("boom"), 1);
            s.Schedule(() => ran.Add("early"), 2);
            _now = 5;
            s.Pump();
            Assert.That(ran, Is.EqualTo(new[] { "early", "late" }));
            Assert.That(_errors.Count, Is.EqualTo(1));
        }

        [Test]
        public void ClearDropsEverything()
        {
            var s = Make();
            int runs = 0;
            s.Schedule(() => runs++, 0);
            s.Clear();
            s.Pump();
            Assert.That(runs, Is.EqualTo(0));
        }
    }
}
