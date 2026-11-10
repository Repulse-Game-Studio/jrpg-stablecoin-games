/**
 * Game_Interpreter (rmmz_objects.js): runs an event's command list, one `update()` per frame,
 * stopping while it waits (`wait(frames)` or a wait mode). Written from MZ's documented behaviour.
 *
 * A command runs as MZ runs it: the interpreter's method named "command" + its code, whatever
 * defines it (the shim or a plugin); a code without one is skipped. The shim defines 230 (Wait)
 * and 357 (Plugin Command, MZ): `[pluginName, commandName, commandText, args]`, which calls
 * `PluginManager.callCommand` with the interpreter as `this`. Wait modes other than "message" are
 * never waiting (the shim has no transfers, routes, animations or images). Plugins add their own
 * wait modes by aliasing `updateWaitMode`, as with MZ.
 */
import type { Ctx } from "./context.ts";

export interface EventCommand {
  code: number;
  indent: number;
  parameters: any[];
}

export function createGameInterpreter(ctx: Ctx) {
  const g = ctx.g;
  /** Wait modes the shim knows, and whether each still holds; any other mode never holds. */
  const WAIT_MODES: Record<string, () => boolean> = { message: () => g.$gameMessage.isBusy() };

  class Game_Interpreter {
    declare _depth: number;
    declare _mapId: number;
    declare _eventId: number;
    declare _list: EventCommand[] | null;
    declare _index: number;
    declare _waitCount: number;
    declare _waitMode: string;
    declare _indent: number;

    constructor(...args: unknown[]) {
      this.initialize(...args);
    }

    initialize(depth: unknown = 0): void {
      this._depth = typeof depth === "number" ? depth : 0;
      this.clear();
    }

    // Game_Interpreter.prototype.clear
    clear(): void {
      this._mapId = 0;
      this._eventId = 0;
      this._list = null;
      this._index = 0;
      this._waitCount = 0;
      this._waitMode = "";
      this._indent = 0;
    }

    // Game_Interpreter.prototype.setup(list, eventId)
    setup(list: EventCommand[], eventId = 0): void {
      this.clear();
      this._mapId = g.$gameMap ? g.$gameMap.mapId() : 0;
      this._eventId = eventId;
      this._list = list;
    }

    // Game_Interpreter.prototype.isRunning
    isRunning(): boolean {
      return !!this._list;
    }

    // Game_Interpreter.prototype.update: run commands until one waits or the list ends
    update(): void {
      while (this.isRunning()) {
        if (this.updateWait()) break;
        if (!this.executeCommand()) break;
      }
    }

    // Game_Interpreter.prototype.updateWait
    updateWait(): boolean {
      return this.updateWaitCount() || this.updateWaitMode();
    }

    // Game_Interpreter.prototype.updateWaitCount: one frame of a wait(frames) is used up; false once none is left
    updateWaitCount(): boolean {
      const left = this._waitCount;
      this._waitCount = Math.max(0, left - 1);
      return left > 0;
    }

    // Game_Interpreter.prototype.updateWaitMode: whether the wait mode still holds (WAIT_MODES); a
    // mode that no longer holds is cleared. Plugins alias this for their own modes, as with MZ.
    updateWaitMode(): boolean {
      const holds = WAIT_MODES[this._waitMode]?.() ?? false;
      if (!holds) this._waitMode = "";
      return holds;
    }

    // Game_Interpreter.prototype.setWaitMode(waitMode)
    setWaitMode(waitMode: string): void {
      this._waitMode = waitMode;
    }

    // Game_Interpreter.prototype.wait(duration)
    wait(duration: number): void {
      this._waitCount = duration;
    }

    // Game_Interpreter.prototype.executeCommand. Modelled behaviour: the current command runs (its
    // indent becomes the interpreter's) through the method named "command" + its code, looked up
    // on the interpreter at run time (so a plugin's own command methods and aliases run, as in
    // MZ); one that answers false runs again next frame, anything else moves to the next command;
    // a code without a method is skipped; past the end, the interpreter stops.
    executeCommand(): boolean {
      const command = this.currentCommand();
      if (command === undefined) {
        this.terminate();
        return true;
      }
      this._indent = command.indent;
      const run: unknown = (this as any)[`command${command.code}`];
      if (typeof run === "function" && !run.call(this, command.parameters)) return false; // a falsy answer waits
      this._index += 1;
      return true;
    }

    // Game_Interpreter.prototype.currentCommand
    currentCommand(): EventCommand | undefined {
      return this._list ? this._list[this._index] : undefined;
    }

    // Game_Interpreter.prototype.terminate
    terminate(): void {
      this._list = null;
    }

    // Game_Interpreter.prototype.command230: Wait
    command230(params: any[]): boolean {
      this.wait(params[0]);
      return true;
    }

    // Game_Interpreter.prototype.command357: Plugin Command (MZ). Parameters [plugin file, command
    // name, the editor's text, args]: the command registered under the plugin's name runs with
    // this interpreter as `this`.
    command357([pluginFile, commandName, , args]: any[]): boolean {
      g.PluginManager.callCommand(this, g.Utils.extractFileName(pluginFile), commandName, args);
      return true;
    }
  }

  return { Game_Interpreter };
}
