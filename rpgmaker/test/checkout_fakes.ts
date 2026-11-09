/** Test doubles for CheckoutController (twins of fakes.immediate_submitter and the FakeServer classes). Not a test file. */
import { Job } from "../src/core/worker.ts";
import type { Engine } from "../src/core/engine.ts";

const isThenable = (v: unknown): v is PromiseLike<unknown> =>
  typeof v === "object" && v !== null && typeof (v as { then?: unknown }).then === "function";

/**
 * Worker.submit stand-in that runs each command at once. A synchronous command (reportHint,
 * setUiStatus, discardSession) returns an already-done Job; an async one (startSession) is done
 * after a microtask, so tests `await submit.idle()` before ctl.poll().
 */
export function immediateSubmitter(engine: Engine) {
  const inflight = new Set<Promise<unknown>>();
  const submit = (fn: (e: Engine) => unknown): Job<any> => {
    const job = new Job<any>();
    job._start();
    try {
      const result = fn(engine);
      if (isThenable(result)) {
        const p = Promise.resolve(result).then((v) => job._succeed(v), (e) => job._fail(e));
        inflight.add(p);
        void p.finally(() => inflight.delete(p));
      } else {
        job._succeed(result);
      }
    } catch (e) {
      job._fail(e);
    }
    return job;
  };
  submit.idle = async (): Promise<void> => {
    while (inflight.size > 0) await Promise.all([...inflight]);
    await Promise.resolve();
  };
  return submit;
}

/** Worker.submit stand-in that runs commands only when run() is called (twin of DeferredSubmitter). */
export class DeferredSubmitter {
  queue: { fn: (e: Engine) => unknown; job: Job<any> }[] = [];
  readonly engine: Engine;
  constructor(engine: Engine) { this.engine = engine; }

  call = (fn: (e: Engine) => unknown): Job<any> => {
    const job = new Job<any>();
    this.queue.push({ fn, job });
    return job;
  };

  async run(): Promise<void> {
    while (this.queue.length > 0) {
      const { fn, job } = this.queue.shift()!;
      if (!job._start()) continue;
      try {
        job._succeed(await fn(this.engine));
      } catch (e) {
        job._fail(e);
      }
    }
    // let done-callbacks (promise reactions) queued by the jobs above run
    await new Promise<void>((r) => setTimeout(r, 0));
  }
}

export class FakeServer {
  static url = "http://127.0.0.1:1234/c/secret/";
  session_info: Record<string, any>;
  on_tx: (tx: string, sender?: string | null) => void;
  get_status: () => Record<string, any>;
  build_tx?: (account: string) => Job<any>;
  started = false;
  stopped = false;
  running = false;
  url = FakeServer.url;
  constructor(kw: {
    session_info: Record<string, any>; on_tx: (tx: string, sender?: string | null) => void;
    get_status: () => Record<string, any>; build_tx?: (account: string) => Job<any>;
  }) {
    this.session_info = kw.session_info;
    this.on_tx = kw.on_tx;
    this.get_status = kw.get_status;
    this.build_tx = kw.build_tx;
  }
  start(): void { this.started = true; this.running = true; }
  stop(): void { this.stopped = true; this.running = false; }
}

export class PageWithPump extends FakeServer {
  pumps = 0;
  reopens = 0;
  pump(): void { this.pumps++; }
  reopen(): void { this.reopens++; }
}
