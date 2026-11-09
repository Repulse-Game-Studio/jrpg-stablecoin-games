/** JSON value equality as the conformance README defines it for `params` and outputs. */

function isObject(v: unknown): v is Record<string, unknown> {
  return typeof v === "object" && v !== null && !Array.isArray(v);
}

/** JSON.stringify that renders bigint as `"<n>n"` instead of throwing. */
export function safeStringify(v: unknown): string {
  return JSON.stringify(v, (_k, x) => (typeof x === "bigint" ? `${x}n` : x));
}

function show(v: unknown): string {
  const s = v === undefined ? "undefined" : safeStringify(v);
  return s.length > 120 ? s.slice(0, 117) + "..." : s;
}

/**
 * Objects ignore key order, arrays are ordered, `typeof` must agree (`true` is not `1`),
 * numbers compare by value (JS has one number type, so `1` equals `1.0`).
 */
export function jsonEqual(a: unknown, b: unknown): boolean {
  return firstDiff(a, b) === "";
}

/** "" when equal, otherwise `<path>: <left> != <right>` for the first difference found. */
export function firstDiff(a: unknown, b: unknown, path = ""): string {
  const here = (detail: string) => (path === "" ? detail : `${path}: ${detail}`);
  if (Array.isArray(a) || Array.isArray(b)) {
    if (!Array.isArray(a) || !Array.isArray(b)) return here(`${show(a)} != ${show(b)}`);
    const n = Math.min(a.length, b.length);
    for (let i = 0; i < n; i++) {
      const d = firstDiff(a[i], b[i], `${path}[${i}]`);
      if (d !== "") return d;
    }
    if (a.length !== b.length) return here(`length ${a.length} != ${b.length}`);
    return "";
  }
  if (isObject(a) || isObject(b)) {
    if (!isObject(a) || !isObject(b)) return here(`${show(a)} != ${show(b)}`);
    const keys = Object.keys(a).sort();
    for (const k of keys) {
      const sub = path === "" ? k : `${path}.${k}`;
      if (!Object.prototype.hasOwnProperty.call(b, k)) return `${sub}: missing on the right`;
      const d = firstDiff(a[k], b[k], sub);
      if (d !== "") return d;
    }
    for (const k of Object.keys(b).sort()) {
      if (!Object.prototype.hasOwnProperty.call(a, k)) {
        return `${path === "" ? k : `${path}.${k}`}: missing on the left`;
      }
    }
    return "";
  }
  if (typeof a !== typeof b || a !== b) return here(`${show(a)} != ${show(b)}`);
  return "";
}
