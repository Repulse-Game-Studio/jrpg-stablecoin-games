/** Base58 (Bitcoin alphabet), as used for Solana addresses and signatures. */
import { ValueError } from "../errors.ts";
import { pyStrip } from "../pystr.ts";

export const ALPHABET = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
const INDEX = new Map<string, bigint>([...ALPHABET].map((c, i) => [c, BigInt(i)]));

export function b58encode(data: Uint8Array): string {
  let n = 0n;
  for (const b of data) n = (n << 8n) | BigInt(b);
  let out = "";
  while (n > 0n) {
    out = ALPHABET[Number(n % 58n)]! + out;
    n /= 58n;
  }
  let zeros = 0;
  while (zeros < data.length && data[zeros] === 0) zeros++;
  return "1".repeat(zeros) + out;
}

export function b58decode(text: string): Uint8Array {
  if (typeof text !== "string" || text === "") throw new ValueError(`not base58: ${JSON.stringify(text)}`);
  let n = 0n;
  for (const c of text) {
    const v = INDEX.get(c);
    if (v === undefined) throw new ValueError(`invalid base58 character ${JSON.stringify(c)}`);
    n = n * 58n + v;
  }
  let zeros = 0;
  while (zeros < text.length && text[zeros] === "1") zeros++;
  const body: number[] = [];
  while (n > 0n) {
    body.push(Number(n & 0xffn));
    n >>= 8n;
  }
  const out = new Uint8Array(zeros + body.length);
  body.reverse();
  out.set(body, zeros);
  return out;
}

/** The longest base58 text of `length` bytes: 44 characters for 32, 88 for 64 (Python's max_encoded_length). */
export function maxEncodedLength(length: number): number {
  return Math.ceil(length * Math.log(256) / Math.log(58));
}

/**
 * `length` bytes from base58 `text`. Decoding is quadratic in the text's length, so text longer than
 * any encoding of `length` bytes is refused before it is decoded.
 */
export function decodeFixed(text: string, length: number): Uint8Array {
  if (typeof text === "string" && text.length > maxEncodedLength(length)) {
    throw new ValueError(`too long for ${length} bytes: ${text.length} characters`);
  }
  const raw = b58decode(text);
  if (raw.length !== length) throw new ValueError(`expected ${length} bytes, got ${raw.length}: ${JSON.stringify(text)}`);
  return raw;
}

export function normalizePubkey(text: unknown): string {
  if (typeof text !== "string") throw new ValueError(`not an address: ${String(text)}`);
  return b58encode(decodeFixed(pyStrip(text), 32));
}

export function normalizeSignature(text: unknown): string {
  if (typeof text !== "string") throw new ValueError(`not a signature: ${String(text)}`);
  return b58encode(decodeFixed(pyStrip(text), 64));
}
