/**
 * The bundle's entry point: `build.mjs` turns this file and everything it imports into
 * `dist/js/plugins/SCFG.js`, one ES2020 IIFE after the MZ header. MZ runs it once, when the Plugin
 * Manager loads the plugin, after the plugin parameters are registered and before the game boots
 * (boot.ts: nothing here touches the database or the save storage).
 */
import embedded from "scfg:assets";
import { installAssets } from "./assets.ts";
import { bootPlugin } from "./boot.ts";

installAssets(embedded);
bootPlugin();
