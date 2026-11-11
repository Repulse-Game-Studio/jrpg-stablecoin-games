/**
 * The restore screen: the RPG Maker twin of the `scfg_restore` screen in
 * plugin/game/scfg/scfg_screens.rpy (and of Godot's ui/restore.gd), shown by the default sheet
 * host for `await SCFG.restore()`.
 *
 * By the RestoreController's state: input / checking (the hash field, Paste on the web, Check),
 * choose (each ambiguous payment with its choices, then Restore), saving, done (what was
 * restored); then any message and the purchases on this device, each with Copy hash. Close is
 * pinned at the bottom and closes with "close"; so does cancel. The screen only reads the
 * controller and calls check / choose / confirm; SCFG polls it.
 */
import { formatUnits, TIP_CHOICE, type RestoreController } from "../../core/index.ts";
import { PASTE_MAX, TX_CHARS } from "../actions.ts";
import { Screen, type ScreenEnv } from "./screen.ts";

export class RestoreScreen extends Screen {
  readonly ctl: RestoreController;
  private _synced: string | null = null;

  constructor(ctl: RestoreController, env: ScreenEnv) {
    super("restore", env);
    this.ctl = ctl;
  }

  protected signature(): unknown[] {
    const c = this.ctl;
    // the purchases on this device change only with the receipts: their revision, not a copy each frame
    return [c.state, c.error, c.candidates.length, { ...c.choices }, c.canConfirm, c.added.length, this.scfg._receiptsRev,
      this.env.logo()];
  }

  protected primary(): string {
    const c = this.ctl;
    if (c.state === "input" || c.state === "checking") return "tx_input";
    if (c.state === "choose") {
      for (const i of c.pendingChoices()) {
        if (!Object.prototype.hasOwnProperty.call(c.choices, i)) return `choice_${i}_${c.candidates[i]!.choices[0]}`;
      }
      return "restore";
    }
    return "close";
  }

  protected sync(): void {
    const f = this.control("tx_input") as HTMLInputElement | null;
    if (f !== null && this._synced !== this.ctl.tx_input) {
      f.value = this.ctl.tx_input;
      f.setSelectionRange(f.value.length, f.value.length);
      this._synced = this.ctl.tx_input;
    }
  }

  protected onCancel(): void {
    this.closeWith("close");
  }

  protected build(): void {
    const c = this.ctl;
    this._synced = null;
    const title = this.text("restore_purchases");
    this.sheet.setAttribute("aria-label", title);
    this.el.setAttribute("data-state", c.state);
    const body = this.div("sheet-body stack");
    this.body = body;
    this.sheet.append(body);
    const logo = this.logo();
    if (logo !== null) body.append(logo);
    body.append(this.label(title, "title", "title"));
    if (c.state === "closed") body.append(this.label(this.text("screen_inactive")));
    else if (c.state === "input" || c.state === "checking") this.inputPart(body);
    else if (c.state === "choose") this.choosePart(body);
    else if (c.state === "saving") body.append(this.statusRow(this.text("saving"), "accent"));
    else if (c.state === "done") {
      if (c.added.length > 0) {
        body.append(this.label(this.text("restored"), "ok", "restored"));
        c.added.forEach((r, i) => body.append(this.label(this.receiptLine(r), "", `added_${i}`)));
      } else {
        body.append(this.label(this.text("nothing_to_restore"), "", "nothing"));
      }
    }
    if (c.message() !== null && c.error) {
      const id = `restore_${c.error}`; // the restore screen's messages (texts.ts: RESTORE_MESSAGES under "restore_")
      body.append(this.label(this.text(id), "error", "message"));
    }
    const owned = this.scfg.receipts();
    if (owned.length > 0) {
      body.append(this.label(this.text("purchases_on_device"), "label"));
      owned.forEach((r, i) => {
        const copy = this.button(`copy_receipt_${i}`, this.text("copy_hash"), "small", () => this.scfg.copy(String(r["tx_hash"] ?? "")));
        body.append(this.div("value-row", this.label(this.receiptLine(r), "grow", `receipt_${i}`), copy));
      });
    }
    const foot = this.div("sheet-foot", this.rule(), this.button("close", this.text("close"), "", () => this.closeWith("close")));
    this.sheet.append(foot);
  }

  private inputPart(body: HTMLElement): void {
    const c = this.ctl;
    const typing = c.state === "input";
    body.append(this.label(this.text("paste_hash_help"), "muted"));
    const field = this.field("tx_input", c.tx_input, PASTE_MAX, TX_CHARS, (t) => {
      c.tx_input = t;
      this._synced = t;
    }, () => this.check(), "tx");
    field.readOnly = !typing;
    this._synced = c.tx_input;
    const row = this.div("field-row", field);
    if (this.scfg._platform.isWeb()) {
      const paste = this.button("paste", this.text("paste"), "small", () => this.scfg.pasteInto(c), "tx");
      paste.disabled = !typing;
      row.append(paste);
    }
    const check = this.button("check", this.text("check"), "accent", () => this.check());
    check.disabled = !typing;
    body.append(row, check);
    if (c.state === "checking") body.append(this.statusRow(this.text("checking_blockchain"), "accent"));
  }

  private choosePart(body: HTMLElement): void {
    const c = this.ctl;
    for (const i of c.pendingChoices()) {
      const cand = c.candidates[i]!;
      body.append(this.label(this.text("unmatched_payment_prompt", formatUnits(cand.transfer.value), cand.token, this.chainName(cand.chain)),
        "", `candidate_${i}`));
      const grid = this.grid(Math.min(cand.choices.length, 2));
      cand.choices.forEach((choice, j) => {
        const tip = choice === TIP_CHOICE;
        grid.append(this.option(`choice_${i}_${choice}`, tip ? this.text("keep_as_tip") : this.itemTitle(choice), "",
          c.choices[i] === choice, () => c.choose(i, choice), { row: `choices_${i}:${Math.floor(j / 2)}`, textIsData: !tip }));
      });
      body.append(grid);
    }
    const restore = this.button("restore", this.text("restore"), "primary", () => c.confirm());
    restore.disabled = !c.canConfirm;
    body.append(restore);
  }

  private check(): void {
    if (this.ctl.state === "input") this.ctl.check();
  }
}
