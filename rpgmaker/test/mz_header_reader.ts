/**
 * Reads an RPG Maker MZ plugin header back, the way MZ's Plugin Manager reads the comment blocks
 * at the top of a plugin file (as documented for plugin authors; not MZ's code): the `/*:` block
 * (`@target`, `@plugindesc`, `@author`, `@url`, `@help`, `@param`, `@command`, `@arg` and their
 * `@text`, `@desc`, `@type`, `@default`, `@min`, `@max`, `@dir`, `@require`, `@option`, `@value`,
 * `@on`, `@off`, `@parent`) and every `/*~struct~Name:` block. `@help` takes every line up to the
 * next annotation. Also MZ's encoding of a value of a declared type (`mzEncode`), as the editor
 * stores it in js/plugins.js and `PluginManager.parameters` hands it over.
 */

export interface HeaderOption {
  option: string;
  value: string;
}

export interface HeaderParam {
  name: string;
  text?: string;
  desc?: string;
  type?: string;
  default?: string;
  min?: string;
  max?: string;
  dir?: string;
  require?: string;
  parent?: string;
  on?: string;
  off?: string;
  options: HeaderOption[];
}

export interface HeaderCommand {
  name: string;
  text?: string;
  desc?: string;
  args: HeaderParam[];
}

export interface Header {
  meta: Record<string, string>;
  help: string;
  params: HeaderParam[];
  commands: HeaderCommand[];
  structs: Record<string, HeaderParam[]>;
}

const ATTRS = ["text", "desc", "type", "default", "min", "max", "dir", "require", "parent", "on", "off"] as const;

/** The comment blocks at the top of `source`: [tag after the opening ("" for "/*:", "~struct~Item"), lines]. */
function blocks(source: string): [string, string[]][] {
  const out: [string, string[]][] = [];
  for (const m of source.matchAll(/\/\*(:|~struct~[A-Za-z0-9_]+:)\n([\s\S]*?)\*\//g)) {
    const lines = m[2]!.split("\n").map((l) => l.replace(/^\s*\* ?/, "").replace(/\s+$/, ""));
    out.push([m[1] === ":" ? "" : m[1]!.slice(0, -1), lines]);
  }
  return out;
}

function readParams(lines: string[], into: { params: HeaderParam[]; commands?: HeaderCommand[]; meta?: Record<string, string> }): string {
  let help: string[] | null = null;
  const helpLines: string[] = [];
  let current: HeaderParam | HeaderCommand | null = null;
  let command: HeaderCommand | null = null;
  for (const line of lines) {
    const m = /^@(\w+)\s?(.*)$/.exec(line);
    if (m === null) {
      if (help !== null) help.push(line);
      continue;
    }
    help = null;
    const [, tag, rest] = m as unknown as [string, string, string];
    if (tag === "help") {
      help = helpLines;
      if (rest !== "") help.push(rest);
    } else if (tag === "param") {
      current = { name: rest, options: [] };
      command = null;
      into.params.push(current);
    } else if (tag === "command") {
      if (!into.commands) throw new Error(`@command outside the main block: ${rest}`);
      command = { name: rest, args: [] };
      current = command;
      into.commands.push(command);
    } else if (tag === "arg") {
      if (command === null) throw new Error(`@arg ${rest} outside a @command`);
      current = { name: rest, options: [] };
      command.args.push(current);
    } else if (tag === "option") {
      if (current === null || !("options" in current)) throw new Error(`@option ${rest} outside a @param/@arg`);
      current.options.push({ option: rest, value: rest });
    } else if (tag === "value") {
      const opts = current !== null && "options" in current ? current.options : [];
      if (opts.length === 0) throw new Error(`@value ${rest} without an @option`);
      opts[opts.length - 1]!.value = rest;
    } else if ((ATTRS as readonly string[]).includes(tag)) {
      if (current === null) {
        if (!into.meta) throw new Error(`@${tag} before any @param`);
        into.meta[tag] = rest;
      } else {
        if (tag in current && tag !== "default") throw new Error(`@${tag} given twice for ${current.name}`);
        (current as any)[tag] = rest;
      }
    } else if (current === null && into.meta) {
      into.meta[tag] = rest;
    } else {
      throw new Error(`unknown annotation @${tag} (${current?.name})`);
    }
  }
  return helpLines.join("\n");
}

export function readHeader(source: string): Header {
  const header: Header = { meta: {}, help: "", params: [], commands: [], structs: {} };
  const found = blocks(source);
  const main = found.filter(([tag]) => tag === "");
  if (main.length !== 1) throw new Error(`expected one /*: block, found ${main.length}`);
  header.help = readParams(main[0]![1], header);
  for (const [tag, lines] of found) {
    if (tag === "") continue;
    const name = tag.slice("~struct~".length);
    const params: HeaderParam[] = [];
    readParams(lines, { params });
    header.structs[name] = params;
  }
  return header;
}

/** The struct name of a `struct<Name>` or `struct<Name>[]` type, else null. */
export function structOf(type: string | undefined): string | null {
  const m = /^struct<(\w+)>(\[\])?$/.exec(type ?? "");
  return m ? m[1]! : null;
}

/**
 * A value as MZ's editor stores a parameter of `type` (every value a string): text types as
 * written, `note` as JSON text of the string, `struct<X>` as JSON text of an object whose fields
 * are encoded by their own types, `X[]` as JSON text of a list of encoded entries.
 */
export function mzEncode(header: Header, type: string | undefined, value: unknown): string {
  const t = type ?? "string";
  if (t.endsWith("[]")) {
    if (!Array.isArray(value)) throw new Error(`${t} takes a list`);
    return JSON.stringify(value.map((v) => mzEncode(header, t.slice(0, -2), v)));
  }
  const struct = structOf(t);
  if (struct !== null) {
    const fields = header.structs[struct];
    if (!fields) throw new Error(`no struct ${struct}`);
    const obj = value as Record<string, unknown>;
    const out: Record<string, string> = {};
    for (const f of fields) out[f.name] = f.name in obj ? mzEncode(header, f.type, obj[f.name]) : f.default ?? "";
    for (const k of Object.keys(obj)) if (!fields.some((f) => f.name === k)) throw new Error(`struct ${struct} has no ${k}`);
    return JSON.stringify(out);
  }
  if (t === "note") return JSON.stringify(String(value));
  return String(value);
}
