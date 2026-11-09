/** Hex, base64 and byte helpers (no Python twin: Python uses bytes.fromhex / .hex() / base64). */
import { ValueError } from "./errors.ts";

/** Decode hex (either case, optional `0x`) into bytes. Odd length or a non-hex character is a ValueError. */
export function hexToBytes(hex: string): Uint8Array {
  if (typeof hex !== "string") throw new ValueError(`not a hex string: ${String(hex)}`);
  const body = hex.startsWith("0x") ? hex.slice(2) : hex;
  if (body.length % 2 !== 0) throw new ValueError(`odd-length hex: ${hex}`);
  if (!/^[0-9a-fA-F]*$/.test(body)) throw new ValueError(`non-hex character in: ${hex}`);
  const out = new Uint8Array(body.length / 2);
  for (let i = 0; i < out.length; i++) out[i] = parseInt(body.slice(2 * i, 2 * i + 2), 16);
  return out;
}

/** Lowercase hex without a prefix. */
export function bytesToHex(b: Uint8Array): string {
  let s = "";
  for (let i = 0; i < b.length; i++) s += (b[i]! < 16 ? "0" : "") + b[i]!.toString(16);
  return s;
}

/** Concatenate byte arrays. */
export function concatBytes(...parts: Uint8Array[]): Uint8Array {
  let n = 0;
  for (const p of parts) n += p.length;
  const out = new Uint8Array(n);
  let o = 0;
  for (const p of parts) {
    out.set(p, o);
    o += p.length;
  }
  return out;
}

/** Bytes of a string whose characters are all below U+0100 (ASCII in practice; no TextEncoder in ES2020). */
export function asciiBytes(s: string): Uint8Array {
  const out = new Uint8Array(s.length);
  for (let i = 0; i < s.length; i++) out[i] = s.charCodeAt(i) & 0xff;
  return out;
}

const B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/** Standard base64 with `=` padding (Python `base64.b64encode(b).decode("ascii")`; no btoa/Buffer in ES2020). */
export function base64Encode(b: Uint8Array): string {
  let out = "";
  for (let i = 0; i < b.length; i += 3) {
    const n = (b[i]! << 16) | ((b[i + 1] ?? 0) << 8) | (b[i + 2] ?? 0);
    out += B64[(n >> 18) & 63]! + B64[(n >> 12) & 63]!;
    out += i + 1 < b.length ? B64[(n >> 6) & 63]! : "=";
    out += i + 2 < b.length ? B64[n & 63]! : "=";
  }
  return out;
}
