/**
 * TEST ONLY. Built to `dist/test/e2e_config.js` for the web e2e page (test/harness/e2e.html), never
 * into SCFG.js. The page loads it after the demo kit's js/plugins.js and before the shim's script,
 * whose install registers the `$plugins` parameters (as MZ's Plugin Manager does), so the change is
 * what a developer's own edit of js/plugins.js would be: `window.scfgE2EConfig`, when the runner set
 * it (an init script), is the JSON text of `{"parameters": {name: value}}`, and each value (a
 * string, as MZ's Plugin Manager stores parameters) replaces that parameter of the SCFG entry of
 * `$plugins`. The runner points the demo kit's parameters at its local chain this way: the wallet,
 * the chains and tokens, and the RPC URLs. A config it cannot read throws here, before any game
 * code runs: an uncaught error in the page (no MZ error screen, which is not listening yet), the
 * game still boots on the demo kit's own parameters, and the runner fails the run on that page
 * error (tools/run_rpgmaker_web_e2e.py fails on every console error and page error).
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
