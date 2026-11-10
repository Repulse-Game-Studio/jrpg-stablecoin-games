/**
 * The Tip Jar entry of MZ's main menu ("Tip Jar Menu" parameter, off by default). The command
 * (named by "Tip Jar Label") is added in `Window_MenuCommand.prototype.addOriginalCommands`,
 * where MZ expects plugins to add theirs, after Formation; it is disabled while payments are off
 * (invalid plugin parameters). `Scene_Menu.prototype.commandScfgTipJar` (its handler) opens the
 * tip sheet, `SCFG.pay("tip")` with the Tip command's defaults (commands.ts `tipOptions`), and
 * gives the menu back when the sheet closes: the command window is active again, on the entry.
 */
import { tipOptions } from "./commands.ts";
import { mz } from "./platform.ts";
import type { Scfg } from "./scfg.ts";

/** The menu command's symbol (`Window_Command.addCommand(name, symbol)`). */
export const TIP_JAR_SYMBOL = "scfgTipJar";

/** Add the Tip Jar to the main menu (once per game). */
export function installTipJarMenu(scfg: Scfg, label: string): void {
  const g = mz();
  const commands = g.Window_MenuCommand.prototype;
  const menu = g.Scene_Menu.prototype;
  const addOriginalCommands = commands.addOriginalCommands;
  commands.addOriginalCommands = function (this: any, ...args: unknown[]) {
    const r = addOriginalCommands.apply(this, args);
    this.addCommand(label, TIP_JAR_SYMBOL, scfg._disabled === null);
    return r;
  };
  const createCommandWindow = menu.createCommandWindow;
  menu.createCommandWindow = function (this: any, ...args: unknown[]) {
    const r = createCommandWindow.apply(this, args);
    this._commandWindow.setHandler(TIP_JAR_SYMBOL, this.commandScfgTipJar.bind(this));
    return r;
  };
  menu.commandScfgTipJar = function (this: any) {
    const commandWindow = this._commandWindow;
    let shown: Promise<unknown>;
    try {
      shown = scfg.pay("tip", tipOptions(scfg));
    } catch (e) {
      shown = Promise.reject(e);
    }
    void shown
      .catch((e: unknown) => scfg._reportError("Tip Jar failed:", e))
      .then(() => {
        if (commandWindow && g.SceneManager._scene === this) commandWindow.activate();
      })
      .catch((e: unknown) => scfg._reportError("Tip Jar: cannot reactivate the menu:", e)); // (MZ's onReject)
  };
}
