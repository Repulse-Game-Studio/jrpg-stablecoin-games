/**
 * `copy.deepcopy` for the Engine's JSON-like state (pending sessions, receipts). No Python twin
 * module: Python uses the standard library. `structuredClone` is not ES2020, so this walks plain
 * objects and arrays; primitives (bigint included) are returned as they are. Anything else
 * (class instances, functions, Maps, Sets, Dates) is a TypeError: Engine state is plain data.
 */
export function deepCopy<T>(value: T): T {
  return walk(value) as T;
}

function walk(v: unknown): unknown {
  if (v === null || typeof v !== "object") {
    if (typeof v === "function" || typeof v === "symbol") throw new TypeError(`cannot copy a ${typeof v}`);
    return v;
  }
  if (Array.isArray(v)) return v.map(walk);
  const proto = Object.getPrototypeOf(v);
  if (proto !== Object.prototype && proto !== null) {
    throw new TypeError(`cannot copy a ${(v as object).constructor?.name ?? "non-plain object"}`);
  }
  const out: Record<string, unknown> = {};
  for (const key of Object.keys(v)) {
    // defineProperty: a key such as "__proto__" stays an own data property.
    Object.defineProperty(out, key, {
      value: walk((v as Record<string, unknown>)[key]), enumerable: true, writable: true, configurable: true,
    });
  }
  return out;
}
