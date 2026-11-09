/**
 * The supported surface of the payment core, for the RPG Maker MZ glue (M2) and its bundle.
 * Anything not exported here is internal and may change without notice; tests import modules
 * directly. No Python twin (Python callers import `scfg_core` submodules).
 */

// Engine and the single-writer worker
export {
  Engine, parseUnreadyKey, unreadyKey,
  type EngineOptions, type Snapshot, type StartSessionOptions,
} from "./engine.ts";
export { CancelledError, Job, Worker, type WorkerOptions } from "./worker.ts";

// Screens' state
export {
  CheckoutController, FINAL_STATES, HINT_VERDICTS, MESSAGES as CHECKOUT_MESSAGES, STEP_ORDER,
  type CheckoutOptions, type CheckoutServer, type ServerArgs, type Submit,
} from "./checkout.ts";
export { MESSAGES as RESTORE_MESSAGES, RestoreController } from "./restore_flow.ts";

// Configuration
export {
  addItem, buildChain, buildConfig, enabledChains, logoContentType,
  type AddItemOptions, type BuildChainOptions, type BuildConfigOptions, type Config,
} from "./config.ts";
export { MIN_TIP, TIP_PRESETS, preparePayment, type PaymentSpec } from "./payments.ts";
export { DISPLAY_ORDER, MAINNET, TESTNET, explorerUrl, getChains, type Chain, type Token } from "./chains.ts";

// Network (backendFor(chain).makeRpc is how a custom rpc_factory builds clients, e.g. with a monotonic clock)
export { backendFor } from "./backends/index.ts";
export type { Backend } from "./backends/base.ts";
export { FetchTransport, TransportError, type Transport } from "./coio.ts";
export { RpcError, RpcUnavailable } from "./rpc.ts";

// Errors
export { ConfigError, IndexError, KeyError, ScfgError, ValueError, isScfgError } from "./errors.ts";
export { RestoreError, TIP_CHOICE, type RestoreCandidate } from "./restore.ts";

// What the page tells the game (the M2 server parses with these)
export { BUILD_TIMEOUT_S, parseBuildMessage, parseTxMessage } from "./page_messages.ts";

// Values, models and persisted records
export { Dec } from "./decimal.ts";
export {
  TOKEN_DECIMALS, asUnits, decimalToUnits, formatUnits, parseAmount, unitsToDecimal, type Units,
} from "./amounts.ts";
export {
  makePaymentResult, makeRestoreResult, type Item, type PaymentResult, type RestoreResult,
} from "./models.ts";
export { computeOwnership, type ReceiptJson } from "./receipts.ts";
export { type Entropy, type SessionRecord, type SessionStore } from "./sessions.ts";
export type { StepOutcome } from "./watcher.ts";
export type { Transfer } from "./transfers.ts";
export type { RawTransferLog, TransferLog } from "./evm.ts";
export { qrRects } from "./paylinks.ts";
