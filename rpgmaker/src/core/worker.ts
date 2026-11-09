/**
 * The single writer: one command queue plus a periodic tick loop that owns the Engine.
 *
 * Twin of scfg_core.worker.Worker (threads) and scfg_core.pump_worker.PumpWorker (Ren'Py web),
 * as one cooperative async object. The game loop never touches the Engine directly. It:
 *   - submits commands with `submit(fn)` and polls the returned `Job` every frame,
 *   - reads the latest detached `Snapshot` from `.snapshot` (replaced after each run),
 *   - collects StepOutcomes with `drainOutcomes()`.
 * At most one `runOnce` is in flight, so a command never executes inside a tick.
 */
import type { Engine, Snapshot } from "./engine.ts";
import type { StepOutcome } from "./watcher.ts";

/** Python's concurrent.futures.CancelledError. */
export class CancelledError extends Error {
  constructor() {
    super("job cancelled");
    this.name = "CancelledError";
  }
}

/** Stands in for Python's Future, for the UI's per-frame poll(). */
export class Job<T> {
  readonly promise: Promise<T>;
  private _state: "pending" | "running" | "ok" | "failed" | "cancelled" = "pending";
  private _value: T | undefined;
  private _error: unknown = null;
  private readonly _cancelledError = new CancelledError();
  private _resolve!: (v: T) => void;
  private _reject!: (e: unknown) => void;

  constructor() {
    this.promise = new Promise<T>((res, rej) => {
      this._resolve = res;
      this._reject = rej;
    });
    // A caller that only polls done()/result() never observes the promise; this keeps a
    // failed or cancelled job from raising an unhandledRejection. Awaiting `promise` still rejects.
    this.promise.catch(() => {});
  }

  done(): boolean {
    return this._state === "ok" || this._state === "failed" || this._state === "cancelled";
  }

  cancelled(): boolean {
    return this._state === "cancelled";
  }

  /** The value; raises the job's own exception if it failed, like Future.result(). */
  result(): T {
    if (this._state === "ok") return this._value as T;
    if (this._state === "failed") throw this._error;
    if (this._state === "cancelled") throw this._cancelledError;
    throw new Error("job is not done");
  }

  /**
   * The failure, or null when the job succeeded (or is not done). A cancelled job returns its
   * CancelledError (Future.exception() raises it), so ported poll() code lands in its failure branch.
   */
  exception(): unknown | null {
    if (this._state === "cancelled") return this._cancelledError;
    return this._state === "failed" ? this._error : null;
  }

  /** Cancels a job that has not started; false once it is running or finished. */
  cancel(): boolean {
    if (this._state === "cancelled") return true;
    if (this._state !== "pending") return false;
    this._state = "cancelled";
    this._reject(this._cancelledError);
    return true;
  }

  /** @internal Worker: false when the job was cancelled before it could start. */
  _start(): boolean {
    if (this._state !== "pending") return false;
    this._state = "running";
    return true;
  }

  /** @internal */
  _succeed(v: T): void {
    this._state = "ok";
    this._value = v;
    this._resolve(v);
  }

  /** @internal */
  _fail(e: unknown): void {
    this._state = "failed";
    this._error = e;
    this._reject(e);
  }
}

export interface WorkerOptions {
  /** Seconds between ticks (Python's `interval`). */
  interval?: () => number;
  on_error?: (e: unknown) => void;
  schedule?: (fn: () => void, ms: number) => unknown;
  cancel?: (handle: unknown) => void;
}

// The src tsconfig has neither DOM nor node types, so reach the timers through globalThis.
const timers = globalThis as unknown as {
  setTimeout(fn: () => void, ms: number): unknown;
  clearTimeout(handle: unknown): void;
};
const defaultSchedule = (fn: () => void, ms: number): unknown => timers.setTimeout(fn, ms);
const defaultCancel = (handle: unknown): void => timers.clearTimeout(handle);

type Command = { fn: (e: Engine) => unknown; job: Job<any> };

export class Worker {
  readonly engine: Engine;
  readonly interval: () => number;
  readonly on_error: (e: unknown) => void;
  snapshot: Snapshot;

  private readonly _schedule: (fn: () => void, ms: number) => unknown;
  private readonly _cancel: (handle: unknown) => void;
  private _commands: Command[] = [];
  private _outcomes: StepOutcome[] = [];
  private _first = true;
  private _chain: Promise<void> | null = null; // the in-flight runOnce (plus its follow-ups)
  private _again = false; // a wake()/submit()/runOnce() landed mid-run
  private _started = false;
  private _stopped = false;
  private _timer: unknown = null;
  private _timerSet = false;

  constructor(engine: Engine, opts: WorkerOptions = {}) {
    this.engine = engine;
    this.interval = opts.interval ?? (() => 15);
    this.on_error = opts.on_error ?? (() => {});
    this._schedule = opts.schedule ?? defaultSchedule;
    this._cancel = opts.cancel ?? defaultCancel;
    this.snapshot = engine.snapshot();
  }

  /**
   * Run fn(engine) in the next run, before its tick; returns a Job for the result. On a worker
   * that was stopped (and not restarted) the Job is cancelled at once; before the first start()
   * it simply queues.
   */
  submit<T>(fn: (e: Engine) => T | Promise<T>): Job<T> {
    const job = new Job<T>();
    if (this._stopped) {
      job.cancel();
      return job;
    }
    this._commands.push({ fn, job });
    this.wake();
    return job;
  }

  /** Tick as soon as the current run (if any) is over. */
  wake(): void {
    if (this._chain) {
      this._again = true;
    } else if (this._started && !this._stopped) {
      this._arm(0);
    }
  }

  drainOutcomes(): StepOutcome[] {
    const out = this._outcomes;
    this._outcomes = [];
    return out;
  }

  /**
   * One pass: the commands queued before it, reverify() on the first pass, tick(), snapshot.
   * Called while a run is in flight it adds exactly one follow-up run and resolves when that
   * follow-up is done. Rejects with the first error a pass raised. After stop() an explicit
   * call still runs one pass; only the loop and follow-up passes honour stop.
   */
  runOnce(): Promise<void> {
    if (this._chain) {
      this._again = true;
      return this._chain;
    }
    // The body starts in a microtask so `_chain` is set before any command runs: a command that
    // calls runOnce()/wake()/submit() synchronously then joins this chain as a follow-up.
    const chain = Promise.resolve().then(async () => {
      let failure: { e: unknown } | null = null;
      try {
        do {
          this._again = false;
          try {
            await this._pass();
          } catch (e) {
            failure ??= { e };
          }
        } while (this._again && !this._stopped);
      } finally {
        this._chain = null;
        this._again = false;
      }
      if (failure) throw failure.e;
    });
    this._chain = chain;
    return chain;
  }

  private async _pass(): Promise<void> {
    const batch = this._commands;
    this._commands = []; // commands submitted from here on belong to the next pass
    for (const { fn, job } of batch) {
      if (!job._start()) continue;
      try {
        job._succeed(await fn(this.engine));
      } catch (e) {
        job._fail(e);
      }
    }
    if (this._first) {
      this._first = false;
      await this.engine.reverify();
    }
    for (const o of await this.engine.tick()) {
      if (o.status !== "waiting" || o.error) this._outcomes.push(o);
    }
    this.snapshot = this.engine.snapshot();
  }

  start(): void {
    if (this._started && !this._stopped) return;
    this._started = true;
    this._stopped = false;
    this._arm(0);
  }

  /**
   * Cancels the timer and queued jobs; no further loop or follow-up runs start. A batch or tick
   * already in flight is allowed to finish.
   */
  stop(): void {
    this._stopped = true;
    this._started = false;
    this._disarm();
    // Like PumpWorker.stop: queued commands will never run, so release their callers.
    const queued = this._commands;
    this._commands = [];
    for (const { job } of queued) job.cancel();
  }

  get running(): boolean {
    return this._started && !this._stopped;
  }

  private _disarm(): void {
    if (this._timerSet) {
      this._timerSet = false;
      this._cancel(this._timer);
      this._timer = null;
    }
  }

  private _arm(seconds: number): void {
    this._disarm();
    this._timerSet = true;
    this._timer = this._schedule(() => this._fire(), Math.max(0, seconds) * 1000);
  }

  private _fire(): void {
    this._timerSet = false;
    this._timer = null;
    if (!this.running) return;
    this.runOnce().then(
      () => this._rearm(),
      (e) => {
        try {
          this.on_error(e);
        } finally {
          this._rearm();
        }
      },
    );
  }

  private _rearm(): void {
    if (!this.running || this._timerSet) return;
    this._arm(this._commands.length ? 0 : this.interval());
  }
}
