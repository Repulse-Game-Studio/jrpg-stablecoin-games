/**
 * TEST ONLY. Built to `dist/test/e2e_config.js` for the web e2e page (test/harness/e2e.html), never
 * into SCFG.js. It runs after js/plugins.js and before the engine (the shim) registers the plugin
 * parameters, as a developer's own edit of js/plugins.js would: `window.scfgE2EConfig`, when the
 * runner set it, is the JSON text of `{"parameters": {name: value}}`, and each value (a string, as
 * MZ's Plugin Manager stores parameters) replaces that parameter of the SCFG entry of `$plugins`.
 * The runner points the demo kit's parameters at its local chain this way: the wallet, the chains
 * and tokens, and the RPC URLs. A config it cannot read stops the page with an error.
 */
export {}; // a module: keeps `w` out of the global scope

const w = globalThis as any;
const raw: unknown = w.scfgE2EConfig;
if (raw !== undefined) {
  if (typeof raw !== "string") throw new Error("e2e: window.scfgE2EConfig must be a JSON string");
  const config = JSON.parse(raw) as { parameters?: Record<string, unknown> };
  const plugins: any[] = Array.isArray(w.$plugins) ? w.$plugins : [];
  const entry = plugins.find((p) => p && p.name === "SCFG");
  if (entry === undefined) throw new Error("e2e: js/plugins.js has no SCFG entry");
  for (const [name, value] of Object.entries(config.parameters ?? {})) {
    if (typeof value !== "string") throw new Error(`e2e: parameter ${name} must be a string, as MZ stores it`);
    entry.parameters[name] = value;
  }
}
