/**
 * The SCFG plugin booted on the MZ shim the way MZ loads it: plugin parameters registered (as
 * `PluginManager.setup` does, from MZ-encoded strings), `bootPlugin()` (boot.ts) run at
 * plugin-load time, then Scene_Boot, a new game and Scene_Map. For the tests of the plugin
 * commands, the owned switches and the Tip Jar menu (src/mz/{commands,switches,menu}.test.ts).
 *
 * The sheet host is a fake (a sheet is a handle whose `closed` the test resolves), the chains are
 * FakeChains, and a worker pass runs only on `pass()`. `nw.Shell.openExternal` records the URLs.
 */
import assert from "node:assert/strict";
import { FakeChain, transferLog } from "./chain_fakes.ts";
import { FakeClock, FakeTransport } from "./rpc_fakes.ts";
import { MemoryStorage, bootShim, installShim, type Shim, type ShimOptions } from "./mz_shim/index.ts";
import { MAINNET, type Chain, type CheckoutController, type RestoreController } from "../src/core/index.ts";
import { bootPlugin } from "../src/mz/boot.ts";
import type { Scfg } from "../src/mz/scfg.ts";
import type { SheetHandle, SheetHost } from "../src/mz/sheet_host.ts";

const g = globalThis as any;

export const DEV = "0x52908400098527886E0F7030069857D2E4169EE7";
export const PAYER = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed";
export const TX = "0x" + "ab".repeat(32);
export const USDC_ARB = MAINNET["arbitrum"]!.tokens["USDC"]!.address;

/** An Items entry as the Plugin Manager stores it (every field a string). */
export interface ItemParam {
  id: string;
  code: string;
  price: string;
  token: string;
  title: string;
  minPrice: string;
  ownedSwitch: string;
}

export const CHAPTER2: ItemParam = { id: "chapter2", code: "37", price: "4.99", token: "USDC", title: "Chapter 2", minPrice: "", ownedSwitch: "1" };

/** A struct<Item>[] parameter as MZ encodes it: the JSON text of a list of JSON texts. */
export const encodeItems = (items: ItemParam[]): string => JSON.stringify(items.map((i) => JSON.stringify(i)));

export interface Sheet {
  kind: "payment" | "restore";
  ctl: any;
  handle: SheetHandle;
  close(how: string): void;
}

/** The sheet host the flows show their screens through, recording what they ask for. */
export class FakeHost implements SheetHost {
  sheets: Sheet[] = [];
  closes = 0;
  notes: string[] = [];

  openPayment(ctl: CheckoutController): SheetHandle | null {
    return this._open("payment", ctl);
  }

  openRestore(ctl: RestoreController): SheetHandle | null {
    return this._open("restore", ctl);
  }

  _open(kind: Sheet["kind"], ctl: unknown): SheetHandle {
    let close!: (how: string) => void;
    const handle: SheetHandle = { closed: new Promise<any>((resolve) => (close = resolve)) };
    this.sheets.push({ kind, ctl, handle, close });
    return handle;
  }

  close(_handle: SheetHandle): void {
    this.closes++;
  }

  notify(text: string): void {
    this.notes.push(text);
  }

  get last(): Sheet {
    assert.ok(this.sheets.length > 0, "a sheet is open");
    return this.sheets[this.sheets.length - 1]!;
  }
}

export interface Glue {
  shim: Shim;
  s: Scfg;
  host: FakeHost;
  storage: MemoryStorage;
  chains: Record<string, FakeChain>;
  clock: FakeClock;
  /** console.error lines ("scfg: ..."). */
  errors: string[];
  /** Dev notices shown over the game (test play). */
  notices: string[];
  /** URLs opened with nw.Shell.openExternal. */
  opened: string[];
}

/**
 * Boot the plugin with these parameters (defaults: the demo's wallet on mainnet, chapter2 with
 * owned switch 1). With `storage` it boots on what an earlier run stored; `commonEvents` are the
 * database's common events (the shim's option).
 */
export async function bootGlue(opts: {
  params?: Record<string, string>; storage?: MemoryStorage; start?: boolean; commonEvents?: ShimOptions["commonEvents"];
} = {}): Promise<Glue> {
  const storage = opts.storage ?? new MemoryStorage();
  const parameters = opts.params ?? { wallet: DEV, testnet: "false", items: encodeItems([CHAPTER2]) };
  const shim = installShim(g, {
    nwjs: true, storage, options: ["test"], plugins: [{ name: "SCFG", status: true, parameters }], commonEvents: opts.commonEvents,
  });
  const chains: Record<string, FakeChain> = {};
  const clock = new FakeClock();
  const errors: string[] = [];
  const notices: string[] = [];
  const opened: string[] = [];
  const factory = (chain: Chain) => {
    chains[chain.key] ??= new FakeChain(1000);
    chains[chain.key]!.chain_obj = chain;
    return chains[chain.key]!;
  };
  const line = (args: unknown[]) => args.map((a) => (a instanceof Error ? `${a.name}: ${a.message}` : String(a))).join(" ");
  const nw = { Shell: { openExternal: (url: string) => void opened.push(url) } };
  const s = bootPlugin({
    clock: clock.call, transport: new FakeTransport({}),
    engine_opts: { rpc_factory: factory, clock: clock.call },
    worker_opts: { schedule: () => null, cancel: () => undefined },
    log: { error: (...a: unknown[]) => void errors.push(line(a)), warn: () => undefined },
    platform: { devNotice: (text: string) => void notices.push(text), nw: () => nw, window: () => shim.window, navigator: () => ({}) },
  });
  const host = new FakeHost();
  s.sheetHost = host;
  const glue: Glue = { shim, s, host, storage, chains, clock, errors, notices, opened };
  if (opts.start === false) return glue;
  await bootShim();
  await s._mirror.idle();
  shim.frame();
  if (s._disabled !== null) return glue; // invalid parameters: payments are off
  assert.ok(s._worker !== null, "the pump started the worker");
  await s._worker.runOnce();
  shim.frame();
  return glue;
}

/** One worker pass, then one frame. */
export async function pass(glue: Glue): Promise<void> {
  await glue.s._worker!.runOnce();
  glue.shim.frame();
}

/** Lets promise reactions run (a sheet's `closed`, a command's Promise). */
export const tick = () => new Promise<void>((r) => setTimeout(r, 0));

/** The open sheet's controller, with a session started on Arbitrum. */
export async function session(glue: Glue): Promise<any> {
  const ctl = glue.host.last.ctl;
  ctl.chooseNetwork("arbitrum");
  await pass(glue);
  assert.ok(ctl.sessionId, "a session started");
  return ctl;
}

/** Pays the session on the fake chain in a new block and lets it confirm (Arbitrum: 20 confirmations). */
export async function payOnChain(glue: Glue, ctl: any, tx = TX): Promise<void> {
  const chain = glue.chains["arbitrum"]!;
  const block = chain.head + 1;
  chain.logs.push(transferLog(USDC_ARB, PAYER, DEV, BigInt(ctl.session.units), { block, tx_hash: tx }));
  chain.head = block;
  await pass(glue);
  chain.head = block + 19;
  await pass(glue);
}

/** An event command list: a plugin command (MZ code 357) of SCFG, then the end of the list. */
export function pluginCommand(command: string, args: Record<string, string>, text = command): { code: number; indent: number; parameters: any[] }[] {
  return [
    { code: 357, indent: 0, parameters: ["SCFG", command, text, args] },
    { code: 0, indent: 0, parameters: [] },
  ];
}

/** A Game_Interpreter running `list` (one update; it then waits or ends). */
export function run(list: { code: number; indent: number; parameters: any[] }[]): any {
  const interpreter = new g.Game_Interpreter();
  interpreter.setup(list);
  interpreter.update();
  return interpreter;
}
