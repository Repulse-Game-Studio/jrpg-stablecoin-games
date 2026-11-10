/**
 * PluginManager (rmmz_managers.js), from MZ's documented behaviour: parameters by plugin name
 * (case-insensitive), commands registered as "<plugin>:<command>" and called with the
 * Game_Interpreter as `this`. `loadScript` loads nothing: the harness page loads SCFG.js itself,
 * in the place MZ's `PluginManager.setup` would.
 */
import type { Ctx, PluginEntry } from "./context.ts";

export function createPluginManager(ctx: Ctx) {
  const g = ctx.g;
  return {
    _scripts: [] as string[],
    _errorUrls: [] as string[],
    _parameters: {} as Record<string, Record<string, string>>,
    _commands: {} as Record<string, (args: any) => void>,

    // PluginManager.setup(plugins). Modelled behaviour: every enabled plugin, once per name, has its
    // parameters registered and then its script loaded.
    setup(plugins: readonly PluginEntry[]): void {
      const enabled = plugins.filter((p) => p.status).map((p) => ({ p, name: g.Utils.extractFileName(p.name) as string }));
      for (const { p, name } of enabled) {
        if (this._scripts.includes(name)) continue;
        this._scripts.push(name);
        this.setParameters(name, p.parameters);
        this.loadScript(name);
      }
    },
    // PluginManager.parameters(name): case-insensitive, {} when unknown
    parameters(name: string): Record<string, string> {
      return this._parameters[name.toLowerCase()] || {};
    },
    // PluginManager.setParameters(name, parameters)
    setParameters(name: string, parameters: Record<string, string>): void {
      this._parameters[name.toLowerCase()] = parameters;
    },
    // PluginManager.loadScript(filename): see the module comment
    loadScript(_filename: string): void {},
    // PluginManager.registerCommand(pluginName, commandName, func)
    registerCommand(pluginName: string, commandName: string, func: (args: any) => void): void {
      this._commands[`${pluginName}:${commandName}`] = func;
    },
    // PluginManager.callCommand(self, pluginName, commandName, args): the registered function runs
    // with `self` (the Game_Interpreter) as `this`; an unknown command does nothing
    callCommand(self: unknown, pluginName: string, commandName: string, args: unknown): void {
      const registered = this._commands[`${pluginName}:${commandName}`];
      if (typeof registered === "function") Reflect.apply(registered, self, [args]);
    },
  };
}
