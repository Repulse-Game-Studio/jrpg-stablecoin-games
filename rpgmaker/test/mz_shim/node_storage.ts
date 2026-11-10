/**
 * A StorageBackend on a directory (Node only): NW.js save files land at `<dir>/save/<name>.rmmzsave`
 * and web keys at `<dir>/<key>`. For drivers that restart on the same storage. Not part of MZ.
 */
import { existsSync, mkdirSync, readdirSync, readFileSync, renameSync, rmSync, statSync, writeFileSync } from "node:fs";
import { dirname, isAbsolute, join, normalize } from "node:path";
import type { StorageBackend } from "./context.ts";

/** The suffix of a file being written (hidden from keys()). */
const TEMP = ".dirtmp~";

export class DirStorage implements StorageBackend {
  readonly dir: string;

  constructor(dir: string) {
    this.dir = dir;
  }

  private path(key: string): string {
    if (isAbsolute(key) || normalize(key).startsWith("..")) throw new Error(`storage key outside the directory: ${key}`);
    return join(this.dir, key);
  }

  keys(): string[] {
    if (!existsSync(this.dir)) return [];
    return readdirSync(this.dir, { recursive: true, encoding: "utf8" })
      .filter((f) => !f.endsWith(TEMP) && statSync(join(this.dir, f)).isFile())
      .map((f) => f.split("\\").join("/"))
      .sort();
  }

  has(key: string): boolean {
    return existsSync(this.path(key));
  }

  read(key: string): string | null {
    try {
      return readFileSync(this.path(key), "utf8");
    } catch (e) {
      if ((e as NodeJS.ErrnoException).code === "ENOENT") return null;
      throw e;
    }
  }

  write(key: string, text: string): void {
    const p = this.path(key);
    mkdirSync(dirname(p), { recursive: true });
    writeFileSync(p + TEMP, text); // whole or not at all (MZ's own "<file>_" backup is modelled by the shim)
    renameSync(p + TEMP, p);
  }

  remove(key: string): void {
    rmSync(this.path(key), { force: true });
  }
}
