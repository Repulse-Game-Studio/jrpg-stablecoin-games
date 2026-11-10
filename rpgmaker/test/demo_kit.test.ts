/**
 * The asset-free demo kit (rpgmaker/demo/, D18): its MZ data files parse and have the shape MZ
 * reads; every plugin command its events call is declared in the plugin header with the same
 * arguments; its js/plugins.js entry parses with `parseParams` and boots the plugin without an
 * error; and it carries no RTP asset (no image, audio or face/character names).
 */
import { afterEach, test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync, readdirSync, statSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { runInNewContext } from "node:vm";
import { readHeader, type Header } from "./mz_header_reader.ts";
import { FakeTransport } from "./rpc_fakes.ts";
import { bootShim, installShim, resetShim } from "./mz_shim/index.ts";
import { PARAM, parseParams } from "../src/mz/params.ts";
import { PLUGIN_NAME, renderHeader } from "../src/mz/plugin_header.ts";
import { bootPlugin } from "../src/mz/boot.ts";

const g = globalThis as any;
const DEMO = join(dirname(dirname(fileURLToPath(import.meta.url))), "demo");
const DEMO_WALLET = "0x52908400098527886E0F7030069857D2E4169EE7";

afterEach(() => resetShim());

const readJson = (name: string): any => JSON.parse(readFileSync(join(DEMO, "data", name), "utf8"));

/** The demo's js/plugins.js, run as MZ's index.html runs it: `$plugins`. */
function demoPlugins(): any[] {
  const sandbox: Record<string, unknown> = {};
  runInNewContext(readFileSync(join(DEMO, "js", "plugins.js"), "utf8"), sandbox);
  assert.ok(Array.isArray(sandbox["$plugins"]), "plugins.js defines $plugins");
  return sandbox["$plugins"] as any[];
}

function scfgEntry(): any {
  const entries = demoPlugins().filter((p) => p.name === PLUGIN_NAME);
  assert.equal(entries.length, 1, "one SCFG entry");
  return entries[0];
}

interface Command {
  code: number;
  indent: number;
  parameters: any[];
}

/** Every event page's command list of the map, with where it is. */
function lists(map: any): [string, Command[]][] {
  const out: [string, Command[]][] = [];
  for (const ev of map.events) {
    if (ev === null) continue;
    ev.pages.forEach((page: any, i: number) => out.push([`event ${ev.id} (${ev.name}) page ${i + 1}`, page.list]));
  }
  return out;
}

test("demoDataFilesParseAndHaveTheMapShape", () => {
  const files = readdirSync(join(DEMO, "data")).sort();
  assert.deepEqual(files, ["Map001.json", "MapInfos.json"], "the data files the kit replaces");
  const infos = readJson("MapInfos.json");
  assert.equal(infos[0], null);
  assert.deepEqual([infos.length, infos[1].id, infos[1].parentId, infos[1].order], [2, 1, 0, 1]);
  const map = readJson("Map001.json");
  for (const key of ["autoplayBgm", "autoplayBgs", "battleback1Name", "battleback2Name", "bgm", "bgs", "disableDashing", "displayName",
    "encounterList", "encounterStep", "height", "note", "parallaxLoopX", "parallaxLoopY", "parallaxName", "parallaxShow", "parallaxSx",
    "parallaxSy", "scrollType", "specifyBattleback", "tilesetId", "width", "data", "events"]) {
    assert.ok(key in map, `Map001.json has ${key}`);
  }
  assert.equal(map.data.length, map.width * map.height * 6, "six layers of width x height tiles");
  assert.ok(map.data.every((t: unknown) => Number.isInteger(t) && (t as number) >= 0));
  assert.equal(map.events[0], null);
  const positions = new Set<string>();
  map.events.forEach((ev: any, i: number) => {
    if (ev === null) return;
    assert.equal(ev.id, i, "an event's id is its index");
    assert.ok(ev.x >= 0 && ev.x < map.width && ev.y >= 0 && ev.y < map.height, `event ${i} is on the map`);
    const at = `${ev.x},${ev.y}`;
    assert.ok(!positions.has(at), `one event per tile (${at})`);
    positions.add(at);
    for (const page of ev.pages) {
      for (const key of ["conditions", "directionFix", "image", "list", "moveFrequency", "moveRoute", "moveSpeed", "moveType",
        "priorityType", "stepAnime", "through", "trigger", "walkAnime"]) {
        assert.ok(key in page, `event ${i}: a page has ${key}`);
      }
    }
  });
  for (const [where, list] of lists(map)) {
    const last = list[list.length - 1]!;
    assert.deepEqual([last.code, last.indent], [0, 0], `${where} ends with the end of the list`);
    // every branch is closed: 111 ... (411 ...) 412 at the same indent, an inner list ends with a 0 one deeper
    const open: number[] = [];
    list.forEach((c, n) => {
      if (c.code === 111) open.push(c.indent);
      if (c.code === 411) assert.equal(open[open.length - 1], c.indent, `${where} #${n}: else of the open branch`);
      if (c.code === 412) assert.equal(open.pop(), c.indent, `${where} #${n}: end of the open branch`);
      if (c.code === 411 || c.code === 412) assert.deepEqual([list[n - 1]!.code, list[n - 1]!.indent], [0, c.indent + 1], `${where} #${n}`);
    });
    assert.deepEqual(open, [], `${where}: no branch left open`);
  }
});

test("demoEventsCallOnlyDeclaredCommandsWithTheirArgs", () => {
  const h: Header = readHeader(renderHeader());
  const map = readJson("Map001.json");
  const called = new Set<string>();
  for (const [where, list] of lists(map)) {
    list.forEach((c, n) => {
      if (c.code !== 357) return;
      const [plugin, name, text, args] = c.parameters;
      assert.equal(plugin, PLUGIN_NAME, where);
      const cmd = h.commands.find((x) => x.name === name);
      assert.ok(cmd, `${where}: ${name} is declared in the header`);
      assert.equal(text, cmd.text, `${where}: the editor's text of ${name}`);
      assert.deepEqual(Object.keys(args).sort(), cmd.args.map((a) => a.name).sort(), `${where}: ${name}'s args`);
      for (const a of cmd.args) {
        assert.equal(typeof args[a.name], "string", `${where}: args arrive as strings`);
        if (a.type === "variable") assert.match(args[a.name], /^\d+$/, `${where}: ${a.name} is a variable number`);
      }
      // the editor's summary lines (657) follow the command
      const shown = list.slice(n + 1).findIndex((x) => x.code !== 657);
      assert.equal(shown, cmd.args.length, `${where}: one 657 line per argument`);
      called.add(name);
    });
  }
  assert.deepEqual([...called].sort(), h.commands.map((c) => c.name).sort(), "the demo uses every command");
});

test("demoDoorIsGatedByTheOwnedSwitch", () => {
  const map = readJson("Map001.json");
  const item = parseParams(scfgEntry().parameters).items.find((i) => i.id === "chapter2")!;
  const door = map.events.find((ev: any) => ev && ev.name === "Door");
  assert.ok(door, "a Door event");
  assert.equal(door.pages.length, 2);
  assert.equal(door.pages[0].conditions.switch1Valid, false, "page 1: locked");
  assert.deepEqual([door.pages[1].conditions.switch1Valid, door.pages[1].conditions.switch1Id], [true, item.ownedSwitch],
    "page 2: open while chapter2's owned switch is ON");
});

test("demoStartTilesAndTheWayToTheDoorAreFree", async () => {
  // The README's player start (8, 9), and the shim's (its $dataSystem's startX / startY, where the
  // harness pages and the e2e boot): no NPC stands there, nor in the column up to the door.
  const map = readJson("Map001.json");
  const readme = readFileSync(join(DEMO, "README.md"), "utf8");
  assert.match(readme, /right-click the tile \(8, 9\) > Set Starting Position/);
  installShim(g, {});
  await bootShim();
  const shimStart = [g.$dataSystem.startX, g.$dataSystem.startY];
  const door = map.events.find((ev: any) => ev && ev.name === "Door");
  const at = (x: number, y: number) => map.events.filter((ev: any) => ev && ev.x === x && ev.y === y).map((ev: any) => ev.name);
  for (const [x, y] of [[8, 9], shimStart]) assert.deepEqual(at(x!, y!), [], `nothing on the start tile (${x}, ${y})`);
  for (let y = door.y + 1; y <= 9; y++) assert.deepEqual(at(door.x, y), [], `the way to the door is free at (${door.x}, ${y})`);
  // the README's table names each event's tile
  for (const ev of map.events) {
    if (ev === null) continue;
    assert.ok(readme.includes(`| ${ev.id} ${ev.name.split(" (")[0]} | (${ev.x}, ${ev.y}) |`), `README lists event ${ev.id} at (${ev.x}, ${ev.y})`);
  }
});

test("demoPluginsJsParsesAndConfigures", async () => {
  const text = readFileSync(join(DEMO, "js", "plugins.js"), "utf8");
  assert.match(text, /PLACEHOLDER/, "the placeholder wallet is clearly marked");
  const entry = scfgEntry();
  assert.equal(entry.status, true);
  const h = readHeader(renderHeader());
  assert.deepEqual(Object.keys(entry.parameters).sort(), h.params.map((p) => p.name).sort(), "every parameter, as MZ writes them");
  for (const v of Object.values(entry.parameters)) assert.equal(typeof v, "string", "MZ stores every value as a string");
  const p = parseParams(entry.parameters);
  assert.deepEqual(p.configure, { testnet: true, wallet: DEMO_WALLET });
  assert.deepEqual(p.items, [{ id: "chapter2", code: 37, price: "4.99", token: "USDC", title: "Chapter 2", min_price: null, ownedSwitch: 1 }]);
  assert.equal(p.tipJarMenu, true);
  assert.equal(entry.parameters[PARAM.items], JSON.stringify([JSON.stringify({
    id: "chapter2", code: "37", price: "4.99", token: "USDC", title: "Chapter 2", minPrice: "", ownedSwitch: "1",
  })]), "MZ's encoding of a struct list");
  // boots on the shim as a game with this plugins.js would
  const errors: string[] = [];
  installShim(g, { nwjs: true, options: ["test"], plugins: demoPlugins() });
  const s = bootPlugin({
    log: { error: (...a: unknown[]) => void errors.push(a.join(" ")), warn: () => undefined }, platform: { devNotice: () => undefined },
    // no worker passes and no network
    worker_opts: { schedule: () => null, cancel: () => undefined }, transport: new FakeTransport({}),
  });
  assert.equal(s._disabled, null);
  assert.deepEqual(errors, []);
  assert.equal(s._config!.testnet, true);
  assert.deepEqual([...s._config!.items.keys()], ["chapter2"]);
  await bootShim();
  assert.equal(g.$gameSwitches.value(1), false, "not owned on a fresh install");
  assert.deepEqual(errors, []);
});

test("demoKitCarriesNoRtpAssets", () => {
  const all = readdirSync(DEMO, { recursive: true, encoding: "utf8" }).filter((f) => statSync(join(DEMO, f)).isFile()).sort();
  assert.deepEqual(all, ["README.md", join("data", "Map001.json"), join("data", "MapInfos.json"), join("js", "plugins.js")].sort());
  const map = readJson("Map001.json");
  assert.deepEqual([map.bgm.name, map.bgs.name, map.parallaxName, map.battleback1Name, map.battleback2Name], ["", "", "", "", ""]);
  for (const ev of map.events) {
    if (ev === null) continue;
    for (const page of ev.pages) {
      assert.equal(page.image.characterName, "", `event ${ev.id}: no character sprite (set your own in the editor)`);
      for (const c of page.list) if (c.code === 101) assert.equal(c.parameters[0], "", `event ${ev.id}: no face image`);
    }
  }
  const readme = readFileSync(join(DEMO, "README.md"), "utf8");
  assert.match(readme, /unverified/i, "the README says the kit is unverified against real MZ");
  assert.match(readme, /switch 1/i);
});
