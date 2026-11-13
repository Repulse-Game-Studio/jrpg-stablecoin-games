/** Program-derived addresses and associated token accounts (classic SPL Token program). */
import { asciiBytes, concatBytes } from "../bytes.ts";
import { ValueError } from "../errors.ts";
import { sha256 } from "../sha256.ts";
import { b58encode, decodeFixed } from "./base58.ts";
import { isOnCurve } from "./ed25519.ts";

export const TOKEN_PROGRAM_ID = "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA";
export const ASSOCIATED_TOKEN_PROGRAM_ID = "ATokenGPvbdGVxr1b2hvZbsiqW5xWH25efTNsLJA8knL";
export const MAX_SEED_LEN = 32;
export const MAX_SEEDS = 16;
const MARKER = asciiBytes("ProgramDerivedAddress");

export function createProgramAddress(seeds: Uint8Array[], programId: Uint8Array): Uint8Array | null {
  if (seeds.length > MAX_SEEDS || seeds.some((s) => s.length > MAX_SEED_LEN)) {
    throw new ValueError("too many seeds or a seed longer than 32 bytes");
  }
  const digest = sha256(concatBytes(...seeds, programId, MARKER));
  return isOnCurve(digest) ? null : digest;
}

export function findProgramAddress(seeds: Uint8Array[], programId: Uint8Array): [Uint8Array, number] {
  for (let bump = 255; bump >= 0; bump--) {
    const address = createProgramAddress([...seeds, new Uint8Array([bump])], programId);
    if (address !== null) return [address, bump];
  }
  throw new ValueError("no viable bump seed");
}

/** The developer's (or player's) token account for `mint`, as wallets create it. */
export function associatedTokenAddress(owner: string, mint: string): string {
  const seeds = [decodeFixed(owner, 32), decodeFixed(TOKEN_PROGRAM_ID, 32), decodeFixed(mint, 32)];
  const [address] = findProgramAddress(seeds, decodeFixed(ASSOCIATED_TOKEN_PROGRAM_ID, 32));
  return b58encode(address);
}
