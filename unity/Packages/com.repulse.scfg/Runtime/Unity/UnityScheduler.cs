// No Python twin: the glue's IScheduler (Godot's core/tree_scheduler.gd), the Worker's timer source.
//
// Entries run from Pump(), which ScfgRunner calls once a frame, on the game's thread. Schedule() never runs a callback
// inline. Each Pump first takes the entries that are due and only then runs them, so an entry scheduled (even with 0
// seconds) while callbacks run waits for the next frame. Time is Time.realtimeSinceStartupAsDouble by default: it keeps
// going at Time.timeScale = 0 and while the game is paused.
#nullable enable
using System;
using System.Collections.Generic;
using Scfg.Core;
using UnityEngine;

namespace Scfg
{
    /// <summary>The Worker's <see cref="IScheduler"/> on Unity's frame loop (see the file header).</summary>
    internal sealed class UnityScheduler : IScheduler
    {
        private sealed class Entry
        {
            public readonly Action Fn;
            public readonly double Due;
            public readonly long Seq;
            public bool Gone;

            public Entry(Action fn, double due, long seq)
            {
                Fn = fn;
                Due = due;
                Seq = seq;
            }
        }

        private readonly Func<double> _clock;
        private readonly Action<Exception> _onError;
        private readonly List<Entry> _entries = new List<Entry>();
        private long _seq;

        /// <param name="clock">Seconds, monotonic; default <c>Time.realtimeSinceStartupAsDouble</c>.</param>
        /// <param name="onError">Gets an exception a callback threw (the other callbacks still run); default Debug.LogException.</param>
        public UnityScheduler(Func<double>? clock = null, Action<Exception>? onError = null)
        {
            _clock = clock ?? (() => Time.realtimeSinceStartupAsDouble);
            _onError = onError ?? Debug.LogException;
        }

        /// <summary>The entries waiting to run.</summary>
        public int Count => _entries.Count;

        public object Schedule(Action fn, double seconds)
        {
            if (fn == null) throw new TypeError("Schedule needs a callback");
            double wait = double.IsNaN(seconds) || seconds < 0 ? 0 : seconds;
            var entry = new Entry(fn, _clock() + wait, _seq++);
            _entries.Add(entry);
            return entry;
        }

        public void Cancel(object handle)
        {
            if (handle is Entry entry)
            {
                entry.Gone = true;
                _entries.Remove(entry);
            }
        }

        /// <summary>Runs the entries due now, oldest due first; returns how many ran.</summary>
        public int Pump()
        {
            if (_entries.Count == 0) return 0;
            double now = _clock();
            var due = new List<Entry>();
            foreach (var e in _entries)
                if (e.Due <= now) due.Add(e);
            if (due.Count == 0) return 0;
            due.Sort((a, b) => a.Due != b.Due ? a.Due.CompareTo(b.Due) : a.Seq.CompareTo(b.Seq));
            foreach (var e in due) _entries.Remove(e);
            int ran = 0;
            foreach (var e in due)
            {
                if (e.Gone) continue; // cancelled by an earlier callback of this frame
                e.Gone = true;
                ran++;
                try
                {
                    e.Fn();
                }
                catch (Exception error)
                {
                    _onError(error);
                }
            }
            return ran;
        }

        /// <summary>Drops every entry without running it.</summary>
        public void Clear()
        {
            foreach (var e in _entries) e.Gone = true;
            _entries.Clear();
        }
    }
}
