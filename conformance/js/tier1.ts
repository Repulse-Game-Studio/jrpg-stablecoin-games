/**
 * Tier 1 runner: replays conformance/vectors/tier1/*.json against the TS core.
 * `FUNCS` maps the Python function name of a case to an adapter (JSON-friendly input to
 * JSON-friendly output); each later task adds its topics to FUNCS and IMPLEMENTED.
 */
import { AssertionError } from "node:assert";
import {
  asUnits, decimalToUnits, decode, encode, formatUnits, parseAmount, unitsToDecimal, type Units,
} from "../../rpgmaker/src/core/amounts.ts";
import { normalizeAnyTx } from "../../rpgmaker/src/core/backends/index.ts";
import { bytesToHex, hexToBytes } from "../../rpgmaker/src/core/bytes.ts";
import { getChains } from "../../rpgmaker/src/core/chains.ts";
import { addItem, buildChain, buildConfig, type Config } from "../../rpgmaker/src/core/config.ts";
import { preparePayment } from "../../rpgmaker/src/core/payments.ts";
import { Dec } from "../../rpgmaker/src/core/decimal.ts";
import { ConfigError, ValueError } from "../../rpgmaker/src/core/errors.ts";
import {
  addressTopic, isChecksumAddress, normalizeAddress, parseTransferLog, toChecksumAddress,
} from "../../rpgmaker/src/core/evm.ts";
import { keccak256 } from "../../rpgmaker/src/core/keccak.ts";
import type { Item } from "../../rpgmaker/src/core/models.ts";
import { parseBuildMessage, parseTxMessage } from "../../rpgmaker/src/core/page_messages.ts";
import { eip681TransferUri, qrMatrix, qrRects } from "../../rpgmaker/src/core/paylinks.ts";
import { pyInt } from "../../rpgmaker/src/core/pystr.ts";
import {
  addReceipt, computeOwnership, makeReceipt, receiptKey, removeReceipt,
} from "../../rpgmaker/src/core/receipts.ts";
import { normalizeTxHash } from "../../rpgmaker/src/core/sessions.ts";
import {
  b58decode, b58encode, decodeFixed, normalizePubkey, normalizeSignature,
} from "../../rpgmaker/src/core/solana/base58.ts";
import { isOnCurve } from "../../rpgmaker/src/core/solana/ed25519.ts";
import {
  associatedTokenAddress, createProgramAddress, findProgramAddress,
} from "../../rpgmaker/src/core/solana/pda.ts";
import {
  compactU16, compileMessage, unsignedTransaction, type Instruction,
} from "../../rpgmaker/src/core/solana/tx.ts";
import type { Transfer } from "../../rpgmaker/src/core/transfers.ts";
import { solanaPayUri } from "../../rpgmaker/src/core/solana/paylinks.ts";
import { firstDiff, safeStringify } from "./json.ts";
import type { Tier1Case } from "./vectors.ts";

export const FUNCS: Record<string, (input: any) => unknown> = {};

/** Shape check for vector inputs, so a malformed input is a ValueError, not an incidental TypeError. */
function field<T>(i: any, key: string, type: "string" | "number" | "boolean" | "object"): T {
  const v = i?.[key];
  if (typeof v !== type || v === null) throw new ValueError(`vector input ${key} must be a ${type}`);
  return v as T;
}

function finiteDec(text: string): Dec {
  const d = Dec.parse(text);
  if (!(d instanceof Dec)) throw new ValueError("not a finite decimal");
  return d;
}

function seeds(i: any): Uint8Array[] {
  return field<string[]>(i, "seeds_hex", "object").map((s) => hexToBytes(s));
}

// keccak
FUNCS["keccak256"] = (i) => bytesToHex(keccak256(hexToBytes(field(i, "data_hex", "string"))));

// evm_address (the address fields are passed through untouched: non-strings are Python's TypeError/ValueError cases)
FUNCS["to_checksum_address"] = (i) => toChecksumAddress(i.address);
FUNCS["is_checksum_address"] = (i) => isChecksumAddress(i.address);
FUNCS["normalize_address"] = (i) => normalizeAddress(i.address);
FUNCS["address_topic"] = (i) => addressTopic(i.address);
FUNCS["parse_transfer_log"] = (i) => {
  const t = parseTransferLog(field(i, "log", "object"));
  if (t === null) return null;
  return {
    token: t.token, sender: t.sender, recipient: t.recipient, value: t.value.toString(),
    tx_hash: t.tx_hash, log_index: t.log_index, block_number: t.block_number,
    block_hash: t.block_hash,
  };
};

// base58 (text fields are passed through: b58decode and friends reject non-strings themselves)
FUNCS["b58encode"] = (i) => b58encode(hexToBytes(field(i, "data_hex", "string")));
FUNCS["b58decode"] = (i) => bytesToHex(b58decode(i.text));
FUNCS["decode_fixed"] = (i) => bytesToHex(decodeFixed(i.text, field(i, "length", "number")));
FUNCS["normalize_pubkey"] = (i) => normalizePubkey(i.text);
FUNCS["normalize_signature"] = (i) => normalizeSignature(i.text);

// ed25519
FUNCS["is_on_curve"] = (i) => isOnCurve(hexToBytes(field(i, "point_hex", "string")));

// pda
FUNCS["create_program_address"] = (i) => {
  const out = createProgramAddress(seeds(i), decodeFixed(field(i, "program_id", "string"), 32));
  return out === null ? null : b58encode(out);
};
FUNCS["find_program_address"] = (i) => {
  const [address, bump] = findProgramAddress(seeds(i), decodeFixed(field(i, "program_id", "string"), 32));
  return { address: b58encode(address), bump };
};
FUNCS["associated_token_address"] = (i) => associatedTokenAddress(i.owner, i.mint);

// solana_tx
FUNCS["compact_u16"] = (i) => bytesToHex(compactU16(i.n));
FUNCS["compile_message"] = (i) => {
  const ixs: Instruction[] = field<any[]>(i, "instructions", "object").map((x) => ({
    program_id: field<string>(x, "program_id", "string"),
    accounts: field<any[]>(x, "accounts", "object").map((m) => ({
      pubkey: field<string>(m, "pubkey", "string"),
      is_signer: field<boolean>(m, "is_signer", "boolean"),
      is_writable: field<boolean>(m, "is_writable", "boolean"),
    })),
    data: hexToBytes(field(x, "data_hex", "string")),
  }));
  return bytesToHex(compileMessage(field(i, "payer", "string"), ixs, field(i, "recent_blockhash", "string")));
};
FUNCS["unsigned_transaction"] = (i) => bytesToHex(unsignedTransaction(hexToBytes(field(i, "message_hex", "string"))));

// amounts (units arrive as decimal integer strings, Python `int(str)`)
function unitsIn(v: unknown): Units {
  if (typeof v !== "string") throw new TypeError("vector units must be a decimal string");
  return asUnits(pyInt(v));
}
FUNCS["parse_amount"] = (i) => {
  const value = i.type === "float" ? 4.99 : i.type === "bool" ? Boolean(i.value) : i.value;
  return String(parseAmount(value));
};
FUNCS["encode"] = (i) => String(encode(i.price, i.code, i.nonce));
FUNCS["decode"] = (i) => {
  const [price, code, nonce] = decode(unitsIn(i.units));
  return { price: String(price), code, nonce };
};
FUNCS["units_to_decimal"] = (i) => String(unitsToDecimal(unitsIn(i.units)));
FUNCS["decimal_to_units"] = (i) => {
  const d = Dec.parse(field<string>(i, "amount", "string"));
  if (!(d instanceof Dec)) throw new ValueError("amount must be finite");
  return String(decimalToUnits(d));
};
FUNCS["format_units"] = (i) => formatUnits(unitsIn(i.units));

// paylinks and qr
FUNCS["eip681_transfer_uri"] = (i) => eip681TransferUri({
  token_address: field(i, "token_address", "string"), chain_id: field(i, "chain_id", "number"),
  recipient: field(i, "recipient", "string"), units: unitsIn(i.units),
});
FUNCS["solana_pay_uri"] = (i) => solanaPayUri({
  recipient: field(i, "recipient", "string"), units: unitsIn(i.units), mint: field(i, "mint", "string"),
  reference: field(i, "reference", "string"), label: field(i, "label", "string"), message: field(i, "message", "string"),
});
FUNCS["qr_matrix"] = (i) => qrMatrix(field(i, "text", "string")).map((row) => row.map((m) => (m ? "1" : "0")).join(""));
FUNCS["qr_rects"] = (i) => {
  const [size, rects] = qrRects(field(i, "text", "string"), field(i, "border", "number"));
  return { size, rects };
};

// page_messages
FUNCS["parse_tx_message"] = (i) => parseTxMessage(i.data);
FUNCS["parse_build_message"] = (i) => parseBuildMessage(i.data);

// receipts
FUNCS["receipt_key"] = (i) => receiptKey(
  field(i, "chain", "string"), field(i, "tx_hash", "string"), field(i, "log_index", "number"));
FUNCS["make_receipt"] = (i) => {
  const t = field<any>(i, "transfer", "object");
  if (t.class !== "Transfer" && t.class !== "TransferLog") throw new ValueError(`unknown transfer class ${String(t.class)}`);
  const transfer: Transfer = {
    token: t.token, sender: t.sender, recipient: t.recipient, value: unitsIn(t.value),
    tx_hash: t.tx_hash, log_index: t.log_index, block_number: t.block_number,
  };
  return makeReceipt({ chain: i.chain, transfer, kind: i.kind, item: i.item, token: i.token, source: i.source });
};
FUNCS["add_receipt"] = (i) => {
  const store = { ...field<any>(i, "store", "object") };
  const added = addReceipt(store, i.receipt);
  return { added, store };
};
FUNCS["remove_receipt"] = (i) => {
  const store = { ...field<any>(i, "store", "object") };
  removeReceipt(store, i.key);
  return store;
};
FUNCS["compute_ownership"] = (i) => {
  const items: Record<string, Item> = {};
  for (const x of field<any[]>(i, "items", "object")) {
    items[x.item_id] = {
      item_id: x.item_id, code: x.code, price: finiteDec(x.price), token: x.token, title: x.title,
      min_price: finiteDec(x.min_price),
    };
  }
  const [owned, tips] = computeOwnership(field(i, "store", "object"), items);
  return { owned: [...owned].sort(), tip_total: String(tips) };
};

// tx_ids (normalize_signature is registered with base58)
FUNCS["normalize_tx_hash"] = (i) => normalizeTxHash(i.text);
FUNCS["normalize_any_tx"] = (i) => normalizeAnyTx(i.text);

// chains, config, custom_chains, items, payments.
// Inputs follow conformance/python/scfg_conformance/tier1_config.py: a Config is the kwargs of
// build_config (a custom chain inside it the kwargs of build_chain, a non-object entry is passed
// through untouched), items are lists of add_item kwargs, and `{"float": "<repr>"}` is a float.

/** Every dataclass field is emitted (like Python's `to_jsonable`); a `Dec` becomes its `str`. */
function toJsonable(v: unknown, path = ""): unknown {
  if (v === null || typeof v === "string" || typeof v === "boolean") return v;
  if (typeof v === "number") {
    if (!Number.isSafeInteger(v)) throw new ValueError(`non-integral or unsafe number at ${path || "<root>"}`);
    return v;
  }
  if (typeof v === "bigint") throw new ValueError(`bigint at ${path || "<root>"}: write it as a string`);
  if (v instanceof Dec) return String(v);
  if (Array.isArray(v)) return v.map((x, n) => toJsonable(x, `${path}[${n}]`));
  if (typeof v === "object" && v !== undefined) {
    const out: Record<string, unknown> = {};
    for (const [k, x] of Object.entries(v)) {
      if (x === undefined) throw new ValueError(`undefined at ${path ? path + "." : ""}${k}`);
      Object.defineProperty(out, k, { value: toJsonable(x, path ? `${path}.${k}` : k), enumerable: true, writable: true, configurable: true });
    }
    return out;
  }
  throw new ValueError(`unsupported value at ${path || "<root>"}`);
}

/** `{"float": "4.99"}` -> 4.99. A float that is integral in Python (`5.0`) is not expressible in JS: refuse loudly. */
function vectorValue(v: any): unknown {
  if (typeof v === "object" && v !== null && !Array.isArray(v) && Object.keys(v).length === 1 && "float" in v) {
    const n = Number(v.float);
    if (Number.isInteger(n)) throw new AssertionError({ message: `integral float ${v.float} cannot be expressed in JS: skip this case explicitly` });
    return n;
  }
  return v;
}

function chainOrRaw(entry: any): unknown {
  if (typeof entry === "object" && entry !== null && !Array.isArray(entry)) {
    const { key, ...rest } = entry;
    return buildChain(key, rest);
  }
  return entry;
}

function configFrom(kwargs: any): Config {
  const kw = { ...field<any>({ kwargs }, "kwargs", "object") };
  if ("custom_chains" in kw) kw.custom_chains = field<any[]>(kw, "custom_chains", "object").map(chainOrRaw);
  return buildConfig(kw);
}

function itemKwargs(d: any): { item_id: any; rest: any } {
  const { item_id, ...rest } = d;
  for (const k of Object.keys(rest)) rest[k] = vectorValue(rest[k]);
  return { item_id, rest };
}

function withItems(configKw: any, items: any[]): Config {
  const config = configFrom(configKw);
  for (const item of items) {
    const { item_id, rest } = itemKwargs(item);
    addItem(config, item_id, rest);
  }
  return config;
}

FUNCS["get_chains"] = (i) => toJsonable(getChains(i.testnet));
FUNCS["build_chain"] = (i) => {
  const { key, ...rest } = i;
  return toJsonable(buildChain(key, rest));
};
FUNCS["build_config"] = (i) => toJsonable(configFrom(i));
FUNCS["add_item"] = (i) => {
  const config = withItems(i.config, i.existing ?? []);
  const { item_id, rest } = itemKwargs(i.item);
  return toJsonable(addItem(config, item_id, rest));
};
FUNCS["prepare_payment"] = (i) => {
  const config = withItems(i.config, i.items ?? []);
  const kw: Record<string, unknown> = {};
  for (const k of ["item", "amount", "token", "title"]) if (k in i) kw[k] = vectorValue(i[k]);
  return toJsonable(preparePayment(config, i.kind, kw));
};

/** Topics (vector file names without `.json`) whose adapters are all in FUNCS. */
export const IMPLEMENTED: Set<string> = new Set([
  "keccak", "evm_address", "base58", "ed25519", "pda", "solana_tx",
  "amounts", "paylinks", "qr", "page_messages", "receipts", "tx_ids",
  "chains", "config", "custom_chains", "items", "payments",
]);

type Outcome = { out: unknown } | { error: string };

function compute(fn: string, input: unknown): Outcome {
  const f = FUNCS[fn];
  if (f === undefined) throw new AssertionError({ message: `no JS adapter for ${fn}` });
  try {
    return { out: f(input) };
  } catch (e) {
    if (e instanceof ConfigError) return { error: e.code };
    if (e instanceof ValueError || e instanceof TypeError) return { error: "invalid" };
    throw e; // Any other throw fails the case (stricter than Python on purpose).
  }
}

/** Recompute a case through FUNCS and throw an AssertionError (naming the first difference) on mismatch. */
export function runCase(c: Tier1Case): void {
  const got = compute(c.fn, c.in);
  const want: Outcome = "error" in c && c.error !== undefined ? { error: c.error } : { out: c.out };
  const diff = firstDiff(got, want);
  if (diff !== "") {
    throw new AssertionError({
      message: `${c.fn}(${safeStringify(c.in)}): ${diff} (got ${safeStringify(got)}, vector says ${safeStringify(want)})`,
    });
  }
}
