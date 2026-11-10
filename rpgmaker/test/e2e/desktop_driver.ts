/**
 * TEST ONLY. The desktop (NW.js) path of the e2e (tools/run_rpgmaker_desktop_e2e.py, Ruling M2-5):
 * NW.js itself cannot run here, so this Node process plays it. It runs the real SCFG glue
 * (`bootPlugin()`, boot.ts, from the same sources SCFG.js bundles) on the MZ shim as a desktop game
 * (`Utils.isNwjs()` true, saves in `<storage>/save/*.rmmzsave` through MZ's StorageManager), with
 * NW.js's Node `require` (the glue reaches `net`, `crypto`, `https` through it): the real
 * localhost checkout page (local_server.ts on Node's net) and the real Node https transport
 * (node_transport.ts) against the runner's local chain. NW.js's `nw.Shell.openExternal` and
 * `nw.Clipboard` are recorded (the runner opens the URL in Playwright; the Copy buttons' text is
 * read back). The sheet is a headless host (headless_host.ts): it shows nothing, but reports
 * what the plugin's sheet would show (the controller's visible values and the buttons of its
 * step, named as ui/sheet.ts names them) and presses a button by name the way that button does,
 * nothing more. Game frames run 60 times a second (SceneManager.updateMain, as MZ's main loop);
 * the Worker runs on its real timers.
 *
 * Protocol: one JSON command per stdin line, one reply line `SCFG-REPLY {"ok": ...}` or
 * `SCFG-REPLY {"error": "..."}` on stdout, in order (any other stdout line is the game's output).
 *   boot     {storage, parameters?, chain?}  the demo kit's js/plugins.js with these parameters
 *            replaced; chain = {network, key, rpc: [url], confirmations?, tokens?: {name: address}}
 *            points that built-in chain at the local chain (as the Godot e2e driver's
 *            SCFG_E2E_CHAIN). Boots to the map: the stored state is read.
 *   event    {name}       start the demo map's event (Map001.json, "Merchant", "Bard", ...) as the
 *                         player talking to it does: its page 1 runs on the map's interpreter
 *   snapshot              {scene, transport, eventRunning, switches, variables, screens: [...], copied, opened, notes}
 *   press    {name}       press a button of the newest screen (see HeadlessHost)
 *   owns     {item}       SCFG.owns(item)
 *   quit                  MZ's exit (SceneManager.exit: the plugin flushes its save), then this process ends
 */
import { createRequire } from "node:module";
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { createInterface } from "node:readline";
import { fileURLToPath } from "node:url";
import { runInNewContext } from "node:vm";
import { MAINNET, TESTNET, makeToken, type Chain } from "../../src/core/chains.ts";
import { bootPlugin } from "../../src/mz/boot.ts";
import type { Scfg } from "../../src/mz/scfg.ts";
import { installDiskAssets } from "../mz_assets.ts";
import { installShim, type Shim } from "../mz_shim/index.ts";
import { DirStorage } from "../mz_shim/node_storage.ts";
import { DriverError, HeadlessHost } from "./headless_host.ts";

const REPLY = "SCFG-REPLY ";
const FRAME_MS = 1000 / 60;
const QUIT_WAIT_MS = 10_000;
const DEMO = join(dirname(dirname(dirname(fileURLToPath(import.meta.url)))), "demo");

const g = globalThis as any;

// --- the game ------------------------------------------------------------------------------------

let shim: Shim | null = null;
let scfg: Scfg | null = null;
let host: HeadlessHost | null = null;
let loop: ReturnType<typeof setInterval> | null = null;
let terminated: Promise<void> | null = null;
const copied: string[] = [];
const opened: string[] = [];

/** The demo kit's js/plugins.js, run as MZ's index.html runs it. */
function demoPlugins(): any[] {
  const sandbox: Record<string, unknown> = {};
  runInNewContext(readFileSync(join(DEMO, "js", "plugins.js"), "utf8"), sandbox);
  return sandbox["$plugins"] as any[];
}

/** Point a built-in chain at the local chain (as engine_driver.ts / the Godot e2e driver). */
function applyChain(o: any): void {
  const registry: Record<string, Chain> = o.network === "testnet" ? TESTNET : MAINNET;
  const base = registry[o.key];
  if (base === undefined) throw new DriverError(`unknown chain ${JSON.stringify(o.key)}`);
  const next: Chain = { ...base, rpc: [...o.rpc] };
  if (o.confirmations !== undefined && o.confirmations !== null) next.confirmations = o.confirmations;
  if (o.tokens) next.tokens = Object.fromEntries(Object.entries(o.tokens as Record<string, string>).map(([k, a]) => [k, makeToken(a)]));
  registry[o.key] = next;
}

async function boot(cmd: any): Promise<unknown> {
  if (scfg !== null) throw new DriverError("already booted");
  if (typeof cmd.storage !== "string" || cmd.storage === "") throw new DriverError("boot needs a storage directory");
  if (cmd.chain) applyChain(cmd.chain);
  const plugins = demoPlugins();
  const entry = plugins.find((p) => p.name === "SCFG");
  for (const [k, v] of Object.entries((cmd.parameters ?? {}) as Record<string, unknown>)) {
    if (typeof v !== "string") throw new DriverError(`parameter ${k} must be a string, as MZ stores it`);
    entry.parameters[k] = v;
  }
  installDiskAssets();
  shim = installShim(g, { nwjs: true, options: ["test"], storage: new DirStorage(cmd.storage), plugins });
  // NW.js: Node's require in the page, and the nw object (its shell and clipboard, recorded)
  g.require = createRequire(import.meta.url);
  g.nw = {
    Shell: { openExternal: (url: string) => void opened.push(String(url)) },
    Clipboard: { get: () => ({ set: (text: string) => void copied.push(String(text)), get: () => copied[copied.length - 1] ?? "" }) },
  };
  // MZ's exit ends in SceneManager.terminate (nw.App.quit): the plugin's alias runs this after its flush
  terminated = new Promise<void>((resolve) => {
    g.SceneManager.terminate = () => resolve();
  });
  // as MZ loads the plugin script: the parameters are registered, nothing else is loaded yet
  scfg = bootPlugin();
  host = new HeadlessHost(scfg);
  scfg.sheetHost = host;
  await shim.boot();
  loop = setInterval(() => {
    if (shim?.stopped) return; // MZ's game loop stopped on an error (SceneManager.onError)
    try {
      g.SceneManager.updateMain();
    } catch (e) {
      process.stderr.write(`frame error: ${e instanceof Error ? (e.stack ?? e.message) : String(e)}\n`);
    }
  }, FRAME_MS);
  return { scene: g.SceneManager._scene?.constructor?.name ?? null, loaded: scfg.stateLoaded(), disabled: scfg._disabled };
}

/**
 * The booted game; a DriverError when an error stopped it. The shim models MZ's
 * SceneManager.onError/onReject (an uncaught error or an unhandled rejection stops an MZ game):
 * every command after that fails, so the runner fails at once.
 */
function need(): { scfg: Scfg; host: HeadlessHost } {
  if (scfg === null || host === null) throw new DriverError("boot first");
  if (shim !== null && shim.stopped) {
    const why = shim.errors.map((e) => (e instanceof Error ? `${e.name}: ${e.message}` : String(e))).join("; ");
    throw new DriverError(`the game stopped on an error (SceneManager.onError): ${why}`);
  }
  return { scfg, host };
}

function startEvent(name: unknown): null {
  need();
  const map = JSON.parse(readFileSync(join(DEMO, "data", "Map001.json"), "utf8"));
  const ev = map.events.find((e: any) => e && (e.name === name || String(e.name).split(" (")[0] === name));
  if (!ev) throw new DriverError(`no event ${JSON.stringify(name)} on the demo map`);
  const interpreter = g.$gameMap._interpreter;
  if (interpreter.isRunning()) throw new DriverError("an event is already running");
  interpreter.setup(ev.pages[0].list, ev.id);
  return null;
}

function snapshot(): unknown {
  const { host } = need();
  const ids = [1, 2, 3];
  return {
    scene: g.SceneManager._scene?.constructor?.name ?? null,
    // the Engine's transport (the glue's default on NW.js: Node https, Ruling M2-1)
    transport: need().scfg._engine?.transport?.constructor?.name ?? null,
    eventRunning: g.$gameMap.isEventRunning(),
    switches: Object.fromEntries(ids.map((i) => [i, g.$gameSwitches.value(i)])),
    variables: Object.fromEntries(ids.map((i) => [i, g.$gameVariables.value(i)])),
    screens: host.screens.map((s) => host.view(s)),
    copied: [...copied], opened: [...opened], notes: [...host.notes],
  };
}

async function quit(): Promise<null> {
  need();
  g.SceneManager.exit();
  let timer: ReturnType<typeof setTimeout> | undefined;
  const late = new Promise<never>((_, reject) => {
    timer = setTimeout(() => reject(new DriverError("the game did not terminate")), QUIT_WAIT_MS);
  });
  try {
    await Promise.race([terminated, late]);
  } finally {
    clearTimeout(timer);
  }
  if (loop !== null) clearInterval(loop);
  loop = null;
  return null;
}

async function run(cmd: any): Promise<unknown> {
  switch (cmd.op) {
    case "boot":
      return boot(cmd);
    case "event":
      return startEvent(cmd.name);
    case "snapshot":
      return snapshot();
    case "press":
      need().host.press(String(cmd.name));
      return null;
    case "owns":
      return need().scfg.owns(cmd.item);
    case "quit":
      return quit();
    default:
      throw new DriverError(`unknown op ${JSON.stringify(cmd.op)}`);
  }
}

const rl = createInterface({ input: process.stdin, crlfDelay: Infinity });
for await (const line of rl) { // sequential: the next command waits for the previous reply
  if (line.trim() === "") continue;
  let reply: string;
  let cmd: any = null;
  try {
    cmd = JSON.parse(line);
    reply = JSON.stringify({ ok: (await run(cmd)) ?? null });
  } catch (err) {
    reply = JSON.stringify({ error: err instanceof Error ? `${err.name}: ${err.message}` : String(err) });
  }
  process.stdout.write(REPLY + reply + "\n");
  if (cmd?.op === "quit" && reply.startsWith('{"ok"')) break;
}
if (loop !== null) clearInterval(loop);
process.exit(0);
