/**
 * The virtual module `build.mjs` generates for the bundle: the checkout page files and the
 * `src/mz/ui/*.css` stylesheets, imported with esbuild's text loader. Only `main.ts` imports it;
 * node:test cannot resolve it (tests install the files from disk instead).
 */
declare module "scfg:assets" {
  const embedded: import("./assets.ts").Assets;
  export default embedded;
}
