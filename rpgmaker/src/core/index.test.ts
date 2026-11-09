import { test } from "node:test";
import assert from "node:assert/strict";
import * as core from "./index.ts";

test("test_public_surface_loads_and_exports_the_supported_api", () => {
  // Importing the barrel first (as the M2 bundle does) must survive the restore/reverify <-> backends cycle.
  for (const name of [
    "Engine", "Worker", "Job", "CancelledError", "CheckoutController", "RestoreController", "buildConfig", "buildChain",
    "addItem", "preparePayment", "MAINNET", "TESTNET", "getChains", "FetchTransport", "ConfigError", "ScfgError",
    "ValueError", "KeyError", "IndexError", "RestoreError", "RpcError", "RpcUnavailable", "TransportError",
    "unreadyKey", "parseUnreadyKey", "parseTxMessage", "asUnits", "decimalToUnits", "Dec",
  ]) {
    assert.ok((core as Record<string, unknown>)[name] !== undefined, name);
  }
  const config = core.buildConfig({ wallet: "0x52908400098527886E0F7030069857D2E4169EE7" });
  const worker = new core.Worker(new core.Engine(config, {}));
  assert.ok(worker.snapshot.unready instanceof Set);
});

test("test_unready_key_round_trips", () => {
  const key = core.unreadyKey("solana", "USDC");
  assert.equal(key, "solana\u0000USDC");
  assert.deepEqual(core.parseUnreadyKey(key), ["solana", "USDC"]);
  assert.throws(() => core.parseUnreadyKey("solana"), core.ValueError);
});
