/**
 * Legacy Solana transactions, serialized without any library.
 *
 * Message layout: header (3 bytes) | compact-u16 #keys | keys (32 bytes each) |
 * recent blockhash (32) | compact-u16 #instructions | per instruction: program index,
 * compact-u16 #accounts, account indexes, compact-u16 #data, data.
 * Account order (as the Solana SDK compiles it): fee payer, then writable signers, readonly
 * signers, writable non-signers, readonly non-signers, each group sorted by raw key bytes.
 * Program ids are readonly non-signers.
 */
import { bytesToHex, concatBytes } from "../bytes.ts";
import { ValueError } from "../errors.ts";
import { decodeFixed } from "./base58.ts";
import { TOKEN_PROGRAM_ID } from "./pda.ts";

export const COMPUTE_BUDGET_PROGRAM_ID = "ComputeBudget111111111111111111111111111111";

export interface AccountMeta {
  pubkey: string;
  is_signer: boolean;
  is_writable: boolean;
}

export interface Instruction {
  program_id: string;
  accounts: readonly AccountMeta[];
  data: Uint8Array;
}

export function compactU16(n: number): Uint8Array {
  if (typeof n !== "number") throw new TypeError(`compact-u16 needs a number, got ${typeof n}`);
  if (!Number.isInteger(n) || n < 0 || n > 0xffff) throw new ValueError(`compact-u16 out of range: ${n}`);
  const out: number[] = [];
  for (;;) {
    const byte = n & 0x7f;
    n >>= 7;
    if (n) {
      out.push(byte | 0x80);
    } else {
      out.push(byte);
      return new Uint8Array(out);
    }
  }
}

/** Unsigned little-endian integer of `size` bytes (Python `int.to_bytes(size, "little")`). */
function uintLE(value: number | bigint, size: number): Uint8Array {
  let v = BigInt(value);
  if (v < 0n || v >= 1n << BigInt(8 * size)) throw new ValueError(`${value} does not fit in ${size} bytes`);
  const out = new Uint8Array(size);
  for (let i = 0; i < size; i++) {
    out[i] = Number(v & 0xffn);
    v >>= 8n;
  }
  return out;
}

export function compileMessage(payer: string, instructions: readonly Instruction[], recentBlockhash: string): Uint8Array {
  // Keyed by lowercase hex of the raw key: string order equals raw byte order.
  const flags = new Map<string, { raw: Uint8Array; signer: boolean; writable: boolean }>();
  const add = (key: string, signer: boolean, writable: boolean): void => {
    const raw = decodeFixed(key, 32);
    const id = bytesToHex(raw);
    const f = flags.get(id) ?? { raw, signer: false, writable: false };
    f.signer = f.signer || signer;
    f.writable = f.writable || writable;
    flags.set(id, f);
  };
  for (const ix of instructions) {
    for (const meta of ix.accounts) add(meta.pubkey, meta.is_signer, meta.is_writable);
    add(ix.program_id, false, false);
  }
  const payerRaw = decodeFixed(payer, 32);
  flags.delete(bytesToHex(payerRaw));

  const group = (signer: boolean, writable: boolean): Uint8Array[] =>
    [...flags.entries()]
      .filter(([, f]) => f.signer === signer && f.writable === writable)
      .sort(([a], [b]) => (a < b ? -1 : a > b ? 1 : 0))
      .map(([, f]) => f.raw);
  const ws = group(true, true);
  const rs = group(true, false);
  const wu = group(false, true);
  const ru = group(false, false);
  const keys = [payerRaw, ...ws, ...rs, ...wu, ...ru];
  const index = new Map(keys.map((k, i) => [bytesToHex(k), i]));
  const idx = (key: string): number => index.get(bytesToHex(decodeFixed(key, 32)))!;

  const parts: Uint8Array[] = [
    new Uint8Array([1 + ws.length + rs.length, rs.length, ru.length]),
    compactU16(keys.length),
    ...keys,
    decodeFixed(recentBlockhash, 32),
    compactU16(instructions.length),
  ];
  for (const ix of instructions) {
    parts.push(
      new Uint8Array([idx(ix.program_id)]),
      compactU16(ix.accounts.length),
      new Uint8Array(ix.accounts.map((m) => idx(m.pubkey))),
      compactU16(ix.data.length),
      ix.data,
    );
  }
  return concatBytes(...parts);
}

/** Empty signature slots + message, ready for a wallet to sign. */
export function unsignedTransaction(message: Uint8Array): Uint8Array {
  if (message.length === 0) throw new ValueError("empty message");
  const signers = message[0]!;
  return concatBytes(compactU16(signers), new Uint8Array(64 * signers), message);
}

export function setComputeUnitLimit(units: number | bigint): Instruction {
  return { program_id: COMPUTE_BUDGET_PROGRAM_ID, accounts: [], data: concatBytes(new Uint8Array([2]), uintLE(units, 4)) };
}

export function setComputeUnitPrice(microLamports: number | bigint): Instruction {
  return { program_id: COMPUTE_BUDGET_PROGRAM_ID, accounts: [], data: concatBytes(new Uint8Array([3]), uintLE(microLamports, 8)) };
}

/**
 * SPL Token TransferChecked; `extra` keys (e.g. a Solana Pay reference) are appended read-only
 * so the payment can be found with getSignaturesForAddress(extra key).
 */
export function transferChecked(args: {
  source: string;
  mint: string;
  destination: string;
  owner: string;
  amount: number | bigint;
  decimals: number;
  extra?: readonly string[];
}): Instruction {
  const { source, mint, destination, owner, amount, decimals, extra = [] } = args;
  const accounts: AccountMeta[] = [
    { pubkey: source, is_signer: false, is_writable: true },
    { pubkey: mint, is_signer: false, is_writable: false },
    { pubkey: destination, is_signer: false, is_writable: true },
    { pubkey: owner, is_signer: true, is_writable: false },
    ...extra.map((k) => ({ pubkey: k, is_signer: false, is_writable: false })),
  ];
  const data = concatBytes(new Uint8Array([12]), uintLE(amount, 8), uintLE(decimals, 1));
  return { program_id: TOKEN_PROGRAM_ID, accounts, data };
}
