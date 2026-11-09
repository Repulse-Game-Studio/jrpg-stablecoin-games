/** EvmBackend and the backend registry. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { FakeChain } from "../../../test/chain_fakes.ts";
import { MAINNET } from "../chains.ts";
import { buildConfig } from "../config.ts";
import type { Transport } from "../coio.ts";
import { ValueError } from "../errors.ts";
import { RpcClient } from "../rpc.ts";
import { defaultEntropy } from "../sessions.ts";
import { EvmBackend, START_BLOCK_MARGIN } from "./evm.ts";
import { SolanaBackend } from "./solana.ts";
import { backendFor, backendForFamily, normalizeAnyTx } from "./index.ts";

const DEV = "0x52908400098527886E0F7030069857D2E4169EE7";
const TX = "0x" + "AB".repeat(32);

test("test_start_block_margin", () => {
  assert.equal(START_BLOCK_MARGIN, 5);
});

test("test_backend_for_family", () => {
  const evm = backendForFamily("evm");
  assert.ok(evm instanceof EvmBackend);
  assert.equal(evm.family, "evm");
  assert.equal(backendForFamily("evm"), evm); // one instance per family
  assert.equal(backendFor(MAINNET["base"]!), evm);
  assert.throws(() => backendForFamily("cosmos"), ValueError);
  const sol = backendForFamily("solana");
  assert.ok(sol instanceof SolanaBackend);
  assert.equal(sol.family, "solana");
  assert.equal(backendFor(MAINNET["solana"]!), sol);
  assert.equal(backendForFamily("solana"), sol);
  assert.throws(() => sol.normalizeTx(TX), ValueError);
});

test("test_evm_start_backs_off_the_head_and_floors_at_zero", async () => {
  const b = new EvmBackend();
  const chain = MAINNET["arbitrum"]!;
  const args = { chain, wallet: DEV, token: "USDC", entropy: defaultEntropy };
  assert.deepEqual(await b.start({ rpc: new FakeChain(1000), ...args }), { start_block: 995 });
  assert.deepEqual(await b.start({ rpc: new FakeChain(3), ...args }), { start_block: 0 });
});

test("test_evm_normalize_tx", () => {
  const b = new EvmBackend();
  assert.equal(b.normalizeTx(" " + TX), TX.toLowerCase());
  assert.throws(() => b.normalizeTx("not-a-hash"), ValueError);
  assert.equal(normalizeAnyTx(TX), TX.toLowerCase());
});

test("test_evm_make_rpc_uses_the_chain_urls_log_range_transport_and_clock", () => {
  const transport: Transport = { postJson: async () => ({}) };
  const clock = () => 7;
  const chain = MAINNET["polygon"]!;
  const rpc = new EvmBackend().makeRpc(chain, transport, clock);
  assert.ok(rpc instanceof RpcClient);
  assert.deepEqual([...rpc.urls], chain.rpc);
  assert.equal(rpc.logRange, chain.max_log_range);
  assert.equal(rpc.clock, clock);
});

test("test_evm_check_ready_says_every_token_is_ready_without_the_network", async () => {
  const b = new EvmBackend();
  for (const chain of [MAINNET["base"]!, MAINNET["arc"]!]) {
    const rpc = new FakeChain();
    const ready = await b.checkReady({ rpc, chain, wallet: DEV });
    assert.deepEqual(ready, Object.fromEntries(Object.keys(chain.tokens).map((n) => [n, true])));
    assert.deepEqual(rpc.calls, []);
  }
});

test("test_evm_reverify_and_lookup_delegate_to_the_modules", async () => {
  const b = new EvmBackend();
  const chain = MAINNET["base"]!;
  const rpc = new FakeChain(1000);
  assert.equal(await b.reverify({ chain: "base", tx_hash: TX, log_index: 0, block: 5, amount: "1.000000", token: "USDC" },
    { rpc, chain, wallets: [DEV] }), "drop"); // no matching log
  assert.equal(rpc.lastGetLogs![2], 5);
  const config = buildConfig({ wallet: DEV });
  assert.equal(await b.lookup(TX.toLowerCase(), { config, chain, rpc, receipts: {} }), null); // chain doesn't know it
});
