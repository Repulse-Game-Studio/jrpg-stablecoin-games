import { test } from "node:test";
import assert from "node:assert/strict";
import { RpcClient, RpcError, RpcUnavailable } from "../rpc.ts";
import type { Handler } from "../../../test/rpc_fakes.ts";
import { FakeTransport, down, err, ok } from "../../../test/rpc_fakes.ts";
import { SolanaRpc, TX_VERSION } from "./rpc.ts";
import { KeyError } from "../errors.ts";

const A = "https://a.example";
const B = "https://b.example";

function client(handlers: Record<string, Handler>) {
  const t = new FakeTransport(handlers);
  const c = new RpcClient(Object.keys(handlers), { transport: t, clock: () => 0.0 });
  return { rpc: new SolanaRpc(c), t };
}

test("test_get_transaction_asks_for_newest_version_and_json", async () => {
  const { rpc, t } = client({ [A]: () => ok({ slot: 5 }) });
  assert.deepEqual(await rpc.getTransaction("SIG"), { slot: 5 });
  assert.deepEqual(t.calls[0]!.slice(1), ["getTransaction", ["SIG", {
    encoding: "json", commitment: "confirmed", maxSupportedTransactionVersion: TX_VERSION }]]);
});

test("test_get_transaction_lowers_version_when_the_node_rejects_the_parameter", async () => {
  const handler: Handler = (_m, p) => {
    if (p[1].maxSupportedTransactionVersion === 1) return err(-32602, "Invalid params: unknown maxSupportedTransactionVersion 1");
    return ok({ slot: 5 });
  };
  const { rpc } = client({ [A]: handler });
  assert.deepEqual(await rpc.getTransaction("SIG"), { slot: 5 });
  assert.equal(rpc.tx_version, 0);
});

test("test_try_at_each_live_url_reports_per_endpoint_and_dead_ones_cool_down", async () => {
  const { rpc } = client({ [A]: down, [B]: () => err(-1, "boom") });
  const c = rpc.client;
  const out: Array<[string, any]> = [];
  for (const url of c.liveUrls()) {
    const res = await c.tryAt(url, "getSlot", []);
    if (res !== null) out.push([url, res]);
  }
  assert.equal(out.length, 1);
  assert.equal(out[0]![0], B);
  const [kind, value] = out[0]![1];
  assert.ok(kind === "error" && value instanceof RpcError);
  assert.deepEqual(c.liveUrls(), [B]);
});

function history(firstA: number, firstB: number, txB: unknown = null): Record<string, Handler> {
  const make = (first: number, tx: unknown): Handler => (m) => (m === "getFirstAvailableBlock" ? ok(first) : ok(tx));
  return { [A]: make(firstA, null), [B]: make(firstB, txB) };
}

test("test_get_transaction_any_returns_first_found", async () => {
  const { rpc } = client(history(10 ** 9, 0, { slot: 50 }));
  assert.deepEqual(await rpc.getTransactionAny("SIG", { min_slot: 50 }), [{ slot: 50 }, false]);
});

test("test_null_from_full_history_endpoint_is_authoritative", async () => {
  const { rpc } = client(history(10 ** 9, 0));
  assert.deepEqual(await rpc.getTransactionAny("SIG", { min_slot: 50 }), [null, true]);
});

test("test_null_only_from_pruned_endpoints_is_not_authoritative", async () => {
  const { rpc } = client(history(10 ** 9, 10 ** 9));
  assert.deepEqual(await rpc.getTransactionAny("SIG", { min_slot: 50 }), [null, false]);
});

test("test_unreadable_first_available_block_counts_as_pruned", async () => {
  // Python: int("abc") raises ValueError inside _first_available → _UNKNOWN_HISTORY (not cached).
  const garbage: Handler = (m) => (m === "getFirstAvailableBlock" ? ok("abc") : ok(null));
  const { rpc } = client({ [A]: garbage, [B]: garbage });
  assert.deepEqual(await rpc.getTransactionAny("SIG", { min_slot: 50 }), [null, false]);
});

test("test_get_transaction_any_without_answers_raises", async () => {
  const { rpc } = client({ [A]: down, [B]: down });
  await assert.rejects(rpc.getTransactionAny("SIG", { min_slot: 1 }), RpcUnavailable);
});

test("test_get_transaction_any_default_commitment_is_finalized", async () => {
  const { rpc, t } = client({ [A]: () => ok({ slot: 1 }) });
  await rpc.getTransactionAny("SIG");
  assert.equal(t.calls[0]![2][1].commitment, "finalized");
});

test("test_small_helpers", async () => {
  const handler: Handler = (m) => ({
    getSlot: ok(77),
    getSignaturesForAddress: ok([{ signature: "S2" }, { signature: "S1" }]),
    getSignatureStatuses: ok({ value: [null, { confirmationStatus: "finalized" }] }),
    getAccountInfo: ok({ value: null }),
    getLatestBlockhash: ok({ value: { blockhash: "BH", lastValidBlockHeight: 9 } }),
    getBalance: ok({ value: 1234 }),
    getRecentPrioritizationFees: ok([9, 1, 5].map((f) => ({ slot: 1, prioritizationFee: f }))),
  } as Record<string, unknown>)[m];
  const { rpc, t } = client({ [A]: handler });
  assert.equal(await rpc.getSlot(), 77);
  assert.deepEqual((await rpc.getSignaturesForAddress("X", { until: "S0" })).map((s: any) => s.signature), ["S2", "S1"]);
  assert.deepEqual(t.calls[t.calls.length - 1]![2], ["X", { limit: 100, commitment: "confirmed", until: "S0" }]);
  assert.equal((await rpc.getSignatureStatuses(["a", "b"]))[1].confirmationStatus, "finalized");
  assert.deepEqual(t.calls[t.calls.length - 1]![2][1], { searchTransactionHistory: true });
  assert.equal(await rpc.accountExists("X"), false);
  assert.equal(await rpc.getTokenBalance("X"), 0n);
  assert.equal(await rpc.getLatestBlockhash(), "BH");
  assert.equal(await rpc.getBalance("X"), 1234n);
  assert.equal(await rpc.getPriorityFee(["X"]), 5);
});

test("test_signature_options_before_and_empty_result", async () => {
  const { rpc, t } = client({ [A]: () => ok(null) });
  assert.deepEqual(await rpc.getSignaturesForAddress("X", { before: "B1", limit: 5, commitment: "finalized" }), []);
  assert.deepEqual(t.calls[0]![2], ["X", { limit: 5, commitment: "finalized", before: "B1" }]);
});

test("test_empty_dict_results_read_as_empty_lists_like_python_or", async () => {
  // Python `res or []`: an empty dict (or "") is falsy there, truthy in JS
  for (const empty of [{}, "", 0, false]) {
    const { rpc } = client({ [A]: () => ok(empty) });
    assert.deepEqual(await rpc.getSignaturesForAddress("X"), []);
    assert.equal(await rpc.getPriorityFee(["X"]), 0);
  }
});

test("test_token_balance_reads_parsed_amount", async () => {
  const { rpc } = client({ [A]: () => ok({ value: { data: { parsed: { info: { tokenAmount: { amount: "18446744073709551615" } } } } } }) });
  assert.equal(await rpc.getTokenBalance("T"), 18446744073709551615n);
});

const V2 = "Transaction version (2) is not supported by the requesting client. Please try the "
  + "request again with the following configuration parameter: \"maxSupportedTransactionVersion\": 2";

function versioned(needs: number): Handler {
  return (m, p) => {
    if (m === "getFirstAvailableBlock") return ok(0);
    if (p[1].maxSupportedTransactionVersion < needs) {
      return err(-32015, V2.replace("(2)", `(${needs})`).replace(": 2", `: ${needs}`));
    }
    return ok({ slot: 5 });
  };
}

test("test_get_transaction_retries_once_with_the_version_the_node_names", async () => {
  const { rpc, t } = client({ [A]: versioned(2) });
  assert.deepEqual(await rpc.getTransaction("SIG"), { slot: 5 });
  assert.deepEqual(t.calls.map((c) => c[2][1].maxSupportedTransactionVersion), [TX_VERSION, 2]);
  assert.equal(rpc.tx_version, TX_VERSION);
});

test("test_unsupported_version_error_naming_an_older_version_is_raised", async () => {
  const { rpc, t } = client({ [A]: () => err(-32015, V2.replace("(2)", "(0)")) });
  await assert.rejects(rpc.getTransaction("SIG"), RpcError);
  assert.equal(t.calls.length, 1);
});

// tests/unit/test_solana_rpc.py::test_named_version_ignores_numbers_beyond_18_digits:
// a version beyond 18 significant digits is no match; the match is Python's
// re.search(r"version \((\d+)\)", message, re.IGNORECASE): \d is any Unicode decimal digit,
// and IGNORECASE also matches \u017f for s and \u0130, \u0131 for i.
test("test_named_version_ignores_numbers_beyond_18_digits", () => {
  const rows: [string, number | null][] = [
    ["Transaction version (" + "9".repeat(19) + ") is not supported", null],
    ["Transaction version (" + "9".repeat(4301) + ") is not supported", null],
    ["Transaction version (" + "9".repeat(1_000_000) + ") is not supported", null],
    ["Transaction version (" + "9".repeat(15) + ") is not supported", 999_999_999_999_999],
    ["Transaction version (" + "0".repeat(30) + "3) is not supported", 3],
    ["version (" + "9".repeat(19) + ") version (5)", null],
    ["version (12 version (5)", 5],
    ["version (\u0663)", 3],
    ["Transaction VER\u017fION (2) x", 2],
    ["vers\u0131on (4)", 4],
    ["vers\u0130on (4)", 4],
    ["Version (2", null],
    ["version(2)", null],
    ["version (x) version (6)", 6],
    ["version (1)", null],
    ["version (\u{11f53})", null], // a Kawi digit (Unicode 15): not a digit in Python 3.9
  ];
  for (const [message, expected] of rows) {
    const { rpc } = client({ [A]: () => ok(null) });
    assert.equal((rpc as any)._namedVersion(new RpcError(-32015, message)), expected, message.slice(0, 40));
  }
  assert.equal((client({ [A]: () => ok(null) }).rpc as any)._namedVersion(new RpcError(-32000, "version (5)")), null, "only for -32015");
});

test("test_an_overlong_named_version_raises_the_node_error", async () => {
  const { rpc, t } = client({ [A]: () => err(-32015, V2.replace("(2)", "(" + "9".repeat(4301) + ")")) });
  await assert.rejects(rpc.getTransaction("SIG"), RpcError);
  assert.equal(t.calls.length, 1);
});

test("test_get_transaction_any_retries_with_the_named_version", async () => {
  const { rpc } = client({ [A]: versioned(3) });
  assert.deepEqual(await rpc.getTransactionAny("SIG", { min_slot: 1 }), [{ slot: 5 }, false]);
});

// --- malformed replies raise where Python raises (readiness_crash sends getAccountInfo result {}) ---

const isKeyOrType = (e: any) => e instanceof KeyError || e instanceof TypeError;
const malformed: Array<[string, unknown, (r: SolanaRpc) => Promise<unknown>]> = [
  ["account_exists_missing_value", {}, (r) => r.accountExists("X")],
  ["account_exists_non_dict", null, (r) => r.accountExists("X")],
  ["signature_statuses_missing_value", {}, (r) => r.getSignatureStatuses(["a"])],
  ["latest_blockhash_missing_value", {}, (r) => r.getLatestBlockhash()],
  ["latest_blockhash_missing_blockhash", { value: {} }, (r) => r.getLatestBlockhash()],
  ["token_balance_missing_value", {}, (r) => r.getTokenBalance("X")],
  ["token_balance_missing_nested", { value: { data: {} } }, (r) => r.getTokenBalance("X")],
  ["token_balance_non_object_nested", { value: { data: "x" } }, (r) => r.getTokenBalance("X")],
  ["balance_missing_value", {}, (r) => r.getBalance("X")],
  ["priority_fee_missing_field", [{ slot: 1 }], (r) => r.getPriorityFee(["X"])],
];
for (const [name, body, run] of malformed) {
  test(`test_malformed_reply_raises[${name}]`, async () => {
    const { rpc } = client({ [A]: () => ok(body) });
    await assert.rejects(run(rpc), isKeyOrType);
  });
}
