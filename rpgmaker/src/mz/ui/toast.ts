/**
 * A short notification ("Payment confirmed: ...") at the top right of the game, shown by the
 * default sheet host's notify() (Ren'Py's renpy.notify; Godot's ScfgToast). The text is already in
 * the player's language. It fades out after TOAST_SECONDS and removes itself; several stack.
 * Toasts take no clicks: the game behind them keeps its mouse.
 */

export const TOAST_SECONDS = 3;
export const TOAST_FADE_SECONDS = 0.3;

/** Show `text` as a toast in the host's `.scfg-root` (above the screens). Returns the toast element. */
export function showToast(root: HTMLElement, text: string): HTMLElement {
  const doc = root.ownerDocument;
  let stack = root.querySelector(":scope > .toasts") as HTMLElement | null;
  if (stack === null) {
    stack = doc.createElement("div");
    stack.className = "toasts";
    stack.setAttribute("aria-live", "polite");
  }
  root.appendChild(stack); // last: above every screen
  const toast = doc.createElement("div");
  toast.className = "toast";
  toast.setAttribute("role", "status");
  toast.textContent = text;
  stack.appendChild(toast);
  const win = doc.defaultView ?? (globalThis as any);
  win.setTimeout(() => toast.classList.add("fade"), TOAST_SECONDS * 1000);
  win.setTimeout(() => toast.remove(), (TOAST_SECONDS + TOAST_FADE_SECONDS) * 1000);
  return toast;
}
