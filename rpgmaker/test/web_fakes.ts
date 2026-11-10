/**
 * A browser page for the web glue's node:test tests (src/mz/web_page.test.ts, src/mz/web.test.ts):
 * a fake `window.scfgBridge` (plugin/game/scfg/web/bridge.js's API) that records every call and
 * answers what the test queues, and a fake document whose `<script>` elements "run" by putting
 * the fake bridge on the window when bridge.js's text is added. Not a test file.
 */

/** bridge.js's entry points, as the glue calls them: [name, ...arguments]. */
export type BridgeCall = [string, ...unknown[]];

/** `window.scfgBridge`: records calls; answers come from `answers[name]` (queued), else bridge.js's "nothing happened". */
export class FakeBridge {
  calls: BridgeCall[] = [];
  answers: Record<string, unknown[]> = {};
  /** When set, every call throws it (a replaced, broken bridge). */
  throws: unknown = null;

  private _call(name: string, args: unknown[], fallback: unknown): unknown {
    this.calls.push([name, ...args]);
    if (this.throws !== null) throw this.throws;
    const queue = this.answers[name];
    return queue !== undefined && queue.length > 0 ? queue.shift() : fallback;
  }

  hasWallet(...a: unknown[]): unknown { return this._call("hasWallet", a, 0); }
  setPage(...a: unknown[]): unknown { return this._call("setPage", a, undefined); }
  openCheckout(...a: unknown[]): unknown { return this._call("openCheckout", a, undefined); }
  show(...a: unknown[]): unknown { return this._call("show", a, undefined); }
  hide(...a: unknown[]): unknown { return this._call("hide", a, undefined); }
  take(...a: unknown[]): unknown { return this._call("take", a, "[]"); }
  setStatus(...a: unknown[]): unknown { return this._call("setStatus", a, undefined); }
  reply(...a: unknown[]): unknown { return this._call("reply", a, undefined); }
  closeCheckout(...a: unknown[]): unknown { return this._call("closeCheckout", a, undefined); }
  copy(...a: unknown[]): unknown { return this._call("copy", a, undefined); }
  takeCopyFailed(...a: unknown[]): unknown { return this._call("takeCopyFailed", a, 0); }
  requestPaste(...a: unknown[]): unknown { return this._call("requestPaste", a, undefined); }
  takePaste(...a: unknown[]): unknown { return this._call("takePaste", a, "null"); }
  openUrl(...a: unknown[]): unknown { return this._call("openUrl", a, undefined); }

  /** The calls of one entry point, without the name. */
  callsTo(name: string): unknown[][] {
    return this.calls.filter((c) => c[0] === name).map((c) => c.slice(1));
  }
}

/** An element of the fake document. */
export class FakeElement {
  readonly tagName: string;
  textContent = "";
  readonly attributes: Record<string, string> = {};
  readonly children: FakeElement[] = [];
  private readonly _added: (el: FakeElement) => void;

  constructor(tagName: string, added: (el: FakeElement) => void) {
    this.tagName = tagName.toUpperCase();
    this._added = added;
  }

  setAttribute(name: string, value: string): void {
    this.attributes[name] = String(value);
  }

  appendChild(el: FakeElement): FakeElement {
    this.children.push(el);
    this._added(el);
    return el;
  }

  get lastElementChild(): FakeElement | null {
    return this.children.length > 0 ? this.children[this.children.length - 1]! : null;
  }
}

/** A page's window: `document` with a head and a body, and `scfgBridge` once bridge.js "ran". */
export interface FakePageWindow {
  document: { head: FakeElement; body: FakeElement; createElement(tag: string): FakeElement };
  scfgBridge?: FakeBridge;
  /** Every `<script>` element added, in order. */
  scripts: FakeElement[];
}

/**
 * A fake page whose scripts "run" when added: the one whose text is `bridgeText` puts `bridge` on
 * the window (as bridge.js does), unless `blocked` (a Content-Security-Policy that stops inline scripts).
 */
export function fakePageWindow(bridge: FakeBridge, opts: { bridgeText: (text: string) => boolean; blocked?: boolean }): FakePageWindow {
  const win = { scripts: [] as FakeElement[] } as unknown as FakePageWindow;
  const added = (el: FakeElement) => {
    if (el.tagName !== "SCRIPT") return;
    win.scripts.push(el);
    if (!opts.blocked && opts.bridgeText(el.textContent)) win.scfgBridge = bridge;
  };
  win.document = {
    head: new FakeElement("head", added),
    body: new FakeElement("body", added),
    createElement: (tag: string) => new FakeElement(tag, added),
  };
  return win;
}
