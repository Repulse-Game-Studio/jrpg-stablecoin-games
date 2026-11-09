/** Twin of tests/unit/test_evm_logs.py (by name; evm.test.ts covers the same ground from Task 2). */
import { test } from "node:test";
import assert from "node:assert/strict";
import { TRANSFER_TOPIC, addressTopic, parseTransferLog } from "./evm.ts";

const USDC = "0xaf88d065e77c8cC2239327C5EDb3A432268e5831";
const PAYER = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed";
const DEV = "0x52908400098527886E0F7030069857D2E4169EE7";

function makeLog(overrides: Record<string, unknown> = {}): Record<string, any> {
  return {
    address: USDC.toLowerCase(),
    topics: [TRANSFER_TOPIC, addressTopic(PAYER), addressTopic(DEV)],
    data: "0x" + (4_993_742).toString(16).padStart(64, "0"),
    transactionHash: "0xABCDEF" + "0".repeat(58),
    logIndex: "0xc",
    blockNumber: "0xffb2a46",
    blockHash: "0x" + "11".repeat(32),
    removed: false,
    ...overrides,
  };
}

test("test_parses_erc20_transfer", () => {
  const t = parseTransferLog(makeLog())!;
  assert.equal(t.token, USDC);
  assert.equal(t.sender, PAYER);
  assert.equal(t.recipient, DEV);
  assert.equal(t.value, 4_993_742n);
  assert.equal(t.tx_hash, "0xabcdef" + "0".repeat(58));
  assert.equal(t.log_index, 12);
  assert.equal(t.block_number, 0xffb2a46);
  assert.equal(t.block_hash, "0x" + "11".repeat(32));
});

test("test_topic0_comparison_is_case_insensitive", () => {
  const upper = TRANSFER_TOPIC.toUpperCase().replace("0X", "0x");
  assert.notEqual(parseTransferLog(makeLog({ topics: [upper, addressTopic(PAYER), addressTopic(DEV)] })), null);
});

test("test_ignores_other_events", () => {
  assert.equal(parseTransferLog(makeLog({ topics: ["0x" + "ab".repeat(32), addressTopic(PAYER), addressTopic(DEV)] })), null);
});

test("test_ignores_erc721_transfer_with_four_topics", () => {
  const log = makeLog({ topics: [TRANSFER_TOPIC, addressTopic(PAYER), addressTopic(DEV), "0x" + "00".repeat(31) + "07"] });
  assert.equal(parseTransferLog(log), null);
});

test("test_ignores_removed_logs", () => {
  assert.equal(parseTransferLog(makeLog({ removed: true })), null);
});

test("test_missing_removed_field_is_fine", () => {
  const log = makeLog();
  delete log["removed"];
  assert.notEqual(parseTransferLog(log), null);
});

test("test_empty_data_means_zero_value", () => {
  assert.equal(parseTransferLog(makeLog({ data: "0x" }))!.value, 0n);
});
