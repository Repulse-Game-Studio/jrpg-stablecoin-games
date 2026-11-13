/**
 * JSON-lines driver for the shared server security suite (tests/server_security/): runs one
 * desktop CheckoutServer (src/mz/local_server.ts, on Node's net) at a time, with fake callbacks,
 * as godot/tests/integration/server_driver.gd does for the GDScript server.
 *
 *   node test/integration/server_driver.ts      (from rpgmaker/)
 *
 * Protocol (tests/server_security/server_handles.py): one JSON command per stdin line, one reply
 * line per command, in order, `SCFG-REPLY {"ok": <result>}` or `SCFG-REPLY {"error": "<why>"}`:
 *   start  {config}  stop the previous server, start a new one and wait until it listens;
 *          ok = {url, port, secret}
 *          config = {assets: {name: base64}, session_info, status,
 *                    logo: null | "broken" | {b64, ctype},
 *                    build: null | {mode: "ok", value, delay_s?} | {mode: "fail"} | {mode: "raise"}
 *                           | {mode: "never"},
 *                    build_timeout_s?}
 *   events        ok = {calls: [[name, args]...], jobs: [{done, cancelled}...], running}: every
 *                 callback the server made (on_tx, build_tx) and the state of each build job
 *   stop          stop the server; ok = {running}
 * End of stdin quits. Nothing else is written to stdout; anything on stderr is a failure for the
 * suite (an exception in a socket handler lands there).
 */
import crypto from "node:crypto";
import net from "node:net";
import { createInterface } from "node:readline";
import { Job } from "../../src/core/worker.ts";
import { CheckoutServer, type ServerOptions } from "../../src/mz/local_server.ts";

const REPLY_PREFIX = "SCFG-REPLY ";

let server: CheckoutServer | null = null;
let calls: unknown[][] = [];
let jobs: Job<unknown>[] = [];
let timers: ReturnType<typeof setTimeout>[] = [];
let config: Record<string, any> = {};

const bytes = (b64: string): Uint8Array => new Uint8Array(Buffer.from(b64, "base64"));

function stopServer(): void {
  if (server !== null) server.stop();
}

function buildTx(account: string): Job<unknown> {
  calls.push(["build_tx", [account]]);
  const build = config["build"];
  if (build["mode"] === "raise") throw new Error("submit failed");
  const job = new Job<unknown>();
  jobs.push(job);
  if (build["mode"] === "fail") {
    job._fail(new Error("timed out"));
  } else if (build["mode"] === "ok") {
    const delay = Number(build["delay_s"] ?? 0);
    if (delay > 0) {
      timers.push(setTimeout(() => {
        if (!job.done()) job._succeed(build["value"]);
      }, delay * 1000));
    } else {
      job._succeed(build["value"]);
    }
  }
  return job;
}

async function start(cfg: unknown): Promise<unknown> {
  if (typeof cfg !== "object" || cfg === null || Array.isArray(cfg)) throw new Error("config must be an object");
  stopServer();
  for (const t of timers) clearTimeout(t);
  config = cfg as Record<string, any>;
  calls = [];
  jobs = [];
  timers = [];
  const assets: Record<string, string> = config["assets"] ?? {};
  const opts: ServerOptions = {
    net, crypto: crypto.webcrypto as any,
    load_asset: (name: string) => {
      if (!Object.prototype.hasOwnProperty.call(assets, name)) throw new Error(`no asset ${name}`);
      return bytes(assets[name]!);
    },
    session_info: config["session_info"] ?? {},
    on_tx: (tx: string, sender: string | null) => void calls.push(["on_tx", [tx, sender]]),
    get_status: () => config["status"] ?? {},
  };
  const logo = config["logo"];
  if (logo === "broken") {
    opts.load_logo = () => {
      throw new Error("gone");
    };
  } else if (logo !== null && typeof logo === "object") {
    opts.load_logo = () => [bytes(logo["b64"]), logo["ctype"]];
  }
  if (config["build"] !== null && typeof config["build"] === "object") opts.build_tx = buildTx;
  const srv = new CheckoutServer(opts);
  if (config["build_timeout_s"] !== undefined) srv.build_timeout_s = Number(config["build_timeout_s"]);
  server = srv;
  srv.start();
  await srv.ready();
  return { url: srv.url, port: srv.port, secret: srv.secret };
}

async function run(cmd: any): Promise<unknown> {
  switch (cmd.op) {
    case "start":
      return start(cmd.config ?? {});
    case "events":
      return {
        calls, jobs: jobs.map((j) => ({ done: j.done(), cancelled: j.cancelled() })),
        running: server !== null && server.running,
      };
    case "stop":
      stopServer();
      return { running: server !== null && server.running };
  }
  throw new Error(`unknown op ${JSON.stringify(cmd.op)}`);
}

const reply = (obj: unknown) => process.stdout.write(REPLY_PREFIX + JSON.stringify(obj) + "\n");

const lines = createInterface({ input: process.stdin, crlfDelay: Infinity });
// One command at a time, in order: the next waits for the previous reply.
let queue: Promise<void> = Promise.resolve();
lines.on("line", (line) => {
  queue = queue.then(async () => {
    let cmd: unknown;
    try {
      cmd = JSON.parse(line);
    } catch {
      reply({ error: `not JSON: ${line.slice(0, 200)}` });
      return;
    }
    if (typeof cmd !== "object" || cmd === null || Array.isArray(cmd)) {
      reply({ error: `not a JSON object: ${line.slice(0, 200)}` });
      return;
    }
    try {
      reply({ ok: await run(cmd) });
    } catch (e) {
      reply({ error: e instanceof Error ? e.message : String(e) });
    }
  });
});
lines.on("close", () => {
  void queue.then(() => {
    stopServer();
    for (const t of timers) clearTimeout(t);
  });
});
