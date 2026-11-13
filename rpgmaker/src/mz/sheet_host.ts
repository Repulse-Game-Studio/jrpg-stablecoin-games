/**
 * The interface between the payment and restore flows (`SCFG.pay()`, `SCFG.restore()`) and the
 * screens that show them: the twin of Godot's sheet_host.gd. `SCFG.sheetHost` holds one, by default
 * the plugin's own (ui/default_sheet_host.ts: the HTML payment sheet and restore screen over the
 * game); replace it to show the flows your own way.
 *
 * A host shows a screen for a controller and hands back a `SheetHandle`, whose `closed` Promise it
 * resolves once, when the player is done with the screen:
 *   - payment sheet: "cancel" (the player gave up), "keep_playing" (close the sheet but keep the
 *     payment and its browser page alive; the final result arrives later, through the pay()
 *     callback or `SCFG.onResult`) or "finish" (close after a final state: paid, underpaid or
 *     expired). These are Ren'Py's `_close_checkout` hows. A rejected `closed` counts as "cancel".
 *   - restore screen: any value (the restore screen only closes).
 *
 * When the game quits with a screen open (the window or tab closes), the flow does not wait for
 * `closed`: pay() resolves "cancelled" (or the final result the controller already has) and
 * restore() its result so far, nothing is delivered (no callback, no `onResult`, no notification),
 * as Ren'Py's `_shutdown`, and `close(handle)` is still called.
 *
 * The screen only reads the controller's fields and getters and calls its methods
 * (CheckoutController / RestoreController, the core's); SCFG polls the controllers every frame, so
 * a screen never calls `poll()`. Buttons that leave the game call SCFG's actions:
 * `SCFG.openBrowser(ctl)`, `SCFG.openWalletApp(ctl)`, `SCFG.openExplorer(url)`, `SCFG.copy(text)`
 * and `SCFG.pasteInto(ctl)`.
 *
 * The flows close the controller (stopping its browser page) BEFORE they call `close(handle)`.
 *
 * A host whose `openPayment()` / `openRestore()` returns null (or no handle with a `closed`
 * Promise) shows nothing: pay() / restore() then resolve a "failed" result whose `error` is
 * "sheet.unavailable". `NullSheetHost` is that bare interface.
 */
import type { CheckoutController, RestoreController } from "../core/index.ts";

/** How a payment sheet was closed (see the header). */
export type SheetClose = "cancel" | "keep_playing" | "finish";

/** A shown screen: `closed` resolves once, when the player closes it. */
export interface SheetHandle {
  closed: Promise<SheetClose | string>;
}

export interface SheetHost {
  /** Show the payment sheet for `ctl`; its handle, or null when no sheet can be shown. */
  openPayment(ctl: CheckoutController): SheetHandle | null;
  /** Show the restore screen for `ctl`; its handle, or null when no screen can be shown. */
  openRestore(ctl: RestoreController): SheetHandle | null;
  /** Remove a screen opened by openPayment() / openRestore(), after its controller was closed. */
  close(handle: SheetHandle): void;
  /**
   * Show a short message to the player, already in the player's language (SCFG.text). The
   * payment and restore announcements come only when `SCFG.notify` is true; "Couldn't copy: ..."
   * comes whatever `SCFG.notify` is, since it answers the player's own Copy press.
   */
  notify(text: string): void;
  /**
   * Optional: called once a frame, after SCFG's pump has polled the controllers (also while the
   * game is paused). The default host reads MZ's input for its screens and rebuilds them here.
   */
  update?(): void;
}

/** The bare interface: shows nothing (pay() and restore() resolve "failed" with the error "sheet.unavailable"). */
export class NullSheetHost implements SheetHost {
  openPayment(_ctl: CheckoutController): SheetHandle | null {
    return null;
  }

  openRestore(_ctl: RestoreController): SheetHandle | null {
    return null;
  }

  close(_handle: SheetHandle): void {}

  notify(_text: string): void {}
}

/** True for what a host may hand back: an object with a `closed` Promise (a thenable). */
export function isSheetHandle(v: unknown): v is SheetHandle {
  if (typeof v !== "object" || v === null) return false;
  const closed = (v as { closed?: unknown }).closed;
  return typeof closed === "object" && closed !== null && typeof (closed as { then?: unknown }).then === "function";
}
