import { test } from "node:test";
import assert from "node:assert/strict";
import { BUILD_TIMEOUT_S, parseBuildMessage, parseTxMessage } from "./page_messages.ts";
import { normalizeAnyTx } from "./backends/index.ts";
import { normalizeTxHash } from "./sessions.ts";
import { ValueError } from "./errors.ts";

const EVM_TX = "0x" + "ab".repeat(32);
const SOL_ACCOUNT = "CejwupnRUhnYHb72dZ8rqnWsvFw49MDYKZ2SvurkX4K3";
const SOL_SIG = "1GMkH3brNXiNNs1tiFZHu4yZSRrzJwxi5wB9bHFtMinfCXNnR1adh8Vo8NTheK4evneedH4qmvjeqcBBNAefgS";

test("test_tx_message_with_a_checksummed_sender", () => {
  const [tx, sender] = parseTxMessage({ tx_hash: "0x" + "AB".repeat(32), from: "0x5aaeb6053f3e94c9b9a09f33669435e7ef1beaed" });
  assert.equal(tx, EVM_TX);
  assert.equal(sender, "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed");
});

test("test_a_bad_sender_is_dropped_not_fatal", () => {
  for (const from of ["nonsense", 5, [], undefined, null, "", true, "0x1234", {}, [1]]) {
    assert.deepEqual(parseTxMessage(from === undefined ? { tx_hash: EVM_TX } : { tx_hash: EVM_TX, from }), [EVM_TX, null], String(from));
  }
  assert.deepEqual(parseTxMessage({ tx_hash: EVM_TX, from: SOL_ACCOUNT }), [EVM_TX, SOL_ACCOUNT]);
  assert.deepEqual(parseTxMessage({ tx_hash: EVM_TX, from: ` ${SOL_ACCOUNT}\n` }), [EVM_TX, SOL_ACCOUNT]);
});

test("test_bad_tx_messages_raise", () => {
  for (const data of [null, [], "x", {}, 5, true, { tx_hash: 5 }, { tx_hash: "0x12" }, { tx_hash: null }, { tx_hash: [EVM_TX] }, [{ tx_hash: EVM_TX }]]) {
    assert.throws(() => parseTxMessage(data), ValueError, JSON.stringify(data));
  }
});

test("test_build_message", () => {
  assert.equal(parseBuildMessage({ account: SOL_ACCOUNT }), SOL_ACCOUNT);
  assert.equal(parseBuildMessage({ account: ` ${SOL_ACCOUNT}\n` }), SOL_ACCOUNT);
  for (const data of [null, {}, { account: 5 }, { account: "0xabc" }, [], "x", { account: null }, { account: true }]) {
    assert.throws(() => parseBuildMessage(data), ValueError, JSON.stringify(data));
  }
  assert.equal(BUILD_TIMEOUT_S, 15);
});

test("test_normalize_tx_hash", () => {
  assert.equal(normalizeTxHash("  " + EVM_TX.toUpperCase().replace("0X", "0x") + "\n"), EVM_TX);
  for (const bad of ["0X" + "ab".repeat(32), EVM_TX.slice(2), "0x" + "ab".repeat(31), "", "0x" + "１".repeat(64), EVM_TX + "\n" + EVM_TX, 5, null, [EVM_TX]]) {
    assert.throws(() => normalizeTxHash(bad as any), ValueError);
  }
});

test("test_normalize_any_tx_tries_evm_then_solana", () => {
  assert.equal(normalizeAnyTx(" " + EVM_TX + "　"), EVM_TX);
  assert.equal(normalizeAnyTx("  " + SOL_SIG + "\n"), SOL_SIG);
  for (const bad of ["garbage", "", 5, null, true, [], {}, SOL_SIG.slice(1), EVM_TX.slice(2)]) {
    assert.throws(() => normalizeAnyTx(bad as any), ValueError);
  }
});
