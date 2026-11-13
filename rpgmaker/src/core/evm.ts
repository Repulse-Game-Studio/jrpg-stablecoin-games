/** EVM helpers: addresses (EIP-55) and event topics. */
import type { Units } from "./amounts.ts";
import { asciiBytes, bytesToHex } from "./bytes.ts";
import { ValueError } from "./errors.ts";
import { keccak256 } from "./keccak.ts";

// The whole string (Python's fullmatch): a trailing newline is not part of an address.
const HEX40 = /^0x[0-9a-fA-F]{40}$/;
// One 32-byte ABI word, as a Transfer log carries its uint256 value.
const WORD = /^0x[0-9a-fA-F]{64}$/;

export const TRANSFER_TOPIC: string =
  "0x" + bytesToHex(keccak256(asciiBytes("Transfer(address,address,uint256)")));

/** The EIP-55 mixed-case form of a 20-byte hex address (any input case). */
export function toChecksumAddress(address: string): string {
  if (typeof address !== "string" || !HEX40.test(address)) {
    throw new ValueError(`not a 20-byte hex address: ${JSON.stringify(address)}`);
  }
  const lower = address.slice(2).toLowerCase();
  const digest = bytesToHex(keccak256(asciiBytes(lower)));
  let out = "0x";
  for (let i = 0; i < lower.length; i++) {
    out += parseInt(digest[i]!, 16) >= 8 ? lower[i]!.toUpperCase() : lower[i]!;
  }
  return out;
}

/** True only if the address is already exactly in EIP-55 form. Never throws. */
export function isChecksumAddress(address: string): boolean {
  try {
    return toChecksumAddress(address) === address;
  } catch (e) {
    if (e instanceof ValueError) return false;
    throw e;
  }
}

/**
 * Validate a user-supplied address and return its checksummed form. Single-case input
 * carries no checksum, so it is accepted; mixed-case input must carry a correct one.
 */
export function normalizeAddress(address: string): string {
  const checksummed = toChecksumAddress(address);
  const body = address.slice(2);
  if (body === body.toLowerCase() || body === body.toUpperCase()) return checksummed;
  if (checksummed !== address) throw new ValueError(`bad EIP-55 checksum: ${address}`);
  return checksummed;
}

/** The address left-padded to 32 bytes, as it appears in indexed event topics. */
export function addressTopic(address: string): string {
  return "0x" + "0".repeat(24) + toChecksumAddress(address).slice(2).toLowerCase();
}

/**
 * One ERC-20 Transfer event as logged, normalized (Python's TransferLog from parse_transfer_log).
 * `value` is the raw uint256 in the logging contract's decimals: equal to token units for a token's
 * own contract, but 18-decimal for Arc's system emitter. Field order follows the Python dataclass.
 */
export interface RawTransferLog {
  token: string; // checksummed contract address
  sender: string; // checksummed
  recipient: string; // checksummed
  value: bigint; // raw logged value
  tx_hash: string; // lowercase
  log_index: number;
  block_number: number;
  block_hash: string; // lowercase
}

/** A TransferLog whose value is known to be token base units (Chain.parse_transfer's result). */
export interface TransferLog extends Omit<RawTransferLog, "value"> {
  value: Units; // base units
}

function topicToAddress(topic: string): string {
  return toChecksumAddress("0x" + topic.slice(-40));
}

/** Python `int(text, 16)` for the forms logs use: optional 0x/0X prefix, then hex digits. */
function parseHex(text: unknown): bigint {
  if (typeof text !== "string") throw new TypeError(`hex integer must be a string, got ${typeof text}`);
  const m = /^(?:0[xX])?([0-9a-fA-F]+)$/.exec(text);
  if (m === null) throw new ValueError(`invalid hex integer: ${JSON.stringify(text)}`);
  return BigInt("0x" + m[1]!);
}

function lowerString(v: unknown, name: string): string {
  if (typeof v !== "string") throw new TypeError(`log ${name} must be a string`);
  return v.toLowerCase();
}

function safeNumber(v: bigint, name: string): number {
  if (v > BigInt(Number.MAX_SAFE_INTEGER)) throw new ValueError(`log ${name} exceeds 2^53: ${v}`);
  return Number(v);
}

/**
 * Parse a raw JSON-RPC log into a TransferLog, or null if it isn't an ERC-20 Transfer.
 * ERC-721 Transfer events share topic0 but have 4 topics, so exactly 3 topics are required.
 */
export function parseTransferLog(log: Record<string, any>): RawTransferLog | null {
  if (typeof log !== "object" || log === null) throw new TypeError("log must be an object");
  const topics: string[] = log["topics"] || [];
  if (log["removed"]) return null;
  if (topics.length !== 3 || topics[0]!.toLowerCase() !== TRANSFER_TOPIC) return null;
  const data: unknown = log["data"];
  if (typeof data !== "string" || !WORD.test(data)) return null; // not exactly the 32-byte value: not an ERC-20 Transfer
  return {
    token: toChecksumAddress(log["address"]),
    sender: topicToAddress(topics[1]!),
    recipient: topicToAddress(topics[2]!),
    value: BigInt(data),
    tx_hash: lowerString(log["transactionHash"], "transactionHash"),
    log_index: safeNumber(parseHex(log["logIndex"]), "logIndex"),
    block_number: safeNumber(parseHex(log["blockNumber"]), "blockNumber"),
    block_hash: lowerString(log["blockHash"], "blockHash"),
  };
}
