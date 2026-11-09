/**
 * Exception types raised to the developer (twin of scfg_core/errors.py), plus the Python
 * built-in exceptions the ports raise where Python would (`ValueError`, `KeyError`,
 * `IndexError`). JS's built-in `TypeError` stands for Python `TypeError`.
 */

/** Python `ValueError`. */
export class ValueError extends Error {
  constructor(message?: string) {
    super(message);
    this.name = "ValueError";
  }
}

/** Python `KeyError`: a dict lookup `d[key]` on a missing key. */
export class KeyError extends Error {
  constructor(key: unknown) {
    super(typeof key === "string" ? JSON.stringify(key) : String(key));
    this.name = "KeyError";
  }
}

/** Python `IndexError`: `seq[i]` out of range. */
export class IndexError extends Error {
  constructor(message = "list index out of range") {
    super(message);
    this.name = "IndexError";
  }
}

// Marks every Stablecoin for Games error, so `instanceof ScfgError` also holds for ConfigError,
// which has to extend ValueError (Python: `class ConfigError(ScfgError, ValueError)`).
const SCFG_ERROR: unique symbol = Symbol("ScfgError");

/** True for every Stablecoin for Games error (ScfgError and its subclasses, ConfigError included). */
export function isScfgError(e: unknown): boolean {
  return typeof e === "object" && e !== null && (e as { [SCFG_ERROR]?: unknown })[SCFG_ERROR] === true;
}

/** Base class for all Stablecoin for Games errors. */
export class ScfgError extends Error {
  constructor(message?: string) {
    super(message);
    this.name = new.target.name;
  }

  /** `e instanceof ScfgError` is `isScfgError(e)`; subclasses keep the ordinary prototype check. */
  static [Symbol.hasInstance](v: unknown): boolean {
    if (this === ScfgError) return isScfgError(v);
    return Function.prototype[Symbol.hasInstance].call(this, v);
  }
}
Object.defineProperty(ScfgError.prototype, SCFG_ERROR, { value: true });

/**
 * Invalid configuration or programmer error. Raised immediately so it shows up in testing.
 * `code` is a stable machine-readable identifier of the reason, `<subject>.<problem>`
 * (for example `chains.empty`). Conformance vectors compare it instead of the message.
 *
 * Like Python's `ConfigError(ScfgError, ValueError)` it is both: `instanceof ValueError` through
 * the prototype chain and `instanceof ScfgError` through the marker.
 */
export class ConfigError extends ValueError {
  readonly code: string;

  constructor(message: string, opts: { code: string }) {
    super(message);
    this.name = "ConfigError";
    this.code = opts.code;
  }
}
Object.defineProperty(ConfigError.prototype, SCFG_ERROR, { value: true });
