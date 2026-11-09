/**
 * State behind the payment screen, independent of any engine (twin of scfg_core/checkout.py).
 *
 * The screen only displays this object's fields and calls its methods from buttons. All engine
 * work goes through `submit` (the Worker), and results come back through `poll()` (jobs) and
 * `onOutcome()` (watcher outcomes routed by session id).
 *
 * States: choose -> starting -> waiting -> confirming -> paid
 *                                      \-> underpaid | expired
 *
 * The sheet shows one step at a time (`step`):
 * amount (tips only) -> network -> method -> browser | qr | manual -> waiting -> done.
 * Steps with a single option are skipped both ways. `back()` works until a transaction
 * is seen; a seen payment moves the sheet to waiting and a final state to done.
 *
 * Stored fields keep the Python (snake_case) names; methods and computed properties are camelCase.
 */
import { asUnits, formatUnits, parseAmount } from "./amounts.ts";
import { backendFor } from "./backends/index.ts";
import { explorerUrl, type Chain } from "./chains.ts";
import { enabledChains, registry, textOn, walletFor, type Config } from "./config.ts";
import type { Dec } from "./decimal.ts";
import { parseUnreadyKey, unreadyKey, type Engine } from "./engine.ts";
import { ValueError } from "./errors.ts";
import type { PaymentResult } from "./models.ts";
import { makePaymentResult } from "./models.ts";
import { eip681TransferUri } from "./paylinks.ts";
import { MIN_TIP, TIP_PRESETS, type PaymentSpec } from "./payments.ts";
import { RpcUnavailable } from "./rpc.ts";
import { solanaPayUri } from "./solana/paylinks.ts";
import type { StepOutcome } from "./watcher.ts";
import type { Job } from "./worker.ts";
import { hasOwn } from "./pystr.ts";

export const FINAL_STATES: readonly string[] = ["paid", "underpaid", "expired"];
// Errors that judge the reported transaction (the hint) and drop it.
export const HINT_VERDICTS: readonly string[] = ["tx_reverted", "tx_not_a_payment", "tx_not_landed"];
// Order of the steps; the three method pages share a place.
export const STEP_ORDER: Readonly<Record<string, number>> = {
  amount: 0, network: 1, method: 2, browser: 3, qr: 3, manual: 3, waiting: 4, done: 5,
};

export const MESSAGES: Readonly<Record<string, string>> = {
  network_unavailable: "Can't reach the network. Retrying...",
  tx_reverted: "That transaction failed on-chain; no funds were sent.",
  tx_not_a_payment: "That transaction isn't a payment for this purchase.",
  tx_not_landed: "That transaction didn't reach the network. You can pay again.",
  invalid_hash: "That doesn't look like a transaction hash or signature.",
  invalid_amount: `Enter an amount of at least ${MIN_TIP} with up to 2 decimals.`,
  browser_unavailable: "Couldn't start the browser checkout. Use another method.",
  start_failed: "Couldn't start the payment. Check your connection and try again.",
  solana_unavailable: "Payments on Solana aren't available right now.",
  internal_error: "Something went wrong while checking the payment. Retrying...",
  paste_blocked: "Your browser blocked pasting. Type it instead: the game also finds the payment without it.",
};

/** What `server_factory` receives: the localhost page (desktop) or the web overlay (M2). */
export interface ServerArgs {
  /** What the page shows and needs to build the payment (amount, recipient, chain, token...). */
  session_info: Record<string, unknown>;
  /**
   * The page reported a sent transaction. The server must first run the page's message through
   * `parseTxMessage` and pass its results: `tx_hash` is then already normalised (lowercase 0x hash
   * or canonical base58 signature) and `sender` is a normalised address or null. A message that
   * `parseTxMessage` rejects (ValueError) must not reach on_tx. Call it from the game loop's turn
   * (like every controller method), never from inside a worker command.
   */
  on_tx: (tx_hash: string, sender?: string | null) => void;
  /** The current `statusInfo()` for the page's status poll. */
  get_status: () => Record<string, unknown>;
  /** Solana only: build the unsigned transaction for `account`; the server polls the Job. */
  build_tx?: (account: string) => Job<any>;
}

export interface CheckoutServer {
  start(): void;
  stop(): void;
  url: string;
  running: boolean;
  reopen?(): void;
  pump?(): void;
}

export type Submit = (fn: (e: Engine) => unknown) => Job<any>;

export interface CheckoutOptions {
  config: Config;
  spec: PaymentSpec;
  submit: Submit;
  mobile?: boolean;
  server_factory?: ((args: ServerArgs) => CheckoutServer) | null;
  /** `worker.snapshot.unready`: `unreadyKey(chain, token)` keys of routes that cannot take a payment. */
  unready?: (() => ReadonlySet<string>) | null;
  game_name?: string;
  browser_wallet?: ((chain: Chain) => boolean) | null;
}


export class CheckoutController {
  config: Config;
  spec: PaymentSpec;
  submit: Submit;
  mobile: boolean;
  server_factory: ((args: ServerArgs) => CheckoutServer) | null;
  unready: () => ReadonlySet<string>;
  browser_wallet: ((chain: Chain) => boolean) | null;
  game_name: string;
  amount: Dec;
  custom_amount = "";
  tx_input = "";
  chain_key: string | null = null;
  session: Record<string, any> | null = null;
  method: string | null = null;
  state = "choose";
  error: string | null = null;
  confirmations = 0;
  required = 0;
  hint_tx: string | null = null;
  dropped_tx: string | null = null; // the hint that never landed (tx_not_landed)
  result: PaymentResult | null = null;
  browser_failed = false;
  step: string;
  nav_dir = 1; // 1: the step came in going forward, -1: going back
  _future: Job<any> | null = null;
  _server: CheckoutServer | null = null;
  _picked_network = false; // move on from the network step once the session starts

  constructor(opts: CheckoutOptions) {
    const { config, spec, submit } = opts;
    this.config = config;
    this.spec = spec;
    this.submit = submit;
    this.mobile = opts.mobile ?? false;
    this.server_factory = opts.server_factory ?? null;
    this.unready = opts.unready ?? (() => new Set<string>()); // an empty set: nothing blocked
    this.browser_wallet = opts.browser_wallet ?? null;
    this.game_name = opts.game_name || "Game";
    this.amount = spec.price;
    this.step = spec.kind === "tip" ? "amount" : "network";
    if (this.step === "network") this._enterNetwork();
  }

  // --- read-only views ---------------------------------------------------------

  get sessionId(): string | null {
    return this.session ? this.session["id"] : null;
  }

  get chain(): Chain | null {
    if (!this.chain_key) return null;
    const chains = enabledChains(this.config);
    return hasOwn(chains, this.chain_key) ? chains[this.chain_key]! : null;
  }

  get family(): string | null {
    const chain = this.chain;
    return chain ? chain.family : null;
  }

  chainOptions(): [string, string, string][] {
    const chains = enabledChains(this.config);
    const blocked = this.unready();
    const out: [string, string, string][] = [];
    for (const k of this.spec.chains) {
      if (hasOwn(chains, k) && !blocked.has(unreadyKey(k, this.spec.token))) {
        out.push([k, chains[k]!.name, chains[k]!.fee_hint]);
      }
    }
    return out;
  }

  tipPresets(): readonly Dec[] {
    return TIP_PRESETS;
  }

  methods(): string[] {
    const m = this.config.methods;
    const out: string[] = [];
    if (m["browser"]!["enabled"] && !this.browser_failed && this._browserPossible()) out.push("browser");
    const qrChains: string[] | null | undefined = m["qr"]!["chains"];
    if (m["qr"]!["enabled"] && (qrChains === null || qrChains === undefined || qrChains.includes(this.chain_key as string))) {
      out.push("qr");
    }
    if (m["manual"]!["enabled"]) out.push("manual");
    return out;
  }

  /**
   * Desktop: a computer's browser can always open the page. Web build: only if a wallet that can
   * pay on the chosen network is in the page (phones included: a wallet app's own browser injects one).
   */
  _browserPossible(): boolean {
    if (this.browser_wallet !== null) {
      const chain = this.chain;
      return chain !== null && this.browser_wallet(chain);
    }
    return !this.mobile;
  }

  /** The exact amount to send, with the item code and nonce: '4.993742'. */
  get payAmountText(): string {
    return this.session ? formatUnits(asUnits(BigInt(this.session["units"]))) : `${this.amount}`;
  }

  /** The price as the player thinks of it: '4.99'. */
  get priceText(): string {
    return `${this.amount}`;
  }

  get tokenAddress(): string | null {
    const chain = this.chain;
    return chain ? chain.tokens[this.spec.token]!.address : null;
  }

  get recipient(): string {
    const chain = this.chain;
    if (chain !== null) return walletFor(this.config, chain) as string;
    return (this.config.wallet || this.config.solana_wallet) as string;
  }

  paymentUri(): string | null {
    if (!this.session) return null;
    const chain = this.chain;
    const units = asUnits(BigInt(this.session["units"]));
    if (this.family === "solana") {
      return solanaPayUri({
        recipient: this.recipient, units, mint: this.tokenAddress as string,
        reference: this.session["reference"], label: this.game_name, message: this.spec.title,
      });
    }
    return eip681TransferUri({
      token_address: this.tokenAddress as string, chain_id: chain!.chain_id, recipient: this.recipient, units,
    });
  }

  explorerUrl(): string | null {
    const tx = this.result && this.result.tx_hash ? this.result.tx_hash : this.hint_tx;
    const chain = this.chain;
    return tx && chain ? explorerUrl(chain, tx) : null;
  }

  message(): string | null {
    if (this.error) return hasOwn(MESSAGES, this.error) ? MESSAGES[this.error]! : null;
    if (this.state === "choose" && this.spec.chains.length > 0 && this.chainOptions().length === 0) {
      return MESSAGES["solana_unavailable"]!;
    }
    return null;
  }

  /** Shown in DEV MODE only: setup problems the developer must fix. */
  devWarnings(): string[] {
    const reg = registry(this.config);
    return [...this.unready()].sort().flatMap((entry) => {
      const [key, token] = parseUnreadyKey(entry);
      return hasOwn(reg, key) && reg[key]!.family === "solana"
        ? [`Your Solana wallet can't receive ${token} yet: receive any amount once.`] : [];
    });
  }

  /**
   * Status for the localhost page. `dropped_tx` names the transaction a `tx_not_landed` is about,
   * so the page offers Pay again only if it is the one the page sent.
   */
  statusInfo(): Record<string, unknown> {
    return {
      state: this.state, confirmations: this.confirmations, required: this.required,
      tx_hash: this.hint_tx, error: this.error, dropped_tx: this.dropped_tx,
    };
  }

  /** A transaction for this payment exists, so going back could start a second one. */
  get txSeen(): boolean {
    return Boolean(this.hint_tx) || this.state === "confirming";
  }

  /** The steps this payment walks through, without the single-option ones. */
  stepPath(): string[] {
    const path = this.spec.kind === "tip" ? ["amount"] : [];
    if (this.chainOptions().length !== 1) path.push("network");
    if (this.methods().length !== 1) path.push("method");
    return [...path, this.method || "browser", "waiting"];
  }

  /** (index of the current step, number of steps) for the progress dots. */
  stepDots(): [number, number] {
    const path = this.stepPath();
    const here = STEP_ORDER[this.step]!;
    return [path.filter((s) => STEP_ORDER[s]! < here).length, path.length];
  }

  get canGoBack(): boolean {
    if (this.step === "done" || this.txSeen) return false;
    return this._previousStep() !== null;
  }

  // --- actions -----------------------------------------------------------------

  selectChain(key: string): void {
    if (!this.spec.chains.includes(key)) {
      throw new ValueError(`chain ${JSON.stringify(key)} not available for this payment`);
    }
    this._discardSession();
    this.chain_key = key;
    this.state = "starting";
    this.error = null;
    const { spec, amount } = this;
    this._future = this.submit((e) => e.startSession(spec.kind, key, {
      item: spec.item, amount, token: spec.token, title: spec.title,
    }));
    const available = this.methods();
    if (this.method === null || !available.includes(this.method)) this.method = available.length > 0 ? available[0]! : null;
  }

  /** Network step: start a session on `key`, or keep the one already running. */
  chooseNetwork(key: string): void {
    this.step = "network";
    this.nav_dir = 1;
    if (key === this.chain_key && this.session && this.state === "waiting") {
      this._afterNetwork();
      return;
    }
    this._picked_network = true; // poll() moves on once the session has started
    if (key !== this.chain_key || this.state !== "starting") this.selectChain(key);
  }

  chooseMethod(name: string): void {
    if (this.methods().includes(name)) {
      this.method = name;
      this.error = null;
      this._go(name, 1);
    }
  }

  /** Leave the amount step (tips), applying a typed amount first. */
  nextStep(): boolean {
    if (this.step !== "amount") return false;
    if (this.custom_amount.trim() && !this.applyCustomAmount()) return false;
    this._enterNetwork();
    return true;
  }

  back(): void {
    if (this.canGoBack) {
      this.error = null;
      this._go(this._previousStep() as string, -1);
    }
  }

  /** The player says they sent the payment from the QR or manual page. */
  markSent(): void {
    if (this.step === "qr" || this.step === "manual") this._go("waiting", 1);
  }

  /** Browser page: start the localhost page and move to waiting; returns its URL. */
  openBrowser(): string | null {
    const url = this.startBrowser();
    if (url) {
      this._go("waiting", 1);
    } else if (this.browser_failed && this.step === "browser") {
      const methods = this.methods();
      this._go(methods.length !== 1 ? "method" : methods[0]!, -1);
    }
    return url;
  }

  setTipAmount(value: unknown): boolean {
    if (this.spec.kind !== "tip") return false;
    let amount: Dec;
    try {
      amount = parseAmount(value);
    } catch (e) {
      if (e instanceof ValueError || e instanceof TypeError) {
        this.error = "invalid_amount";
        return false;
      }
      throw e;
    }
    if (amount.cmp(MIN_TIP) < 0) {
      this.error = "invalid_amount";
      return false;
    }
    this.error = null;
    if (amount.cmp(this.amount) === 0 && this.chain_key) return true; // same amount: keep the session already started
    this.amount = amount;
    if (this.chain_key) this.selectChain(this.chain_key);
    return true;
  }

  applyCustomAmount(): boolean {
    return this.setTipAmount(this.custom_amount.trim());
  }

  submitHash(text?: string | null): boolean {
    const chain = this.chain;
    if (!this.session || chain === null) return false;
    let tx: string;
    try {
      tx = backendFor(chain).normalizeTx(text === undefined || text === null ? this.tx_input : text);
    } catch (e) {
      if (e instanceof ValueError) {
        this.error = "invalid_hash";
        return false;
      }
      throw e;
    }
    this._reportHint(tx);
    this.tx_input = "";
    return true;
  }

  _reportHint(tx_hash: string, _sender: string | null = null): void {
    const sid = this.sessionId;
    this.hint_tx = tx_hash;
    this.error = null;
    this.dropped_tx = null;
    this.submit((e) => e.reportHint(sid as string, tx_hash));
    if (this.step !== "done") this._go("waiting", 1);
  }

  /** Start the localhost page for this session; returns its URL or null. */
  startBrowser(): string | null {
    if (!this.session || this.server_factory === null) return null;
    if (this._server === null) {
      const chain = this.chain as Chain;
      const info: Record<string, unknown> = {
        title: this.spec.title, kind: this.spec.kind, token: this.spec.token,
        family: chain.family, decimals: 6,
        amount_units: String(this.session["units"]), amount_text: this.payAmountText,
        price_text: this.priceText,
        recipient: this.recipient, chain_name: chain.name,
        native_symbol: chain.native_symbol,
        explorer_tx: chain.explorer_tx, testnet: this.config.testnet,
        logo: this.config.logo !== null, accent: this.config.accent,
        on_accent: textOn(this.config.accent),
      };
      if (chain.family === "solana") {
        Object.assign(info, { wallet_chain: chain.wallet_chain, mint: this.tokenAddress, reference: this.session["reference"] });
      } else {
        Object.assign(info, {
          chain_id: chain.chain_id, chain_id_hex: "0x" + chain.chain_id.toString(16),
          rpc_url: chain.rpc[chain.rpc.length - 1], token_address: this.tokenAddress,
          // Arc: the token is also the gas coin, with a fee floor.
          gas_is_token: chain.native_token === this.spec.token,
          min_fee_wei: String(chain.min_fee_wei),
        });
      }
      const args: ServerArgs = {
        session_info: info, on_tx: (tx, sender) => this._reportHint(tx, sender), get_status: () => this.statusInfo(),
      };
      if (chain.family === "solana") args.build_tx = (account) => this._submitBuild(account);
      try {
        this._server = this.server_factory(args);
        this._server.start();
      } catch {
        this._server = null;
        this.browser_failed = true;
        this.error = "browser_unavailable";
        if (this.method === "browser") {
          const methods = this.methods();
          this.method = methods.length > 0 ? methods[0]! : null;
        }
        return null;
      }
    } else if (this._server.reopen) {
      this._server.reopen();
    }
    return this._server.url;
  }

  /**
   * Ask the Worker for the unsigned Solana transfer; returns its Job. The desktop page's server
   * waits on it, the web overlay polls it.
   */
  _submitBuild(account: string): Job<any> {
    const sid = this.sessionId;
    return this.submit((e) => e.buildSolanaPayment(sid as string, account));
  }

  cancel(): PaymentResult {
    const sid = this.sessionId;
    if (sid && !FINAL_STATES.includes(this.state)) this.submit((e) => e.setUiStatus(sid, "cancelled"));
    this.close();
    this.result = this.result || this._result("cancelled");
    return this.result;
  }

  keepPlaying(): PaymentResult {
    const sid = this.sessionId;
    if (sid) this.submit((e) => e.setUiStatus(sid, "pending"));
    return this._result("pending");
  }

  /** Close after a final state (paid/underpaid/expired). */
  finish(): PaymentResult {
    this.close();
    return this.result || this._result("cancelled");
  }

  close(): void {
    this._abandonStart();
    if (this._server !== null) {
      this._server.stop();
      this._server = null;
    }
  }

  // --- updates -------------------------------------------------------------

  /** Pick up the result of startSession (call often from the UI). */
  poll(): void {
    if (this._server !== null && this._server.pump) this._server.pump();
    const fut = this._future;
    if (fut === null || !fut.done()) return;
    this._future = null;
    const exc = fut.exception();
    if (exc !== null) {
      this.state = "choose";
      this.chain_key = null;
      this.error = exc instanceof RpcUnavailable ? "network_unavailable" : "start_failed";
      if (this.step !== "amount" && this.step !== "network") this._go("network", -1);
      return;
    }
    this.session = fut.result();
    this.state = "waiting";
    const chain = this.chain;
    this.required = chain ? chain.confirmations : 0;
    if (this.step === "network" && this._picked_network) this._afterNetwork();
  }

  onOutcome(outcome: StepOutcome): void {
    if (!this.session || outcome.session_id !== this.sessionId) return;
    if (outcome.error !== null && HINT_VERDICTS.includes(outcome.error)) {
      this._hintVerdict(outcome);
    } else if (outcome.error) {
      this.error = outcome.error;
    }
    const status = outcome.status;
    if (status === "confirming") {
      this.state = "confirming";
      this.confirmations = outcome.confirmations;
      this.required = outcome.required;
      if (outcome.error === null) this.error = null;
    } else if (status === "reorged") {
      this.state = "waiting";
      this.confirmations = 0;
    } else if (status === "paid" || status === "underpaid") {
      this.state = status;
      this.result = outcome.result;
      this.error = null;
    } else if (status === "expired" && this.state !== "paid" && this.state !== "underpaid") {
      this.state = "expired";
      this.result = outcome.result;
    }
    if (FINAL_STATES.includes(this.state)) {
      if (this.step !== "done") this._go("done", 1);
    } else if (this.txSeen && this.step !== "waiting") {
      this._go("waiting", 1);
    }
  }

  // --- internals -----------------------------------------------------------

  _go(step: string, direction: number): void {
    this.step = step;
    this.nav_dir = direction;
    this._picked_network = false;
  }

  _previousStep(): string | null {
    const here = STEP_ORDER[this.step]!;
    const earlier = this.stepPath().filter((s) => STEP_ORDER[s]! < here);
    return earlier.length > 0 ? earlier[earlier.length - 1]! : null;
  }

  /** Go forward to the network step, or past it when there is only one network. */
  _enterNetwork(): void {
    const options = this.chainOptions();
    if (options.length === 1) this.chooseNetwork(options[0]![0]);
    else this._go("network", 1);
  }

  /** The session is running: go to the method step, or straight to the only method. */
  _afterNetwork(): void {
    const methods = this.methods();
    if (this.method === null || !methods.includes(this.method)) this.method = methods.length > 0 ? methods[0]! : null;
    this._go(methods.length === 1 ? (this.method as string) : "method", 1);
  }

  _hintVerdict(outcome: StepOutcome): void {
    const about = outcome.hint_tx;
    if (about && this.hint_tx && about !== this.hint_tx) return; // judged an older hint; the newer one is still pending
    this.error = outcome.error;
    this.dropped_tx = outcome.error === "tx_not_landed" ? (about || this.hint_tx) : null;
    this.hint_tx = null;
  }

  /**
   * Stop waiting for a session start the player moved away from, and discard that session once it
   * exists: it would be watched for 24 h and reported expired.
   */
  _abandonStart(): void {
    const fut = this._future;
    this._future = null;
    if (fut === null) return;
    const submit = this.submit;
    const discard = (): void => {
      if (fut.cancelled() || fut.exception() !== null) return;
      const sid = fut.result()["id"];
      submit((e) => e.discardSession(sid));
    };
    // Future.add_done_callback: runs at once when already done, else when the job settles.
    if (fut.done()) discard();
    else fut.promise.then(discard, discard);
  }

  _discardSession(): void {
    if (this.session && (this.state === "waiting" || this.state === "starting")) {
      const sid = this.sessionId as string;
      this.submit((e) => e.discardSession(sid));
    }
    this.close();
    this.session = null;
    this.hint_tx = null;
    this.dropped_tx = null;
    this.confirmations = 0;
  }

  _result(status: string): PaymentResult {
    return makePaymentResult({
      status, kind: this.spec.kind, item: this.spec.item, token: this.spec.token,
      chain: this.chain_key, session_id: this.sessionId,
    });
  }
}
