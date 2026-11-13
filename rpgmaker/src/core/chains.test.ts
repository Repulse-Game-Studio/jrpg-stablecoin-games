import { test } from "node:test";
import assert from "node:assert/strict";
import {
  ARC_MIN_FEE_WEI, ARC_SYSTEM_EMITTER, ARC_USDC, DISPLAY_ORDER, MAINNET, TESTNET, explorerUrl, getChains,
  logSource, makeToken, parseTransfer,
} from "./chains.ts";
import { ValueError } from "./errors.ts";
import { TRANSFER_TOPIC } from "./evm.ts";

const pad = (a: string) => "0x" + "0".repeat(24) + a.slice(2).toLowerCase();
const WALLET = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed";
function log(address: string, value: bigint) {
  return {
    address, topics: [TRANSFER_TOPIC, pad(WALLET), pad(WALLET)], data: "0x" + value.toString(16).padStart(64, "0"),
    transactionHash: "0x" + "ab".repeat(32), logIndex: "0x1", blockNumber: "0x10", blockHash: "0x" + "cd".repeat(32),
  };
}

test("test_constants", () => {
  assert.equal(ARC_USDC, "0x3600000000000000000000000000000000000000");
  assert.equal(ARC_SYSTEM_EMITTER, "0xffffFFFfFFffffffffffffffFfFFFfffFFFfFFfE");
  assert.equal(ARC_MIN_FEE_WEI, 20_000_000_000n);
  assert.deepEqual(DISPLAY_ORDER, ["arbitrum", "base", "arc", "polygon", "ethereum"]);
});

test("test_get_chains_returns_the_mutable_registries", () => {
  assert.equal(getChains(false), MAINNET);
  assert.equal(getChains(true), TESTNET);
  assert.equal(MAINNET["arc"]!.min_fee_wei, 20_000_000_000);
  const saved = MAINNET["base"]!;
  try {
    MAINNET["base"] = { ...saved, rpc: ["http://127.0.0.1:8545"] };
    assert.deepEqual(getChains(false)["base"]!.rpc, ["http://127.0.0.1:8545"]);
  } finally {
    MAINNET["base"] = saved;
  }
});

test("test_token_checksums_and_log_source", () => {
  const t = makeToken("0xa0b86991c6218b36c1d19d4a2e9eb0ce3606eb48");
  assert.equal(t.address, "0xA0b86991c6218b36c1d19D4a2e9Eb0cE3606eB48");
  assert.deepEqual(Object.keys(t).sort(), ["address", "decimals", "log_address", "log_decimals"]);
  assert.equal(logSource(t), t.address);
  assert.equal(logSource(MAINNET["arc"]!.tokens["USDC"]!), ARC_SYSTEM_EMITTER);
  assert.throws(() => makeToken(ARC_USDC, { log_address: ARC_SYSTEM_EMITTER, log_decimals: 3 }), ValueError);
  assert.throws(() => makeToken("nope"), ValueError);
});

test("test_explorer_url", () => {
  assert.equal(explorerUrl(MAINNET["base"]!, "0xabc"), "https://basescan.org/tx/0xabc");
  assert.equal(explorerUrl(TESTNET["solana"]!, "sig"), "https://solscan.io/tx/sig?cluster=devnet");
});

test("test_parse_transfer_plain_token", () => {
  const base = MAINNET["base"]!;
  const t = parseTransfer(base, log(base.tokens["USDC"]!.address, 4_993_742n))!;
  assert.equal(t.value, 4_993_742n);
  assert.equal(t.token, base.tokens["USDC"]!.address);
  assert.equal(parseTransfer(base, log(MAINNET["arbitrum"]!.tokens["USDC"]!.address, 1n)), null);
});

test("test_parse_transfer_arc_floors_18_to_6_with_bigint", () => {
  const arc = MAINNET["arc"]!;
  const big = 12_345_678_901_234_567_890_123n; // far above 2^63
  const t = parseTransfer(arc, log(ARC_SYSTEM_EMITTER, big))!;
  assert.equal(t.value, big / 10n ** 12n);
  assert.equal(t.token, ARC_USDC);
  assert.equal(parseTransfer(arc, log(ARC_SYSTEM_EMITTER, 999_999_999_999n)), null); // floors to 0
  assert.equal(parseTransfer(arc, log(ARC_USDC, 5_000_000n)), null); // not the log source
});
