/**
 * Windows (rmmz_windows.js): Window_Base, Window_Selectable, Window_Command and Window_MenuCommand,
 * written from MZ's documented behaviour and reduced to commands, selection, handlers and keyboard
 * handling (`down`/`up` move, `ok` calls the current command's handler, `cancel` the cancel
 * handler). No drawing, scrolling or touch selection. Rectangle stands in for MZ's (a PIXI
 * Rectangle).
 */
import type { Ctx } from "./context.ts";

export interface MenuCommand {
  name: string;
  symbol: string;
  enabled: boolean;
  ext: unknown;
}

export function createWindows(ctx: Ctx) {
  const g = ctx.g;

  // Rectangle
  class Rectangle {
    declare x: number;
    declare y: number;
    declare width: number;
    declare height: number;

    constructor(x = 0, y = 0, width = 0, height = 0) {
      this.x = x;
      this.y = y;
      this.width = width;
      this.height = height;
    }
  }

  // Window_Base
  class Window_Base {
    declare x: number;
    declare y: number;
    declare width: number;
    declare height: number;
    declare active: boolean;
    declare visible: boolean;
    declare openness: number;

    constructor(...args: unknown[]) {
      this.initialize(...args);
    }

    initialize(rect?: unknown): void {
      const r = (rect ?? new Rectangle()) as Rectangle;
      this.x = r.x;
      this.y = r.y;
      this.width = r.width;
      this.height = r.height;
      this.active = true;
      this.visible = true;
      this.openness = 255;
    }

    update(): void {}

    // Window.prototype.isOpen / open / close; Window_Base.prototype.show / hide / activate / deactivate
    isOpen(): boolean {
      return this.openness >= 255;
    }

    open(): void {
      this.openness = 255;
    }

    close(): void {
      this.openness = 0;
    }

    show(): void {
      this.visible = true;
    }

    hide(): void {
      this.visible = false;
    }

    activate(): void {
      this.active = true;
    }

    deactivate(): void {
      this.active = false;
    }
  }

  // Window_Selectable
  class Window_Selectable extends Window_Base {
    declare _index: number;
    declare _handlers: Record<string, () => void>;
    declare _cursorFixed: boolean;
    declare _cursorAll: boolean;
    declare _canRepeat: boolean;

    initialize(rect?: unknown): void {
      super.initialize(rect);
      this._index = -1;
      this._handlers = {};
      this._cursorFixed = false;
      this._cursorAll = false;
      this._canRepeat = true;
      this.deactivate();
    }

    // Window_Selectable.prototype.index / maxItems / select / forceSelect / smoothSelect / deselect
    index(): number {
      return this._index;
    }

    maxItems(): number {
      return 0;
    }

    select(index: number): void {
      this._index = index;
    }

    forceSelect(index: number): void {
      this.select(index);
    }

    smoothSelect(index: number): void {
      this.select(index);
    }

    deselect(): void {
      this.select(-1);
    }

    // Window_Selectable.prototype.setHandler / isHandled / callHandler
    setHandler(symbol: string, method: () => void): void {
      this._handlers[symbol] = method;
    }

    isHandled(symbol: string): boolean {
      return !!this._handlers[symbol];
    }

    callHandler(symbol: string): void {
      if (this.isHandled(symbol)) this._handlers[symbol]!();
    }

    // Window_Selectable.prototype.isOpenAndActive
    isOpenAndActive(): boolean {
      return this.isOpen() && this.visible && this.active;
    }

    // Window_Selectable.prototype.update
    update(): void {
      super.update();
      this.processCursorMove();
      this.processHandling();
    }

    // Window_Selectable.prototype.isCursorMovable / processCursorMove / cursorDown / cursorUp
    isCursorMovable(): boolean {
      return this.isOpenAndActive() && !this._cursorFixed && !this._cursorAll && this.maxItems() > 0;
    }

    processCursorMove(): void {
      if (!this.isCursorMovable()) return;
      if (g.Input.isRepeated("down")) this.cursorDown(g.Input.isTriggered("down"));
      if (g.Input.isRepeated("up")) this.cursorUp(g.Input.isTriggered("up"));
    }

    // modelled: one row down; past the last row only when wrapping (a fresh press), to the first
    cursorDown(wrap: boolean): void {
      const count = this.maxItems();
      const next = this.index() + 1;
      if (next < count) this.smoothSelect(next);
      else if (wrap) this.smoothSelect(next % count);
    }

    // modelled: one row up (from row 0 when nothing is selected); above the first only when wrapping, to the last
    cursorUp(wrap: boolean): void {
      const count = this.maxItems();
      const here = Math.max(0, this.index());
      if (here > 0) this.smoothSelect(here - 1);
      else if (wrap) this.smoothSelect(count - 1);
    }

    // Window_Selectable.prototype.processHandling
    // modelled: while open and active, OK wins over cancel, each only when enabled
    processHandling(): void {
      if (!this.isOpenAndActive()) return;
      const ok = this.isOkEnabled() && this.isOkTriggered();
      if (ok) return this.processOk();
      if (this.isCancelEnabled() && this.isCancelTriggered()) this.processCancel();
    }

    // Window_Selectable.prototype.isOkEnabled / isCancelEnabled / isOkTriggered / isCancelTriggered
    isOkEnabled(): boolean {
      return true;
    }

    isCancelEnabled(): boolean {
      return this.isHandled("cancel");
    }

    isOkTriggered(): boolean {
      return this._canRepeat ? g.Input.isRepeated("ok") : g.Input.isTriggered("ok");
    }

    isCancelTriggered(): boolean {
      return g.Input.isRepeated("cancel");
    }

    // Window_Selectable.prototype.isCurrentItemEnabled
    isCurrentItemEnabled(): boolean {
      return true;
    }

    // Window_Selectable.prototype.processOk / callOkHandler / processCancel / callCancelHandler
    // modelled: OK on an enabled item (a disabled one does nothing), cancel always: either one
    // clears the input, deactivates the window and calls its handler
    processOk(): void {
      if (this.isCurrentItemEnabled()) this._shimLeave(() => this.callOkHandler());
    }

    callOkHandler(): void {
      this.callHandler("ok");
    }

    processCancel(): void {
      this._shimLeave(() => this.callCancelHandler());
    }

    // (shim) what OK and cancel share
    _shimLeave(handler: () => void): void {
      this.updateInputData();
      this.deactivate();
      handler();
    }

    callCancelHandler(): void {
      this.callHandler("cancel");
    }

    // Window_Selectable.prototype.updateInputData: consumes the key that was just handled
    updateInputData(): void {
      g.Input.update();
      g.TouchInput.update();
    }
  }

  // Window_Command
  class Window_Command extends Window_Selectable {
    declare _list: MenuCommand[];

    initialize(rect?: unknown): void {
      super.initialize(rect);
      this.refresh();
      this.select(0);
      this.activate();
    }

    // Window_Command.prototype.maxItems / clearCommandList / makeCommandList / addCommand
    maxItems(): number {
      return this._list.length;
    }

    clearCommandList(): void {
      this._list = [];
    }

    makeCommandList(): void {}

    addCommand(name: string, symbol: string, enabled = true, ext: unknown = null): void {
      this._list.push({ name, symbol, enabled, ext });
    }

    // Window_Command.prototype.commandName / commandSymbol / isCommandEnabled / currentData
    commandName(index: number): string {
      return this._list[index]!.name;
    }

    commandSymbol(index: number): string {
      return this._list[index]!.symbol;
    }

    isCommandEnabled(index: number): boolean {
      return this._list[index]!.enabled;
    }

    currentData(): MenuCommand | null {
      return this.index() >= 0 ? this._list[this.index()] ?? null : null;
    }

    // Window_Command.prototype.isCurrentItemEnabled / currentSymbol / currentExt
    isCurrentItemEnabled(): boolean {
      return this.currentData()?.enabled ?? false;
    }

    currentSymbol(): string | null {
      return this.currentData()?.symbol ?? null;
    }

    currentExt(): unknown {
      return this.currentData()?.ext ?? null;
    }

    // Window_Command.prototype.findSymbol / selectSymbol
    findSymbol(symbol: string): number {
      return this._list.findIndex((item) => item.symbol === symbol);
    }

    // modelled: the command with that symbol, else the first
    selectSymbol(symbol: string): void {
      this.forceSelect(Math.max(0, this.findSymbol(symbol)));
    }

    // Window_Command.prototype.callOkHandler: the current command's own handler, else "ok"
    // modelled: a handler for the current command's symbol is called directly; otherwise the "ok"
    // fallback is Window_Selectable's own callOkHandler, looked up on its prototype at call time,
    // so plugins that alias Window_Selectable.prototype.callOkHandler are reached as in MZ; with
    // neither handler, the window stays active
    callOkHandler(): void {
      const own = this.currentSymbol();
      if (own !== null && this.isHandled(own)) {
        this.callHandler(own);
        return;
      }
      if (!this.isHandled("ok")) {
        this.activate();
        return;
      }
      Window_Selectable.prototype.callOkHandler.call(this);
    }

    // Window_Command.prototype.refresh
    refresh(): void {
      this.clearCommandList();
      this.makeCommandList();
    }
  }

  // Window_MenuCommand: the main menu; plugins add commands in addOriginalCommands
  class Window_MenuCommand extends Window_Command {
    static _lastCommandSymbol: string | null = null;

    // Window_MenuCommand.initCommandPosition
    static initCommandPosition(): void {
      Window_MenuCommand._lastCommandSymbol = null;
    }

    // Window_MenuCommand.prototype.makeCommandList
    makeCommandList(): void {
      this.addMainCommands();
      this.addFormationCommand();
      this.addOriginalCommands();
      this.addOptionsCommand();
      this.addSaveCommand();
      this.addGameEndCommand();
    }

    // Window_MenuCommand.prototype.addMainCommands
    addMainCommands(): void {
      for (const [name, symbol] of [["Item", "item"], ["Skill", "skill"], ["Equip", "equip"], ["Status", "status"]]) {
        if (this.needsCommand(symbol!)) this.addCommand(name!, symbol!, true);
      }
    }

    // Window_MenuCommand.prototype.addFormationCommand
    addFormationCommand(): void {
      if (this.needsCommand("formation")) this.addCommand("Formation", "formation", true);
    }

    // Window_MenuCommand.prototype.addOriginalCommands: empty, for plugins
    addOriginalCommands(): void {}

    // Window_MenuCommand.prototype.addOptionsCommand / addSaveCommand / addGameEndCommand
    addOptionsCommand(): void {
      if (this.needsCommand("options")) this.addCommand("Options", "options", true);
    }

    addSaveCommand(): void {
      if (this.needsCommand("save")) this.addCommand("Save", "save", g.$gameSystem.isSaveEnabled());
    }

    addGameEndCommand(): void {
      this.addCommand("Game End", "gameEnd", true);
    }

    // Window_MenuCommand.prototype.needsCommand(name): MZ reads $dataSystem.menuCommands
    needsCommand(_name: string): boolean {
      return true;
    }

    // Window_MenuCommand.prototype.processOk
    processOk(): void {
      Window_MenuCommand._lastCommandSymbol = this.currentSymbol();
      super.processOk();
    }

    // Window_MenuCommand.prototype.selectLast
    selectLast(): void {
      this.selectSymbol(Window_MenuCommand._lastCommandSymbol ?? "");
    }
  }

  return { Rectangle, Window_Base, Window_Selectable, Window_Command, Window_MenuCommand };
}
