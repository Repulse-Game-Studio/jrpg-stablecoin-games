/**
 * Twin of tests/unit/test_solana_backend.py, test_solana_watch.py and the backend half of
 * test_solana_build.py (the checkout controller and server tests belong to the glue, src/mz).
 */
import { test } from "node:test";
import assert from "node:assert/strict";
import { FakeChain } from "../../../test/chain_fakes.ts";
import { FakeSolanaChain, pubkey, sig } from "../../../test/solana_fakes.ts";
import { asUnits } from "../amounts.ts";
import { MAINNET, makeToken, type Chain } from "../chains.ts";
import type { Transport } from "../coio.ts";
import { addItem, buildConfig } from "../config.ts";
import { Dec } from "../decimal.ts";
import { InvalidOperation, ValueError } from "../errors.ts";
import type { Item } from "../models.ts";
import { makeReceipt, receiptKey } from "../receipts.ts";
import { RestoreError, lookup } from "../restore.ts";
import { RpcClient, RpcError, RpcUnavailable } from "../rpc.ts";
import { HINT_TIMEOUT_S, SCAN_HORIZON_S, createSession, setHint, type Entropy, type SessionStore } from "../sessions.ts";
import { b58decode, b58encode, normalizeSignature } from "../solana/base58.ts";
import { associatedTokenAddress } from "../solana/pda.ts";
import { SolanaRpc } from "../solana/rpc.ts";
import { COMPUTE_BUDGET_PROGRAM_ID } from "../solana/tx.ts";
import type { Transfer } from "../transfers.ts";
import { backendForFamily } from "./index.ts";
import {
  COMPUTE_UNITS, FINAL_CONFIRMATIONS, MAX_PRIORITY_FEE, MAX_SIG_PAGES, MIN_PRIORITY_FEE, SCAN_TX_BUDGET, SIG_PAGE,
  SOLANA_HINT_TIMEOUT_S, START_SLOT_MARGIN, SolanaBackend, transferTo,
} from "./solana.ts";

const DEV = "7xKXtg2CW87d97TXJSDpbD5jBkheTqA83TZRuJosgAsU";
const OLD = "4SELZ24sTinbBYo27VvsUAHepTRXGVBghc8WTUpvx6Pn";
const PAYER = "AUn431TLHb8hD7r95QKArw2ezfSR5YBZipFo6vu1iZpe";
const B = new SolanaBackend();
const MAIN = MAINNET["solana"]!;
const MAIN_MINT = MAIN.tokens["USDC"]!.address;

const transport: Transport = { postJson: async () => ({}) };

function counted(calls: string[], name: string): number {
  return calls.filter((c) => c === name).length;
}

/** Entropy whose token_bytes are 32 counting bytes, different on every draw. */
function countingEntropy(): Entropy {
  let n = 0;
  return {
    tokenHex: () => { throw new Error("unexpected"); },
    randBelow: () => { throw new Error("unexpected"); },
    tokenBytes: (k) => { n += 1; return new Uint8Array(k).fill(n); },
  };
}

test("test_constants", () => {
  assert.deepEqual(
    [FINAL_CONFIRMATIONS, SCAN_TX_BUDGET, SIG_PAGE, MAX_SIG_PAGES, START_SLOT_MARGIN, COMPUTE_UNITS, MIN_PRIORITY_FEE,
      MAX_PRIORITY_FEE, SOLANA_HINT_TIMEOUT_S],
    [32, 5, 100, 10, 150, 30_000, 1_000, 1_000_000, 180],
  );
});

// --- test_solana_backend.py -----------------------------------------------------------------

function cfg() {
  const c = buildConfig({ solana_wallet: DEV, previous_wallets: [OLD] });
  addItem(c, "chapter2", { code: 37, price: "4.99", token: "USDC", title: "Chapter 2" });
  return c;
}

test("test_make_rpc_wraps_the_failover_client", () => {
  const clock = () => 3;
  const rpc = B.makeRpc(MAIN, transport, clock);
  assert.ok(rpc instanceof SolanaRpc && rpc.client instanceof RpcClient);
  assert.deepEqual([...rpc.client.urls], MAIN.rpc);
  assert.equal(rpc.client.clock, clock);
});

test("test_start_fields", async () => {
  const chain = new FakeSolanaChain(1000);
  const dest = associatedTokenAddress(DEV, MAIN_MINT);
  chain.addresses[dest] = [{ signature: sig(9), slot: 999, err: null }];
  const entropy = countingEntropy();
  const f = await B.start({ rpc: chain, chain: MAIN, wallet: DEV, token: "USDC", entropy });
  assert.equal(f["start_block"], 1000 - START_SLOT_MARGIN);
  assert.equal(f["dest"], dest);
  assert.equal(f["scan_cursor"], sig(9));
  assert.equal(b58decode(f["reference"] as string).length, 32);
  assert.equal(f["reference"], b58encode(new Uint8Array(32).fill(1)));
  assert.equal(f["scan_before"], null);
  assert.deepEqual(Object.keys(f), ["start_block", "reference", "dest", "scan_cursor", "scan_before"]);
  const again = await B.start({ rpc: chain, chain: MAIN, wallet: DEV, token: "USDC", entropy });
  assert.notEqual(again["reference"], f["reference"]);
  const fresh = await B.start({ rpc: new FakeSolanaChain(10), chain: MAIN, wallet: OLD, token: "USDC", entropy });
  assert.equal(fresh["scan_cursor"], null);
  assert.equal(fresh["start_block"], 0); // floored at zero
});

test("test_start_draws_the_reference_after_the_network_calls", async () => {
  const chain = new FakeSolanaChain(1000);
  chain.down = true;
  let drawn = 0;
  const entropy: Entropy = { ...countingEntropy(), tokenBytes: (k) => { drawn += 1; return new Uint8Array(k); } };
  await assert.rejects(B.start({ rpc: chain, chain: MAIN, wallet: DEV, token: "USDC", entropy }), RpcUnavailable);
  assert.equal(drawn, 0);
});

function receiptFor(txSig: string, opts: { value?: number; slot?: number; recipient?: string; index?: number } = {}) {
  const { value = 4_993_742, slot = 950, recipient = DEV, index = 2 } = opts;
  const transfer: Transfer = {
    token: MAIN_MINT, sender: PAYER, recipient, value: asUnits(BigInt(value)), tx_hash: txSig, log_index: index, block_number: slot,
  };
  return makeReceipt({ chain: "solana", transfer, kind: "unlock", item: "chapter2", token: "USDC", source: "payment" }) as unknown as Record<string, unknown>;
}

function verdict(chain: FakeSolanaChain, r: Record<string, unknown>, wallets: string[] = [DEV, OLD]) {
  return B.reverify(r, { rpc: chain, chain: MAIN, wallets });
}

test("test_reverify_keeps_genuine_and_previous_wallet_receipts", async () => {
  const chain = new FakeSolanaChain();
  chain.pay(sig(1), { mint: MAIN_MINT, payer: PAYER, owner: DEV, amount: 4_993_742, slot: 950 });
  chain.pay(sig(2), { mint: MAIN_MINT, payer: PAYER, owner: OLD, amount: 4_993_742, slot: 950 });
  assert.equal(await verdict(chain, receiptFor(sig(1))), "keep");
  assert.equal(await verdict(chain, receiptFor(sig(2), { recipient: OLD })), "keep");
});

const CONTRADICTED: [string, Parameters<typeof receiptFor>[1], Record<string, unknown>][] = [
  ["amount differs", { value: 9_000_000 }, {}],
  ["slot differs", { slot: 951 }, {}],
  ["not paid to the developer", { recipient: PAYER }, {}],
  ["failed transaction", {}, { err: { InstructionError: [0, "Custom"] } }],
];
for (const [why, receiptKw, payKw] of CONTRADICTED) {
  test(`test_reverify_drops_contradicted_receipts[${why}]`, async () => {
    const chain = new FakeSolanaChain();
    chain.pay(sig(1), { mint: MAIN_MINT, payer: PAYER, owner: DEV, amount: 4_993_742, slot: 950, ...payKw });
    assert.equal(await verdict(chain, receiptFor(sig(1), receiptKw)), "drop");
  });
}

test("test_reverify_drops_when_the_log_index_differs", async () => {
  const chain = new FakeSolanaChain();
  chain.pay(sig(1), { mint: MAIN_MINT, payer: PAYER, owner: DEV, amount: 4_993_742, slot: 950 });
  assert.equal(await verdict(chain, receiptFor(sig(1), { index: 1 })), "drop");
});

test("test_invented_signature_is_dropped_when_history_covers_it", async () => {
  const chain = new FakeSolanaChain();
  chain.first_available = 0;
  assert.equal(await verdict(chain, receiptFor(sig(1))), "drop");
});

test("test_old_receipt_on_a_pruned_endpoint_is_kept", async () => {
  // publicnode keeps ~2 days; a null from it proves nothing.
  const chain = new FakeSolanaChain();
  chain.first_available = 10 ** 9;
  assert.equal(await verdict(chain, receiptFor(sig(1))), "keep");
});

test("test_outage_keeps_and_far_future_slot_drops", async () => {
  const chain = new FakeSolanaChain(1000);
  chain.down = true;
  assert.equal(await verdict(chain, receiptFor(sig(1))), "keep");
  chain.down = false;
  assert.equal(await verdict(chain, receiptFor(sig(1), { slot: 1000 + 20_000 })), "drop");
  chain.pay(sig(3), { mint: MAIN_MINT, payer: PAYER, owner: DEV, amount: 4_993_742, slot: 1005 });
  assert.equal(await verdict(chain, receiptFor(sig(3), { slot: 1005 })), "keep"); // lagging head
  assert.equal(await verdict(chain, receiptFor(sig(1), { slot: 1000 + 10_000 })), "keep"); // the tolerance edge
});

test("test_reverify_asks_the_finalized_slot_and_errors_keep", async () => {
  const chain = new FakeSolanaChain(1000);
  const seen: string[] = [];
  chain.getSlot = async (c = "confirmed") => { seen.push(c); throw new RpcError(-32005, "node is behind"); };
  assert.equal(await verdict(chain, receiptFor(sig(1))), "keep");
  assert.deepEqual(seen, ["finalized"]);
});

test("test_reverify_drops_a_receipt_for_a_wallet_no_longer_accepted", async () => {
  const chain = new FakeSolanaChain();
  assert.equal(await verdict(chain, receiptFor(sig(1)), [OLD]), "drop");
  assert.deepEqual(chain.calls, []); // judged without the network
});

test("test_lookup_classifies_by_amount_code", async () => {
  const chain = new FakeSolanaChain();
  chain.pay(sig(1), { mint: MAIN_MINT, payer: PAYER, owner: DEV, amount: 4_993_742, slot: 950 });
  const out = (await B.lookup(sig(1), { config: cfg(), chain: MAIN, rpc: chain, receipts: {} }))!;
  assert.equal(out.length, 1);
  const c = out[0]!;
  assert.deepEqual([c.chain, c.token, c.kind, c.item, c.already_restored], ["solana", "USDC", "unlock", "chapter2", false]);
  assert.equal(c.transfer.value, 4_993_742n);
  assert.equal(c.transfer.log_index, 2);
  assert.ok(!("block_hash" in c.transfer)); // a plain Transfer
  const receipts = { [receiptKey("solana", sig(1), 2)]: {} };
  assert.ok((await B.lookup(sig(1), { config: cfg(), chain: MAIN, rpc: chain, receipts }))![0]!.already_restored);
});

test("test_lookup_skips_a_value_decode_cannot_price", async () => {
  // Python: classify -> decode raises decimal.InvalidOperation from 10**32 units; that transfer is
  // skipped and reported, so the transaction pays nothing the game knows.
  const chain = new FakeSolanaChain();
  const tx = chain.pay(sig(1), { mint: MAIN_MINT, payer: PAYER, owner: DEV, amount: 1, slot: 950 });
  tx["meta"]["postTokenBalances"][1]["uiTokenAmount"]["amount"] = String(10n ** 32n);
  const errors: unknown[] = [];
  await assert.rejects(B.lookup(sig(1), { config: cfg(), chain: MAIN, rpc: chain, receipts: {}, on_error: (e) => errors.push(e) }),
    (e: unknown) => e instanceof RestoreError && e.code === "tx_not_a_payment");
  assert.equal(errors.length, 1);
  assert.ok(errors[0] instanceof InvalidOperation && !(errors[0] instanceof ValueError));
});

test("test_lookup_offers_previous_wallet_payments_too", async () => {
  const chain = new FakeSolanaChain();
  chain.pay(sig(1), { mint: MAIN_MINT, payer: PAYER, owner: OLD, amount: 4_993_742, slot: 950 });
  const out = (await B.lookup(sig(1), { config: cfg(), chain: MAIN, rpc: chain, receipts: {} }))!;
  assert.deepEqual(out.map((c) => [c.token, c.transfer.recipient]), [["USDC", OLD]]);
});

test("test_lookup_outcomes", async () => {
  const chain = new FakeSolanaChain();
  assert.equal(await B.lookup(sig(1), { config: cfg(), chain: MAIN, rpc: chain, receipts: {} }), null);
  chain.pay(sig(2), { mint: MAIN_MINT, payer: PAYER, owner: DEV, amount: 1, err: { x: 1 } });
  await assert.rejects(B.lookup(sig(2), { config: cfg(), chain: MAIN, rpc: chain, receipts: {} }),
    (e: unknown) => e instanceof RestoreError && e.code === "tx_reverted");
  chain.pay(sig(3), { mint: MAIN_MINT, payer: DEV, owner: PAYER, amount: 1_000_000 });
  await assert.rejects(B.lookup(sig(3), { config: cfg(), chain: MAIN, rpc: chain, receipts: {} }),
    (e: unknown) => e instanceof RestoreError && e.code === "tx_not_a_payment");
});

test("test_restore_routes_signatures_to_solana_and_hashes_to_evm", async () => {
  const sol = new FakeSolanaChain();
  sol.pay(sig(1), { mint: MAIN_MINT, payer: PAYER, owner: DEV, amount: 4_993_742 });
  const evm = new FakeChain();
  const chains = new Map<string, Chain>([["arbitrum", MAINNET["arbitrum"]!], ["solana", MAIN]]);
  const rpcs: Record<string, unknown> = { arbitrum: evm, solana: sol };
  const out = await lookup(sig(1), { config: cfg(), chains, rpc_for: (k) => rpcs[k], receipts: {} });
  assert.equal(out[0]!.chain, "solana");
  assert.deepEqual(evm.calls, []);
  await assert.rejects(lookup("0x" + "ab".repeat(32), { config: cfg(), chains, rpc_for: (k) => rpcs[k], receipts: {} }),
    (e: unknown) => e instanceof RestoreError && e.code === "tx_not_found");
  assert.ok(!sol.calls.slice(1).includes("get_transaction_any"));
});

test("test_check_ready", async () => {
  const chain = new FakeSolanaChain();
  chain.accounts.add(associatedTokenAddress(DEV, MAIN_MINT));
  assert.deepEqual(await B.checkReady({ rpc: chain, chain: MAIN, wallet: DEV }), { USDC: true, USDT: false });
});

const UNREADABLE: [string, Record<string, unknown>][] = [
  ["no owner or amount", { meta: { err: null, postTokenBalances: [{ mint: MAIN_MINT, accountIndex: 2 }] } }],
  ["amount not a number", { meta: { err: null, postTokenBalances: [
    { mint: MAIN_MINT, accountIndex: 2, owner: DEV, uiTokenAmount: { amount: "x" } }] } }],
  ["balances not a list", { meta: { err: null, postTokenBalances: "garbage" } }],
];
for (const [why, payload] of UNREADABLE) {
  test(`test_reverify_keeps_a_receipt_whose_transaction_it_cannot_read[${why}]`, async () => {
    const chain = new FakeSolanaChain();
    chain.txs[sig(1)] = { ...payload, slot: 950 };
    assert.equal(await verdict(chain, receiptFor(sig(1))), "keep");
  });
}

// --- test_solana_watch.py -------------------------------------------------------------------

const MINT = "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v";
const REF = pubkey(1);
const DEST = "OWNERATA";
const UNITS = 4_993_742;
const CHAIN: Chain = {
  ...MAIN, rpc: ["https://x.example"], confirmations: FINAL_CONFIRMATIONS, max_log_range: 0,
  explorer_tx: "https://solscan.io/tx/{}", tokens: { USDC: makeToken(MINT) }, wallet_chain: "", native_token: "",
};
const ITEMS: ReadonlyMap<string, Item> = new Map([[
  "chapter2", {
    item_id: "chapter2", code: 37, price: Dec.parse("4.99") as Dec, token: "USDC", title: "Chapter 2",
    min_price: Dec.parse("4.99") as Dec,
  } as Item,
]]);

function setup(kw: Record<string, unknown> = {}) {
  const pending: SessionStore = {};
  const receipts: SessionStore = {};
  const chain = new FakeSolanaChain(1000);
  const rec = createSession(pending, {
    kind: "unlock", item: "chapter2", title: "Chapter 2", token: "USDC", chain: "solana", price: "4.99", code: 37,
    nonce: 42, start_block: 900, now: 0.0, extra: { reference: REF, dest: DEST, scan_cursor: null }, ...kw,
  });
  return { pending, receipts, chain, rec };
}

type Setup = ReturnType<typeof setup>;

function step(s: Setup, now = 1.0, b: SolanaBackend = B) {
  return b.advance(s.pending, s.receipts, s.rec["id"], { rpc: s.chain, chain: CHAIN, wallet: DEV, items: ITEMS, now });
}

function hint(s: Setup, tx: string, at = 0.0): void {
  setHint(s.pending, s.rec["id"], tx, at, (t) => B.normalizeTx(t));
}

const pay = (chain: FakeSolanaChain, n: number, opts: Partial<Parameters<FakeSolanaChain["pay"]>[1]> = {}) =>
  chain.pay(sig(n), { mint: MINT, payer: PAYER, owner: DEV, amount: UNITS, ...opts });

test("test_registered_and_normalizes_signatures", () => {
  assert.equal(backendForFamily("solana").family, "solana");
  assert.ok(backendForFamily("solana") instanceof SolanaBackend);
  assert.equal(backendForFamily("solana"), backendForFamily("solana"));
  const s = sig(5);
  assert.equal(B.normalizeTx(" " + s), s);
  assert.equal(s, normalizeSignature(s));
  assert.throws(() => B.normalizeTx("0x" + "ab".repeat(32)), ValueError);
});

test("test_token_accepts_base58_mints_and_keeps_evm_checksums", () => {
  assert.equal(makeToken(MINT).address, MINT);
  const arb = MAINNET["arbitrum"]!.tokens["USDC"]!.address;
  assert.equal(makeToken(arb.toLowerCase()).address, arb);
  assert.throws(() => makeToken("not-an-address"), ValueError);
});

test("test_transfer_to_reads_balance_changes", () => {
  const chain = new FakeSolanaChain();
  const tx = pay(chain, 1, { slot: 950 });
  const t = transferTo(tx, { sig: sig(1), mint: MINT, owner: DEV })!;
  assert.deepEqual([t.value, t.sender, t.recipient, t.log_index, t.block_number, t.token, t.tx_hash],
    [BigInt(UNITS), PAYER, DEV, 2, 950, MINT, sig(1)]);
  assert.equal(transferTo(tx, { sig: sig(1), mint: MINT, owner: PAYER }), null);
  assert.equal(transferTo(tx, { sig: sig(1), mint: "OTHERMINT", owner: DEV }), null);
  const failed = pay(chain, 2, { err: { InstructionError: [0, "Custom"] } });
  assert.equal(transferTo(failed, { sig: sig(2), mint: MINT, owner: DEV }), null);
});

test("test_transfer_to_payer_is_the_largest_drop_else_the_fee_payer", () => {
  const row = (i: number, who: string, v: number) => ({ accountIndex: i, mint: MINT, owner: who, uiTokenAmount: { amount: String(v) } });
  const tx = {
    slot: "77",
    transaction: { message: { accountKeys: [{ pubkey: OLD }, "x"] } },
    meta: {
      err: null,
      preTokenBalances: [row(3, OLD, 10), row(1, PAYER, 10), row(4, REF, 10)],
      postTokenBalances: [row(3, OLD, 7), row(1, PAYER, 3), row(4, REF, 3), row(5, DEV, 7), row(6, DEV, 6)],
    },
  };
  const t = transferTo(tx, { sig: "s", mint: MINT, owner: DEV })!;
  // PAYER and REF both drop 7: Python min() keeps the first in accountIndex order (1 before 4)
  assert.deepEqual([t.sender, t.value, t.log_index, t.block_number], [PAYER, 13n, 5, 77]);
  const noDrops = { ...tx, meta: { err: null, preTokenBalances: [], postTokenBalances: [row(2, DEV, 5)] } };
  assert.equal(transferTo(noDrops, { sig: "s", mint: MINT, owner: DEV })!.sender, OLD); // accountKeys[0].pubkey
  const plainKey = { ...noDrops, transaction: { message: { accountKeys: [PAYER] } } };
  assert.equal(transferTo(plainKey, { sig: "s", mint: MINT, owner: DEV })!.sender, PAYER);
  const noKeys = { ...noDrops, transaction: null };
  assert.equal(transferTo(noKeys, { sig: "s", mint: MINT, owner: DEV })!.sender, null);
});

test("test_transfer_to_net_gain_and_malformed_rows", () => {
  const row = (i: number, who: unknown, v: unknown) => ({ accountIndex: i, mint: MINT, owner: who, uiTokenAmount: { amount: v } });
  // a net loss across the owner's accounts is no payment
  const loss = { slot: 1, meta: { preTokenBalances: [row(2, DEV, "10")], postTokenBalances: [row(2, DEV, "4"), row(3, DEV, "5")] } };
  assert.equal(transferTo(loss, { sig: "s", mint: MINT, owner: DEV }), null);
  // Python int() forms: whitespace, underscores, sign
  const forms = { slot: 1, meta: { preTokenBalances: [], postTokenBalances: [row(2, DEV, " +1_000 ")] } };
  assert.equal(transferTo(forms, { sig: "s", mint: MINT, owner: DEV })!.value, 1000n);
  // u64 amounts beyond 2^53 stay exact
  const big = { slot: 1, meta: { preTokenBalances: [], postTokenBalances: [row(2, DEV, "18446744073709551615")] } };
  assert.equal(transferTo(big, { sig: "s", mint: MINT, owner: DEV })!.value, 18446744073709551615n);
  const bad = { slot: 1, meta: { postTokenBalances: [row(2, DEV, "x")] } };
  assert.throws(() => transferTo(bad, { sig: "s", mint: MINT, owner: DEV }), ValueError);
  const noIndex = { slot: 1, meta: { postTokenBalances: [{ mint: MINT, owner: DEV, uiTokenAmount: { amount: "1" } }] } };
  assert.throws(() => transferTo(noIndex, { sig: "s", mint: MINT, owner: DEV }), { name: "KeyError" });
  const noSlot = { meta: { postTokenBalances: [row(2, DEV, "1")] } };
  assert.throws(() => transferTo(noSlot, { sig: "s", mint: MINT, owner: DEV }), { name: "KeyError" });
  assert.throws(() => transferTo({ meta: { postTokenBalances: "garbage" } }, { sig: "s", mint: MINT, owner: DEV }), TypeError);
  assert.throws(() => transferTo({ meta: "x" }, { sig: "s", mint: MINT, owner: DEV }), TypeError);
  // other mints are never read
  const other = { slot: 1, meta: { postTokenBalances: [{ mint: "M", accountIndex: [1] }] } };
  assert.equal(transferTo(other, { sig: "s", mint: MINT, owner: DEV }), null);
});

test("test_waits_when_nothing_arrived", async () => {
  const s = setup();
  assert.equal((await step(s)).status, "waiting");
});

test("test_reference_payment_confirms_then_finalizes_into_a_receipt", async () => {
  const s = setup();
  pay(s.chain, 1, { slot: 950, addresses: [REF, DEST] });
  let out = await step(s);
  assert.deepEqual([out.status, out.confirmations, out.required], ["confirming", 1, 32]);
  s.chain.finalize(sig(1));
  out = await step(s);
  assert.equal(out.status, "paid");
  assert.equal(out.result!.tx_hash, sig(1));
  assert.equal(out.result!.chain, "solana");
  assert.equal(out.result!.payer, PAYER);
  assert.equal(String(out.result!.amount), "4.993742");
  const key = receiptKey("solana", sig(1), 2);
  assert.equal(s.receipts[key]["block"], 950);
  assert.equal(s.receipts[key]["recipient"], DEV);
  assert.ok(!(s.rec["id"] in s.pending));
});

test("test_reference_matches_any_amount_and_detects_underpayment", async () => {
  let s = setup();
  pay(s.chain, 1, { amount: 5_000_000, addresses: [REF] });
  assert.equal((await step(s)).status, "confirming");
  s = setup();
  pay(s.chain, 2, { amount: 1_000_000, addresses: [REF] });
  const out = await step(s);
  assert.equal(out.status, "underpaid");
  assert.ok(!(s.rec["id"] in s.pending));
  assert.equal(String(out.result!.amount), "1.000000");
});

test("test_scan_matches_exact_amount_only_and_moves_cursor", async () => {
  const s = setup();
  pay(s.chain, 1, { amount: 5_000_000, addresses: [DEST] });
  assert.equal((await step(s)).status, "waiting");
  assert.equal(s.pending[s.rec["id"]]["scan_cursor"], sig(1));
  pay(s.chain, 2, { addresses: [DEST] });
  assert.equal((await step(s)).status, "confirming");
  assert.equal(s.pending[s.rec["id"]]["match"]["tx_hash"], sig(2));
});

test("test_scan_spends_at_most_five_transactions_per_step", async () => {
  const s = setup();
  for (let n = 1; n < 8; n++) pay(s.chain, n, { amount: 1_230_000, addresses: [DEST] });
  pay(s.chain, 99, { addresses: [DEST] });
  assert.equal((await step(s)).status, "waiting");
  assert.equal(counted(s.chain.calls, "get_transaction"), 5);
  assert.equal((await step(s)).status, "confirming");
});

test("test_scan_pages_back_past_a_full_page", async () => {
  // >100 new signatures since the last step must not hide a payment.
  const s = setup();
  pay(s.chain, 500, { addresses: [DEST] });
  for (let n = 1; n < 250; n++) s.chain.addresses[DEST]!.unshift({ signature: sig(n), slot: 990, err: null });
  assert.equal((await step(s)).status, "confirming");
  assert.ok(counted(s.chain.calls, "get_signatures_for_address") >= 3);
});

test("test_scan_skips_failed_signatures_without_fetching_them", async () => {
  const s = setup();
  pay(s.chain, 1, { addresses: [DEST], err: { InstructionError: [0, "Custom"] } });
  assert.equal((await step(s)).status, "waiting");
  assert.ok(!s.chain.calls.includes("get_transaction"));
  assert.equal(s.pending[s.rec["id"]]["scan_cursor"], sig(1));
});

test("test_already_receipted_payment_is_not_matched_again", async () => {
  const s = setup();
  pay(s.chain, 1, { addresses: [REF] });
  s.receipts[receiptKey("solana", sig(1), 2)] = { chain: "solana" };
  assert.equal((await step(s)).status, "waiting");
});

const BAD_HINTS: [string, Record<string, unknown>, string][] = [
  ["reverted", { err: { InstructionError: [0, "Custom"] } }, "tx_reverted"],
  ["to someone else", { owner: PAYER }, "tx_not_a_payment"],
  ["before the session started", { slot: 100 }, "tx_not_a_payment"],
];
for (const [why, kwargs, error] of BAD_HINTS) {
  test(`test_bad_hints_are_dropped_with_a_reason[${why}]`, async () => {
    const s = setup();
    pay(s.chain, 1, kwargs);
    hint(s, sig(1));
    const out = await step(s);
    assert.deepEqual([out.status, out.error], ["waiting", error]);
    assert.equal(s.pending[s.rec["id"]]["hint_tx"], null);
  });
}

test("test_hint_not_yet_visible_waits_then_expires", async () => {
  const s = setup();
  hint(s, sig(1));
  assert.equal((await step(s, 10.0)).status, "waiting");
  assert.equal(s.pending[s.rec["id"]]["hint_tx"], sig(1));
  await step(s, HINT_TIMEOUT_S + 1);
  assert.equal(s.pending[s.rec["id"]]["hint_tx"], null);
});

test("test_good_hint_matches_round_amount", async () => {
  const s = setup();
  pay(s.chain, 1, { amount: 5_000_000 });
  hint(s, sig(1));
  assert.equal((await step(s)).status, "confirming");
});

test("test_vanished_match_becomes_a_hint_and_is_found_again", async () => {
  // a lagging endpoint's null status must not lose the payment.
  const s = setup();
  pay(s.chain, 1, { addresses: [DEST] });
  assert.equal((await step(s)).status, "confirming");
  const saved = s.chain.statuses[sig(1)];
  delete s.chain.statuses[sig(1)];
  const out = await step(s, 5.0);
  assert.equal(out.status, "reorged");
  assert.equal(s.pending[s.rec["id"]]["state"], "awaiting");
  assert.equal(s.pending[s.rec["id"]]["hint_tx"], sig(1));
  assert.equal(s.pending[s.rec["id"]]["hint_at"], 5.0);
  assert.equal(s.pending[s.rec["id"]]["match"], null);
  s.chain.statuses[sig(1)] = saved; // the endpoint caught up
  assert.equal((await step(s, 6.0)).status, "confirming");
  s.chain.finalize(sig(1));
  assert.equal((await step(s, 7.0)).status, "paid");
});

test("test_network_outage_changes_nothing", async () => {
  const s = setup();
  s.chain.down = true;
  const before = JSON.parse(JSON.stringify(s.pending[s.rec["id"]]));
  const out = await step(s);
  assert.equal(out.error, "network_unavailable");
  assert.deepEqual(s.pending[s.rec["id"]], before);
});

test("test_horizon_closes_the_session", async () => {
  const s = setup();
  assert.equal((await step(s, SCAN_HORIZON_S + 1)).status, "closed");
  assert.ok(!(s.rec["id"] in s.pending));
});

test("test_wrong_mint_on_the_reference_is_ignored", async () => {
  const s = setup();
  pay(s.chain, 1, { mint: "So11111111111111111111111111111111111111112", addresses: [REF] });
  assert.equal((await step(s)).status, "waiting");
});

/**
 * sig(1) is the old cursor; `spam` unrelated transfers newer than it, with the exact payment
 * placed after `paymentAt` of them (0 = just after the cursor).
 */
function busyAccount(chain: FakeSolanaChain, spam: number, paymentAt: number): void {
  pay(chain, 1, { amount: 1_230_000, addresses: [DEST] });
  for (let n = 0; n <= spam; n++) {
    if (n === paymentAt) pay(chain, 9000, { addresses: [DEST] });
    if (n < spam) pay(chain, 100 + n, { amount: 1_230_000, addresses: [DEST] });
  }
}

async function stepsUntilConfirming(s: Setup, b: SolanaBackend = B, limit = 300) {
  for (let n = 0; n < limit; n++) {
    const out = await step(s, n, b);
    if (out.status !== "waiting") return out;
  }
  throw new Error("payment never matched");
}

/** Python monkeypatches SIG_PAGE and MAX_SIG_PAGES; here a backend instance carries its own. */
function smallPages(): SolanaBackend {
  const b = new SolanaBackend();
  b.sig_page = 10;
  b.max_sig_pages = 3;
  return b;
}

test("test_scan_crosses_a_backlog_longer_than_one_walk", async () => {
  // A busy account (>SIG_PAGE * MAX_SIG_PAGES new signatures) only delays detection.
  const s = setup();
  busyAccount(s.chain, 1050, 0);
  s.rec["scan_cursor"] = sig(1);
  const out = await stepsUntilConfirming(s);
  assert.equal(out.status, "confirming");
  assert.equal(s.pending[s.rec["id"]]["match"]["tx_hash"], sig(9000));
});

for (const paymentAt of [0, 40, 80, 104, 105]) {
  test(`test_scan_never_skips_signatures_when_it_falls_far_behind[${paymentAt}]`, async () => {
    const b = smallPages();
    const s = setup();
    busyAccount(s.chain, 105, paymentAt); // 106+ signatures: several walks deep
    s.rec["scan_cursor"] = sig(1);
    const out = await stepsUntilConfirming(s, b);
    assert.equal(out.status, "confirming");
    assert.equal(s.pending[s.rec["id"]]["match"]["tx_hash"], sig(9000));
    assert.ok(s.chain.calls.includes("get_transaction"));
  });
}

test("test_scan_walk_resumes_where_it_stopped_after_new_traffic", async () => {
  const b = smallPages();
  const s = setup();
  busyAccount(s.chain, 70, 35);
  s.rec["scan_cursor"] = sig(1);
  for (let n = 0; n < 4; n++) assert.equal((await step(s, n, b)).status, "waiting"); // walked back, started the oldest window
  for (let k = 0; k < 40; k++) pay(s.chain, 5000 + k, { amount: 1_230_000, addresses: [DEST] }); // traffic mid-backlog
  await stepsUntilConfirming(s, b);
  assert.equal(s.pending[s.rec["id"]]["match"]["tx_hash"], sig(9000));
});

test("test_outage_during_a_scan_walk_leaves_the_cursors_unchanged", async () => {
  const b = smallPages();
  const s = setup();
  busyAccount(s.chain, 105, 50);
  s.rec["scan_cursor"] = sig(1);
  assert.equal((await step(s, 1.0, b)).status, "waiting"); // first walk: moves scan_before
  assert.notEqual(s.pending[s.rec["id"]]["scan_before"], null);
  const before = JSON.parse(JSON.stringify(s.pending[s.rec["id"]]));
  const real = s.chain.getSignaturesForAddress.bind(s.chain);
  const calls: number[] = [];
  s.chain.getSignaturesForAddress = async (address, opts) => {
    calls.push(1);
    if (calls.length === 2) throw new RpcUnavailable("fake outage"); // dies mid-walk
    return real(address, opts);
  };
  const out = await step(s, 1.0, b);
  assert.equal(out.error, "network_unavailable");
  assert.equal(calls.length, 2);
  assert.deepEqual(s.pending[s.rec["id"]], before);
});

test("test_hint_that_never_lands_expires_after_the_solana_timeout", async () => {
  // A wallet returns the signature before the transaction lands; dropped ones are routine.
  assert.equal(SOLANA_HINT_TIMEOUT_S, 180);
  const s = setup();
  hint(s, sig(1));
  let out = await step(s, SOLANA_HINT_TIMEOUT_S - 1);
  assert.deepEqual([out.status, out.error], ["waiting", null]);
  assert.equal(s.pending[s.rec["id"]]["hint_tx"], sig(1));
  out = await step(s, SOLANA_HINT_TIMEOUT_S);
  assert.deepEqual([out.status, out.error, out.hint_tx], ["waiting", "tx_not_landed", sig(1)]);
  assert.equal(s.pending[s.rec["id"]]["hint_tx"], null);
});

test("test_reference_payment_matches_while_a_hint_is_pending", async () => {
  const s = setup();
  hint(s, sig(1)); // never lands
  pay(s.chain, 2, { addresses: [REF, DEST] });
  const out = await step(s, 10.0);
  assert.deepEqual([out.status, out.error], ["confirming", null]);
  assert.equal(s.pending[s.rec["id"]]["match"]["tx_hash"], sig(2));
  assert.equal(s.pending[s.rec["id"]]["hint_tx"], null);
});

test("test_reference_payment_clears_the_verdict_on_a_dropped_hint", async () => {
  // a hint dropped this step and a reference payment found in the same step: the payment wins
  const s = setup();
  pay(s.chain, 1, { err: { InstructionError: [0, "Custom"] } });
  hint(s, sig(1));
  pay(s.chain, 2, { addresses: [REF] });
  const out = await step(s, 10.0);
  assert.deepEqual([out.status, out.error, out.hint_tx], ["confirming", null, null]);
});

test("test_pending_hint_suspends_the_scan", async () => {
  const s = setup();
  hint(s, sig(1)); // not visible yet
  pay(s.chain, 2, { addresses: [DEST] });
  assert.equal((await step(s, 10.0)).status, "waiting");
  assert.equal(counted(s.chain.calls, "get_signatures_for_address"), 1); // the reference only
});

test("test_hint_whose_lookup_keeps_failing_still_expires", async () => {
  const s = setup();
  hint(s, sig(1));
  s.chain.getTransaction = async () => { throw new RpcError(-32015, "Transaction version (9) is not supported"); };
  let out = await step(s, 10.0);
  assert.equal(out.error, "network_unavailable");
  assert.equal(s.pending[s.rec["id"]]["hint_tx"], sig(1));
  out = await step(s, SOLANA_HINT_TIMEOUT_S + 1);
  assert.deepEqual([out.status, out.error], ["waiting", "tx_not_landed"]);
  assert.equal(s.pending[s.rec["id"]]["hint_tx"], null);
});

test("test_expired_hint_lookup_outage_is_still_network_trouble", async () => {
  // only an RpcError is forgiven once the hint is past its timeout; an outage is not a verdict
  const s = setup();
  hint(s, sig(1));
  s.chain.getTransaction = async () => { throw new RpcUnavailable("down"); };
  const out = await step(s, SOLANA_HINT_TIMEOUT_S + 1);
  assert.equal(out.error, "network_unavailable");
  assert.equal(s.pending[s.rec["id"]]["hint_tx"], sig(1));
});

for (const failure of ["unavailable", "rpc_error"]) {
  test(`test_hint_expiry_is_reported_even_if_the_reference_lookup_fails[${failure}]`, async () => {
    // Later ticks are plain "waiting" (not forwarded), so the expiry tick must say it.
    const s = setup();
    hint(s, sig(1));
    s.chain.getSignaturesForAddress = async () => {
      if (failure === "unavailable") throw new RpcUnavailable("all endpoints down");
      throw new RpcError(-32005, "node is behind");
    };
    let out = await step(s, SOLANA_HINT_TIMEOUT_S);
    assert.deepEqual([out.status, out.error], ["waiting", "tx_not_landed"]);
    assert.equal(out.hint_tx, sig(1)); // which hint didn't land
    assert.equal(s.pending[s.rec["id"]]["hint_tx"], null);
    out = await step(s, SOLANA_HINT_TIMEOUT_S + 1);
    assert.deepEqual([out.status, out.error], ["waiting", "network_unavailable"]);
  });
}

test("test_hint_verdicts_name_the_hint_they_are_about", async () => {
  const s = setup();
  pay(s.chain, 1, { err: { InstructionError: [0, "Custom"] } });
  hint(s, sig(1));
  let out = await step(s, 1.0);
  assert.deepEqual([out.error, out.hint_tx], ["tx_reverted", sig(1)]);
  out = await step(s, 2.0);
  assert.deepEqual([out.error, out.hint_tx], [null, null]);
});

for (const cursor of [null, "unknown-to-this-endpoint"]) {
  test(`test_scan_walk_stops_at_signatures_older_than_the_session[${cursor}]`, async () => {
    // Without a usable cursor the walk must not crawl the token account's whole history.
    const s = setup();
    for (let n = 1; n < 251; n++) pay(s.chain, n, { slot: 500 + n, addresses: [DEST] }); // history from before the session
    pay(s.chain, 999, { slot: 950, addresses: [DEST] });
    s.rec["scan_cursor"] = cursor === null ? null : sig(12345);
    const out = await step(s);
    assert.equal(out.status, "confirming");
    assert.equal(s.pending[s.rec["id"]]["match"]["tx_hash"], sig(999));
    assert.equal(counted(s.chain.calls, "get_transaction"), 1);
    assert.ok(counted(s.chain.calls, "get_signatures_for_address") <= 2); // reference + one page
  });
}

test("test_confirming_reports_the_node_confirmations", async () => {
  const s = setup();
  pay(s.chain, 1, { addresses: [REF] });
  await step(s);
  s.chain.statuses[sig(1)] = { ...s.chain.statuses[sig(1)], confirmations: "7" };
  assert.deepEqual([(await step(s)).confirmations], [7]);
  s.chain.statuses[sig(1)] = { ...s.chain.statuses[sig(1)], confirmations: null };
  assert.deepEqual([(await step(s)).confirmations], [0]);
});

test("test_missing_session", async () => {
  const s = setup();
  const out = await B.advance(s.pending, s.receipts, "ps_nope", { rpc: s.chain, chain: CHAIN, wallet: DEV, items: ITEMS, now: 1 });
  assert.deepEqual(out, {
    session_id: "ps_nope", status: "missing", confirmations: 0, required: 32, result: null, error: null, hint_tx: null,
  });
});

test("test_malformed_session_raises_instead_of_waiting", async () => {
  const s = setup();
  delete s.pending[s.rec["id"]]["scan_cursor"];
  await assert.rejects(step(s), { name: "KeyError" }); // the Engine contains it as internal_error
});

// --- test_solana_build.py (backend) ------------------------------------------------------

const PLAYER = PAYER;
const BLOCKHASH = "4sGjMW1sUnHzSxGspuhpqLDx6wiyjNtZAMdL4VZHirAn";

class BuildRpc extends FakeSolanaChain {
  fee = 5_000;
  async getTokenBalance(account: string): Promise<bigint> {
    return account === associatedTokenAddress(PLAYER, MAIN_MINT) ? 7_000_000n : 0n;
  }
  async getBalance(_address: string): Promise<bigint> {
    return 2_000_000n;
  }
  async getLatestBlockhash(): Promise<string> {
    return BLOCKHASH;
  }
  async getPriorityFee(_accounts: Iterable<string>): Promise<number> {
    return this.fee;
  }
}

function sessionRec(): Record<string, any> {
  return { token: "USDC", units: 4_993_742, reference: pubkey(2), dest: associatedTokenAddress(DEV, MAIN_MINT) };
}

/** Enough of a legacy transaction parser to check what a wallet will be asked to sign. */
function parseTx(raw: Uint8Array) {
  let o = 0;
  const cu16 = (): number => {
    let n = 0;
    for (let shift = 0; ; shift += 7) {
      const b = raw[o++]!;
      n |= (b & 0x7f) << shift;
      if (!(b & 0x80)) return n;
    }
  };
  const take = (n: number) => { const s = raw.slice(o, o + n); o += n; return s; };
  const nsig = cu16();
  const sigs = take(64 * nsig);
  const header = [...take(3)];
  const keys = Array.from({ length: cu16() }, () => b58encode(take(32)));
  const blockhash = b58encode(take(32));
  const instructions = Array.from({ length: cu16() }, () => {
    const program = keys[raw[o++]!]!;
    const accounts = [...take(cu16())].map((i) => keys[i]!);
    const data = take(cu16());
    return { program, accounts, data };
  });
  assert.equal(o, raw.length);
  return { nsig, sigs, header, keys, blockhash, instructions };
}

const le = (v: bigint, size: number) => {
  const out = new Uint8Array(size);
  for (let i = 0; i < size; i++) out[i] = Number((v >> BigInt(8 * i)) & 0xffn);
  return out;
};

for (const [fee, expected] of [[5_000, 5_000], [0, MIN_PRIORITY_FEE], [10 ** 12, MAX_PRIORITY_FEE]] as const) {
  test(`test_build_payment_transaction[${fee}]`, async () => {
    const rpc = new BuildRpc();
    rpc.fee = fee;
    const rec = sessionRec();
    const out = await new SolanaBackend().buildPayment(rec, { rpc, chain: MAIN, account: PLAYER });
    assert.deepEqual(Object.keys(out), ["transaction", "token_balance", "sol_balance"]);
    assert.deepEqual([out["token_balance"], out["sol_balance"]], ["7000000", "2000000"]);
    const tx = parseTx(new Uint8Array(Buffer.from(out["transaction"] as string, "base64")));
    assert.equal(Buffer.from(Buffer.from(out["transaction"] as string, "base64")).toString("base64"), out["transaction"]);
    assert.equal(tx.nsig, 1);
    assert.ok(tx.sigs.every((b) => b === 0)); // unsigned: the wallet fills the slot
    assert.deepEqual(tx.header, [1, 0, 4]); // one signer (the player); mint, reference, 2 programs read-only
    assert.equal(tx.keys[0], PLAYER);
    assert.ok(tx.keys.includes(rec["reference"]) && tx.keys.includes(rec["dest"]));
    assert.equal(tx.blockhash, BLOCKHASH);
    assert.deepEqual(tx.instructions.map((ix) => ix.program), [COMPUTE_BUDGET_PROGRAM_ID, COMPUTE_BUDGET_PROGRAM_ID,
      "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA"]);
    assert.deepEqual([...tx.instructions[0]!.data], [2, ...le(BigInt(COMPUTE_UNITS), 4)]);
    assert.deepEqual([...tx.instructions[1]!.data], [3, ...le(BigInt(expected), 8)]);
    assert.deepEqual([...tx.instructions[2]!.data], [12, ...le(4_993_742n, 8), 6]);
    assert.deepEqual(tx.instructions[2]!.accounts,
      [associatedTokenAddress(PLAYER, MAIN_MINT), MAIN_MINT, rec["dest"], PLAYER, rec["reference"]]);
  });
}

test("test_build_payment_rejects_a_bad_account", async () => {
  const rpc = new BuildRpc();
  await assert.rejects(new SolanaBackend().buildPayment(sessionRec(), { rpc, chain: MAIN, account: "0x1234" }), ValueError);
  assert.deepEqual(rpc.calls, []);
});

test("test_build_payment_normalizes_the_account", async () => {
  const rpc = new BuildRpc();
  const out = await new SolanaBackend().buildPayment(sessionRec(), { rpc, chain: MAIN, account: ` ${PLAYER}\n` });
  assert.equal(out["token_balance"], "7000000");
});
