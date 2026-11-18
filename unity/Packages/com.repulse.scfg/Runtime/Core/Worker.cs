// Twin of scfg_core/worker.py (Worker, and concurrent.futures.Future as Job), following rpgmaker/src/core/worker.ts
// and godot/addons/scfg/core/worker.gd + job.gd: the single writer, one command queue plus a periodic tick loop that
// owns the Engine, as one cooperative object on one thread (Python's runs on a thread of its own).
//
// The game loop never touches the Engine directly. It:
//   - submits commands with Submit(fn) and polls the returned Job every frame (or hangs a done callback on it),
//   - reads the latest detached Snapshot from .Snapshot (replaced after each pass),
//   - collects StepOutcomes with DrainOutcomes() (a "waiting" outcome only when it has an error).
//
// ONE RUN AT A TIME. RunOnce() is one pass: the queued commands (in order, until none is left), Reverify() on the very
// first pass only, Tick(), then the snapshot. The in-flight state is set synchronously, before the first command
// runs, so a command, Wake(), Submit() or RunOnce() arriving while a run is in flight (also from inside a command or a
// done callback) only marks a follow-up: exactly ONE more pass runs when the current one ends, and a joining
// RunOnce() returns the same Task, which completes when the whole chain is done and faults with the chain's first
// failure. A command must not AWAIT RunOnce() (the chain waits for the command: deadlock); call it without awaiting,
// or use Wake(). A command submitted while the commands run joins them; one submitted during the tick runs in the
// follow-up pass, never inside a tick.
//
// DRAINING CAN SPIN. The pass drains the queue until it is empty, as Python's does, so done callbacks (or commands)
// that keep submitting commands which complete synchronously never let the pass reach its tick: on Unity's main
// thread that is a frozen frame, where Python only spins its worker thread. Resubmit from a later frame instead.
//
// STOP. Stop() cancels the timer and the queued jobs and ends the chain after the pass in flight. An explicit
// RunOnce() after Stop() runs one pass of its own when nothing is in flight; while a pass is in flight it joins that
// chain and gets no pass of its own (as worker.ts).
//
// The worker creates no thread and no timer: the loop's timer is the injected IScheduler, and the Tasks are promises
// that the transport (and the scheduler's callbacks) complete on the caller's thread. A Job is not a Task: callers
// poll it or use AddDoneCallback, so a failed job nobody looks at is never an unobserved Task exception.
#nullable enable
using System;
using System.Collections.Generic;
using System.Runtime.ExceptionServices;
using System.Threading.Tasks;

namespace Scfg.Core
{
    /// <summary>
    /// Python's <c>concurrent.futures.Future</c> for the UI's per-frame poll. A job is pending, running, finished
    /// (with a value or an exception) or cancelled. The worker completes the jobs it hands out; test fakes and drivers
    /// may complete their own, as Python's Future API is public too. Not thread-safe: it lives on one thread.
    /// </summary>
    /// <summary>
    /// Raised into a job the caller abandoned (<see cref="Job.Abandon"/>), at its next RPC request. Not a
    /// <see cref="TransportError"/>, so no endpoint cools down (worker.py's Abandoned).
    /// </summary>
    public sealed class Abandoned : System.Exception
    {
        public Abandoned(string message) : base(message) { }
    }

    public sealed class Job
    {
        private enum State { Pending, Running, Finished, Cancelled }

        private State _state = State.Pending;
        private object? _value;
        private ExceptionDispatchInfo? _error;
        private CancelledError? _cancelled;
        private bool _notified; // Python's CANCELLED_AND_NOTIFIED: SetRunningOrNotifyCancel saw the cancel
        private List<Action<Job>>? _callbacks = new List<Action<Job>>();
        private readonly Action<System.Exception> _callbackError;

        /// <param name="callbackError">
        /// Where an exception thrown by a done callback goes (Python logs it and goes on with the other callbacks):
        /// default <see cref="Errors.PrintError"/>. A sink that throws itself is ignored. The worker's jobs report to
        /// their Engine's <see cref="Engine.OnError"/>.
        /// </param>
        public Job(Action<System.Exception>? callbackError = null)
        {
            _callbackError = callbackError ?? Errors.PrintError;
        }

        public bool Done() => _state == State.Finished || _state == State.Cancelled;

        public bool Cancelled() => _state == State.Cancelled;

        public bool Running() => _state == State.Running;

        /// <summary>Whether <see cref="Abandon"/> found it running: its RPC requests are refused from then on.</summary>
        public bool IsAbandoned { get; private set; }

        /// <summary>
        /// worker.py's abandon(): a job that has not started is cancelled (it never runs); a running one is refused its
        /// next RPC request with <see cref="Abandoned"/>, so the commands queued behind it wait at most for the request
        /// in flight. Its done callbacks still run when it ends.
        /// </summary>
        public void Abandon()
        {
            if (!Cancel()) IsAbandoned = true;
        }

        /// <summary>
        /// The value. Throws the job's own exception if it failed (rethrown with its original stack trace, on every
        /// call), its <see cref="CancelledError"/> if it was cancelled, and <c>RuntimeError("job is not done")</c>
        /// while it is pending or running.
        /// </summary>
        public object? Result()
        {
            switch (_state)
            {
                case State.Finished:
                    _error?.Throw();
                    return _value;
                case State.Cancelled:
                    throw _cancelled!;
                default:
                    throw new RuntimeError("job is not done");
            }
        }

        /// <summary>
        /// The failure: null on success and while not done. A cancelled job returns its <see cref="CancelledError"/>
        /// (Python's <c>exception()</c> raises it), so ported poll code lands in its failure branch.
        /// </summary>
        public System.Exception? Exception() => _state == State.Cancelled ? _cancelled : _error?.SourceException;

        /// <summary>
        /// Cancels a job that has not started and runs its done callbacks. False once it is running or finished, true
        /// if it is (already) cancelled.
        /// </summary>
        public bool Cancel()
        {
            if (_state == State.Cancelled) return true;
            if (_state != State.Pending) return false;
            _state = State.Cancelled;
            _cancelled = new CancelledError();
            Settle();
            return true;
        }

        /// <summary>
        /// Python's <c>add_done_callback</c>: <paramref name="callback"/> runs at once when the job is done, otherwise
        /// when it finishes or is cancelled, once, on the thread that settles it. One that throws goes to the
        /// callback-error sink (see the constructor) and the other callbacks still run.
        /// </summary>
        public void AddDoneCallback(Action<Job> callback)
        {
            if (callback == null) throw new TypeError("a done callback is required");
            if (_callbacks != null && !Done())
            {
                _callbacks.Add(callback);
                return;
            }
            Invoke(callback);
        }

        /// <summary>
        /// Python's <c>set_running_or_notify_cancel</c>: false for a cancelled job (do not run it), true for a pending
        /// one, which is then running. A running or finished job is a <c>RuntimeError</c>, as in Python, and so is a
        /// second call on a cancelled job (Python's CANCELLED_AND_NOTIFIED state).
        /// </summary>
        public bool SetRunningOrNotifyCancel()
        {
            if (_state == State.Cancelled && !_notified)
            {
                _notified = true;
                return false;
            }
            if (_state != State.Pending) throw new RuntimeError("job in unexpected state");
            _state = State.Running;
            return true;
        }

        /// <summary>Finishes the job with <paramref name="value"/>; an <see cref="InvalidStateError"/> if it is done.</summary>
        public void SetResult(object? value)
        {
            if (Done()) throw new InvalidStateError("job is already done");
            _value = value;
            _state = State.Finished;
            Settle();
        }

        /// <summary>Finishes the job with <paramref name="error"/>; an <see cref="InvalidStateError"/> if it is done.</summary>
        public void SetException(System.Exception error)
        {
            if (error == null) throw new TypeError("SetException needs an exception");
            if (Done()) throw new InvalidStateError("job is already done");
            _error = ExceptionDispatchInfo.Capture(error);
            _state = State.Finished;
            Settle();
        }

        /// <summary>The worker's start: true only for a pending job (a cancelled or tampered one is skipped).</summary>
        internal bool TryStart()
        {
            if (_state != State.Pending) return false;
            _state = State.Running;
            return true;
        }

        private void Settle()
        {
            var callbacks = _callbacks;
            _callbacks = null; // each runs once; one added from inside a callback runs at once (the job is done)
            if (callbacks == null) return;
            foreach (var cb in callbacks) Invoke(cb);
        }

        private void Invoke(Action<Job> callback)
        {
            try
            {
                callback(this);
            }
            catch (System.Exception e)
            {
                try
                {
                    _callbackError(e);
                }
                catch (System.Exception)
                {
                    // a sink that throws has nowhere left to report to; the job and the other callbacks go on
                }
            }
        }
    }

    /// <summary>What the controllers take to run Engine work: <see cref="Worker.Submit"/> is one.</summary>
    public delegate Job SubmitFn(Func<Engine, Task<object?>> command);

    /// <summary>
    /// Delayed work, injected: the core never creates a timer. <see cref="Schedule"/> runs <c>fn</c> once, on the
    /// game's thread, after <c>seconds</c> (0: as soon as it can, never inline), and returns a handle for
    /// <see cref="Cancel"/>, which keeps a callback that has not run from running. Unity's glue implements it with its
    /// frame loop. The worker survives a callback run inline or after its Cancel (it ignores a stale one), but a
    /// scheduler that runs every 0-second callback inline recurses when the passes complete synchronously.
    /// </summary>
    public interface IScheduler
    {
        object Schedule(Action fn, double seconds);
        void Cancel(object handle);
    }

    /// <summary>
    /// Builds the commands <see cref="Worker.Submit"/> (a <see cref="SubmitFn"/>) takes, so callers need not write the
    /// <c>Task&lt;object?&gt;</c> plumbing: <c>submit(Commands.Sync(e =&gt; e.ReportHint(sid, tx)))</c>,
    /// <c>submit(Commands.Of(e =&gt; e.RestoreLookup(tx)))</c>. An async lambda converts directly too:
    /// <c>submit(async e =&gt; await e.StartSession(kind, chain, opts))</c>.
    /// </summary>
    public static class Commands
    {
        /// <summary>A command with no value: the job's result is null.</summary>
        public static Func<Engine, Task<object?>> Sync(Action<Engine> fn)
        {
            if (fn == null) throw new TypeError("a command is required");
            return e =>
            {
                fn(e);
                return Task.FromResult<object?>(null);
            };
        }

        /// <summary>
        /// A synchronous command whose value is the job's result. A Task value is refused (the job fails with a
        /// TypeError): it would be handed back unawaited, so use <see cref="Of{T}"/>.
        /// </summary>
        public static Func<Engine, Task<object?>> Sync<T>(Func<Engine, T> fn)
        {
            if (fn == null) throw new TypeError("a command is required");
            return e =>
            {
                object? value = fn(e);
                if (value is Task) throw new TypeError("Commands.Sync got a Task: use Commands.Of for an async command");
                return Task.FromResult(value);
            };
        }

        /// <summary>An async command whose awaited value is the job's result.</summary>
        public static Func<Engine, Task<object?>> Of<T>(Func<Engine, Task<T>> fn)
        {
            if (fn == null) throw new TypeError("a command is required");
            return async e => await fn(e);
        }

        /// <summary>An async command with no value: the job's result is null.</summary>
        public static Func<Engine, Task<object?>> Of(Func<Engine, Task> fn)
        {
            if (fn == null) throw new TypeError("a command is required");
            return async e =>
            {
                await fn(e);
                return null;
            };
        }
    }

    /// <summary>The single writer (see the file header).</summary>
    public sealed class Worker
    {
        private sealed class Command
        {
            public readonly Func<Engine, Task<object?>> Fn;
            public readonly Job Job;
            public Command(Func<Engine, Task<object?>> fn, Job job) { Fn = fn; Job = job; }
        }

        public Engine Engine { get; }
        /// <summary>The latest detached snapshot, replaced after each pass that got through its tick.</summary>
        public Snapshot Snapshot { get; private set; }
        /// <summary>Seconds between the loop's passes (Python's <c>interval</c>); read after every timer-driven run.</summary>
        public Func<double> Interval { get; }
        /// <summary>Gets the failure of a timer-driven run (Python's <c>on_error</c>); the loop goes on.</summary>
        public Action<Exception> OnError { get; }

        private readonly IScheduler _scheduler;
        private readonly Func<Task<List<StepOutcome>>> _tick;
        private readonly Func<Task<List<string>>> _reverify;
        private readonly Func<Snapshot> _snapshot;
        private readonly Queue<Command> _commands = new Queue<Command>();
        private List<StepOutcome> _outcomes = new List<StepOutcome>();
        private bool _first = true;
        private TaskCompletionSource<object?>? _chain; // the in-flight run plus its follow-ups
        private bool _again; // a Wake()/Submit()/RunOnce() landed mid-run
        private bool _started;
        private bool _stopped;
        private object? _timer;
        private bool _timerSet;
        private long _timerGen; // which Arm the live timer belongs to: a stale or inline Fire is told apart

        /// <param name="interval">Seconds between passes; default 15.</param>
        /// <param name="onError">Default: ignore (Python's <c>lambda e: None</c>).</param>
        public Worker(Engine engine, IScheduler scheduler, Func<double>? interval = null, Action<Exception>? onError = null)
            : this(engine, scheduler, interval, onError, null, null, null)
        {
        }

        /// <summary>The tests' seam: the Engine is sealed, so a fake tick, reverify and snapshot come in here.</summary>
        internal Worker(Engine engine, IScheduler scheduler, Func<double>? interval, Action<Exception>? onError,
                        Func<Task<List<StepOutcome>>>? tick, Func<Task<List<string>>>? reverify, Func<Snapshot>? snapshot)
        {
            Engine = engine ?? throw new TypeError("a Worker needs an engine");
            _scheduler = scheduler ?? throw new TypeError("a Worker needs a scheduler");
            Interval = interval ?? (() => 15.0);
            OnError = onError ?? (_ => { });
            _tick = tick ?? engine.Tick;
            _reverify = reverify ?? engine.Reverify;
            _snapshot = snapshot ?? engine.Snapshot;
            Snapshot = _snapshot();
        }

        /// <summary>
        /// Run <paramref name="fn"/>(engine) in the next pass, before its tick; returns its Job. A synchronous throw
        /// and a faulted Task both fail the job. On a stopped worker (not restarted) the job is cancelled at once;
        /// before the first <see cref="Start"/> it simply queues.
        /// </summary>
        public Job Submit(Func<Engine, Task<object?>> fn)
        {
            if (fn == null) throw new TypeError("a command is required");
            var job = new Job(Report);
            if (_stopped)
            {
                job.Cancel();
                return job;
            }
            _commands.Enqueue(new Command(fn, job));
            Wake();
            return job;
        }

        /// <summary>Tick as soon as the current run (if any) is over.</summary>
        public void Wake()
        {
            if (_chain != null) _again = true;
            else if (Running) Arm(0);
        }

        public List<StepOutcome> DrainOutcomes()
        {
            var output = _outcomes;
            _outcomes = new List<StepOutcome>();
            return output;
        }

        /// <summary>
        /// One pass (see the file header); the Task faults with the first exception a pass of the chain threw (a
        /// failing reverify or tick ends that pass, without a new snapshot; the reverify is not retried). After
        /// <see cref="Stop"/> an explicit call still runs one pass; only the loop and the follow-ups honour stop.
        /// </summary>
        public Task RunOnce()
        {
            if (_chain != null)
            {
                _again = true;
                return _chain.Task;
            }
            var chain = new TaskCompletionSource<object?>();
            _chain = chain; // before the first command runs: a re-entrant call joins as a follow-up
            _ = Drive(chain); // Drive never faults: it settles `chain` instead
            return chain.Task;
        }

        private async Task Drive(TaskCompletionSource<object?> chain)
        {
            Exception? failure = null;
            try
            {
                do
                {
                    _again = false;
                    try
                    {
                        await Pass();
                    }
                    catch (Exception e)
                    {
                        failure ??= e;
                    }
                } while (_again && !_stopped);
            }
            finally
            {
                _chain = null;
                _again = false;
            }
            if (failure != null)
            {
                chain.SetException(failure);
                // Mark the fault observed: a RunOnce() Task nobody awaits (a re-entrant call, a forgotten one) must not
                // be logged as an unobserved Task exception when it is collected. Awaiters still get the exception.
                _ = chain.Task.Exception;
            }
            else
            {
                chain.SetResult(null);
            }
        }

        private async Task Pass()
        {
            // Drain until empty, as Python's workers do: a command submitted by a running command runs in this pass,
            // before the tick.
            while (_commands.Count > 0)
            {
                var cmd = _commands.Dequeue();
                if (!cmd.Job.TryStart()) continue; // cancelled, or completed by someone else
                object? value = null;
                Exception? error = null;
                var job = cmd.Job;
                Engine.RefuseRequests = () => job.IsAbandoned; // this job's requests, refused once it is abandoned
                try
                {
                    var task = cmd.Fn(Engine) ?? throw new TypeError("a command returned null instead of a Task");
                    value = await task;
                }
                catch (Exception e)
                {
                    error = e;
                }
                finally
                {
                    Engine.RefuseRequests = null;
                }
                if (cmd.Job.Done())
                {
                    // settled from outside while it ran: the command's own failure has no job left to carry it
                    if (error != null) Report(error);
                    continue;
                }
                if (error != null) cmd.Job.SetException(error);
                else cmd.Job.SetResult(value);
            }
            if (_first)
            {
                _first = false;
                await _reverify();
            }
            foreach (var o in await _tick())
                if (o.Status != "waiting" || !string.IsNullOrEmpty(o.Error)) _outcomes.Add(o);
            Snapshot = _snapshot();
        }

        /// <summary>Starts the loop: a pass at once, then one every <see cref="Interval"/>. Does nothing while running.</summary>
        public void Start()
        {
            if (Running) return;
            _started = true;
            _stopped = false;
            Arm(0);
        }

        /// <summary>
        /// Cancels the timer and the queued jobs; no further loop or follow-up pass starts. A command or tick already in
        /// flight is allowed to finish.
        /// </summary>
        public void Stop()
        {
            _stopped = true;
            _started = false;
            Disarm();
            // Like PumpWorker.stop: queued commands will never run, so release their callers.
            var queued = _commands.ToArray();
            _commands.Clear();
            foreach (var cmd in queued) cmd.Job.Cancel();
        }

        /// <summary>True between <see cref="Start"/> and <see cref="Stop"/>.</summary>
        public bool Running => _started && !_stopped;

        private void Disarm()
        {
            if (!_timerSet) return;
            _timerSet = false;
            _timerGen++;
            var handle = _timer;
            _timer = null;
            if (handle != null) _scheduler.Cancel(handle);
        }

        private void Arm(double seconds)
        {
            Disarm();
            // The flag and the generation are set BEFORE Schedule (as worker.ts): a scheduler that fires inline, against
            // the contract, then clears them itself, and the handle is kept only if this Arm's timer is still the live one.
            long gen = ++_timerGen;
            _timerSet = true;
            object handle;
            try
            {
                handle = _scheduler.Schedule(() => Fire(gen), Math.Max(0, seconds));
            }
            catch (Exception)
            {
                if (gen == _timerGen) _timerSet = false;
                throw;
            }
            if (gen == _timerGen && _timerSet) _timer = handle;
        }

        private void Fire(long gen)
        {
            if (gen != _timerGen || !_timerSet) return; // a cancelled or superseded timer that fired anyway
            _timerSet = false;
            _timer = null;
            if (!Running) return;
            _ = FireRun(); // FireRun never faults: every failure goes to OnError or Report
        }

        private async Task FireRun()
        {
            try
            {
                await RunOnce();
            }
            catch (Exception e)
            {
                LoopError(e);
            }
            try
            {
                Rearm();
            }
            catch (Exception e)
            {
                // Interval() or the scheduler threw: no timer is set, and the next Wake()/Submit() arms one
                LoopError(e);
            }
        }

        private void Rearm()
        {
            if (!Running || _timerSet) return;
            Arm(_commands.Count > 0 ? 0 : Interval());
        }

        private void LoopError(Exception e)
        {
            try
            {
                OnError(e);
            }
            catch (Exception inner)
            {
                Report(inner);
            }
        }

        /// <summary>Where a crash with nowhere else to go is reported: the Engine's <see cref="Engine.OnError"/> as it is now.</summary>
        private void Report(Exception e)
        {
            try
            {
                (Engine.OnError ?? Errors.PrintError)(e);
            }
            catch (Exception)
            {
                // a handler that throws has nowhere left to report to
            }
        }
    }
}
