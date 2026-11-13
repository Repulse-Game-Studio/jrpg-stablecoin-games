import { test } from "node:test";
import assert from "node:assert/strict";
import {
  TRANSFER_TOPIC, addressTopic, isChecksumAddress, normalizeAddress, parseTransferLog, toChecksumAddress,
} from "./evm.ts";
import { ValueError } from "./errors.ts";

const EIP55 = [
  "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed",
  "0xfB6916095ca1df60bB79Ce92cE3Ea74c37c5d359",
  "0xdbF03B407c01E7cD3CBea99509d93f8DDDC8C6FB",
  "0xD1220A0cf47c7B9Be7A2E6BA89F429762e7b9aDb",
];

test("test_transfer_topic", () => {
  assert.equal(TRANSFER_TOPIC, "0xddf252ad1be2c89b69c2b068fc378daa952ba7f163c4a11628f55a4df523b3ef");
});

test("test_checksum_from_lower_and_upper_body", () => {
  for (const a of EIP55) {
    assert.equal(toChecksumAddress(a.toLowerCase()), a);
    assert.equal(toChecksumAddress("0x" + a.slice(2).toUpperCase()), a);
  }
});

test("test_checksum_rejects_malformed", () => {
  for (const bad of ["", "0x", EIP55[0]!.slice(2), EIP55[0]!.slice(0, -1), EIP55[0] + "0", "0x" + "zz".repeat(20)]) {
    assert.throws(() => toChecksumAddress(bad), ValueError);
  }
  assert.throws(() => toChecksumAddress(42 as any), ValueError);
  assert.throws(() => toChecksumAddress(null as any), ValueError);
});

test("test_checksum_rejects_a_trailing_newline", () => {
  for (const bad of [EIP55[0] + "\n", EIP55[0]!.toLowerCase() + "\n", "\n" + EIP55[0], EIP55[0] + "\r\n"]) {
    assert.throws(() => toChecksumAddress(bad), ValueError, JSON.stringify(bad));
    assert.throws(() => normalizeAddress(bad), ValueError, JSON.stringify(bad));
    assert.equal(isChecksumAddress(bad), false);
  }
});

test("test_is_checksum_and_normalize", () => {
  assert.equal(isChecksumAddress(EIP55[0]!), true);
  assert.equal(isChecksumAddress(EIP55[0]!.toLowerCase()), false);
  assert.equal(isChecksumAddress("nope"), false);
  assert.equal(normalizeAddress(EIP55[1]!.toLowerCase()), EIP55[1]);
  const broken = "0x" + EIP55[0]!.slice(2).replace("A", "a");
  assert.throws(() => normalizeAddress(broken), ValueError);
});

test("test_address_topic", () => {
  assert.equal(addressTopic(EIP55[0]!), "0x" + "0".repeat(24) + EIP55[0]!.slice(2).toLowerCase());
});

function log(over: Record<string, unknown> = {}): Record<string, unknown> {
  return {
    address: "0xa0b86991c6218b36c1d19d4a2e9eb0ce3606eb48",
    topics: [TRANSFER_TOPIC, addressTopic(EIP55[0]!), addressTopic(EIP55[1]!)],
    data: "0x" + (4_990_000).toString(16).padStart(64, "0"),
    transactionHash: "0x" + "ab".repeat(32),
    logIndex: "0x7",
    blockNumber: "0x12d687",
    blockHash: "0x" + "CD".repeat(32),
    removed: false,
    ...over,
  };
}

test("test_parse_transfer_log", () => {
  const t = parseTransferLog(log())!;
  assert.deepEqual(t, {
    token: "0xA0b86991c6218b36c1d19D4a2e9Eb0cE3606eB48",
    sender: EIP55[0],
    recipient: EIP55[1],
    value: 4_990_000n,
    tx_hash: "0x" + "ab".repeat(32),
    log_index: 7,
    block_number: 1_234_567,
    block_hash: "0x" + "cd".repeat(32),
  });
});

test("test_parse_transfer_log_filters", () => {
  assert.equal(parseTransferLog(log({ removed: true })), null);
  assert.equal(parseTransferLog(log({ topics: [] })), null);
  assert.equal(parseTransferLog(log({ topics: [...(log().topics as string[]), "0x" + "00".repeat(32)] })), null);
  assert.equal(parseTransferLog(log({ topics: ["0x" + "11".repeat(32), "0x0", "0x0"] })), null);
});

test("test_parse_transfer_log_values", () => {
  assert.equal(parseTransferLog(log({ data: "0x" + "00".repeat(32) }))!.value, 0n);
  assert.equal(parseTransferLog(log({ data: "0x" + "ff".repeat(32) }))!.value, 2n ** 256n - 1n);
  assert.equal(parseTransferLog(log({ data: "0x" + "FF".repeat(32) }))!.value, 2n ** 256n - 1n);
  // anything but one 32-byte word ("0x" + 64 hex digits) is not a transfer, never an error
  for (const data of ["0x", "", null, undefined, 16, "0x01", "0xzz", "0x" + "00".repeat(31), "0x" + "00".repeat(33),
    "0x" + "00".repeat(31) + "0", "0X" + "00".repeat(32), "00".repeat(33), "0x" + "0_".repeat(32), "0x" + "00".repeat(31) + " 1"]) {
    assert.equal(parseTransferLog(log({ data })), null, JSON.stringify(data));
  }
  const noData = log();
  delete noData["data"];
  assert.equal(parseTransferLog(noData), null);
});

test("test_parse_transfer_log_rejects_missing_or_mistyped_fields", () => {
  for (const key of ["transactionHash", "blockHash"]) {
    assert.throws(() => parseTransferLog(log({ [key]: undefined })), TypeError, key);
    assert.throws(() => parseTransferLog(log({ [key]: null })), TypeError, key);
    assert.throws(() => parseTransferLog(log({ [key]: 5 })), TypeError, key);
  }
  assert.throws(() => parseTransferLog(log({ logIndex: 10 })), TypeError);
  assert.throws(() => parseTransferLog(log({ blockNumber: 12345 })), TypeError);
  assert.throws(() => parseTransferLog(log({ logIndex: "0x" + "f".repeat(14) })), ValueError);
});
