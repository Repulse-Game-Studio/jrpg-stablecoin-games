/**
 * The harness page's boot, after the plugin scripts ran: what MZ's main.js does once the plugins
 * are loaded, `SceneManager.run(Scene_Boot)`. Built to `dist/test/harness.js`. Boot errors are
 * logged as MZ's error screen would show them.
 */
export {}; // a module: keeps `w` out of the global scope

const w = globalThis as any;

try {
  w.SceneManager.run(w.Scene_Boot);
} catch (e) {
  console.error("harness: boot failed", e);
  throw e;
}
