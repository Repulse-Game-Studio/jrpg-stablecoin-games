/**
 * State behind the restore screen, independent of the engine UI (twin of scfg_core/restore_flow.py).
 *
 * input -> checking -> (choose ->) saving -> done
 */
import { normalizeAnyTx } from "./backends/index.ts";
import { ValueError } from "./errors.ts";
import { makeRestoreResult, type RestoreResult } from "./models.ts";
import { RestoreError, type RestoreCandidate } from "./restore.ts";
import type { Submit } from "./checkout.ts";
import type { Job } from "./worker.ts";
import { hasOwn } from "./pystr.ts";

export const MESSAGES: Record<string, string> = {
  invalid_hash: "That doesn't look like a transaction hash or signature.",
  tx_not_found: "Transaction not found. Check the hash, or try again later.",
  tx_reverted: "That transaction failed on-chain; nothing was paid.",
  tx_not_a_payment: "This transaction isn't a payment to this game.",
  network_unavailable: "Can't reach the network. Try again in a moment.",
  paste_blocked: "Your browser blocked pasting. Type the hash instead.",
  failed: "Something went wrong. Please try again.",
};


export class RestoreController {
  submit: Submit;
  state = "input";
  tx_input = "";
  error: string | null = null;
  candidates: RestoreCandidate[] = [];
  choices: Record<number, string> = {};
  added: Record<string, any>[] = [];
  _future: Job<any> | null = null;
  _phase: string | null = null;

  constructor({ submit }: { submit: Submit }) {
    this.submit = submit;
  }

  message(): string | null {
    return this.error && hasOwn(MESSAGES, this.error) ? MESSAGES[this.error]! : null;
  }

  pendingChoices(): number[] {
    const out: number[] = [];
    this.candidates.forEach((c, i) => {
      if (c.kind === null && !c.already_restored) out.push(i);
    });
    return out;
  }

  check(text?: string | null): boolean {
    let tx: string;
    try {
      tx = normalizeAnyTx(text === undefined || text === null ? this.tx_input : text);
    } catch (e) {
      if (!(e instanceof ValueError)) throw e;
      this.error = "invalid_hash";
      return false;
    }
    this.error = null;
    this.state = "checking";
    this._phase = "lookup";
    this._future = this.submit((e) => e.restoreLookup(tx));
    return true;
  }

  choose(index: number, choice: string): void {
    this.choices[index] = choice;
  }

  /** Every candidate that needs the player's choice has one. */
  get canConfirm(): boolean {
    return this.pendingChoices().every((i) => hasOwn(this.choices, String(i)));
  }

  confirm(): void {
    if ((this.state !== "checking" && this.state !== "choose") || !this.canConfirm) return;
    const cands = this.candidates.filter((c) => !c.already_restored);
    const indexMap = new Map<number, number>();
    cands.forEach((c, i) => indexMap.set(this.candidates.indexOf(c), i));
    const choices: Record<number, string> = {};
    for (const [k, v] of Object.entries(this.choices)) {
      const mapped = indexMap.get(Number(k));
      if (mapped !== undefined) choices[mapped] = v;
    }
    this.state = "saving";
    this._phase = "apply";
    this._future = this.submit((e) => e.restoreApply(cands, choices));
  }

  poll(): void {
    const fut = this._future;
    if (fut === null || !fut.done()) return;
    this._future = null;
    const exc = fut.exception();
    if (exc !== null) {
      this.state = "input";
      this.error = exc instanceof RestoreError ? exc.code : "failed";
      return;
    }
    if (this._phase === "lookup") {
      this.candidates = fut.result();
      if (this.candidates.every((c) => c.already_restored)) {
        this.state = "done";
      } else if (this.pendingChoices().length > 0) {
        this.state = "choose";
      } else {
        this.confirm();
      }
    } else {
      this.added = fut.result();
      this.state = "done";
    }
  }

  result(): RestoreResult {
    if (this.state === "done") {
      return makeRestoreResult({ status: this.added.length > 0 ? "restored" : "nothing_found", restored: [...this.added] });
    }
    return makeRestoreResult({ status: "cancelled", error: this.error });
  }
}
