import { readdirSync, readFileSync } from "node:fs";
import { dirname, join, relative } from "node:path";
import { fileURLToPath } from "node:url";

export interface Tier1Case {
  fn: string;
  in: any;
  out?: unknown;
  error?: string;
}

export interface Tier1Topic {
  topic: string;
  cases: Tier1Case[];
}

export function vectorsDir(): string {
  return join(dirname(fileURLToPath(import.meta.url)), "..", "vectors");
}

function readJson(path: string): any {
  return JSON.parse(readFileSync(path, "utf8"));
}

function jsonFiles(dir: string): string[] {
  const out: string[] = [];
  for (const entry of readdirSync(dir, { withFileTypes: true }).sort((x, y) => (x.name < y.name ? -1 : 1))) {
    const p = join(dir, entry.name);
    if (entry.isDirectory()) out.push(...jsonFiles(p));
    else if (entry.name.endsWith(".json")) out.push(p);
  }
  return out;
}

export function loadTier1(): Tier1Topic[] {
  const dir = join(vectorsDir(), "tier1");
  return jsonFiles(dir).map((p) => {
    const v = readJson(p);
    return { topic: v.topic as string, cases: v.cases as Tier1Case[] };
  });
}

/** `path` is relative to the vectors directory, with `/` separators (e.g. `tier2/evm/x.json`). */
export function loadTier2(): { path: string; vector: any }[] {
  const root = vectorsDir();
  return jsonFiles(join(root, "tier2")).map((p) => ({
    path: relative(root, p).split("\\").join("/"),
    vector: readJson(p),
  }));
}
