/**
 * DataManager (rmmz_managers.js): the database, new game, save and load, as plugins hook them.
 * Written from MZ's documented behaviour. `loadDatabase()` (from Scene_Boot.create, after every
 * plugin script has run) loads `$dataSystem` asynchronously, as MZ's XHR loads do; until then it
 * is null. `extractSaveContents` is the hook plugins alias to re-apply state after a load.
 *
 * Simplified: the database is only `$dataSystem` and `$dataCommonEvents`, built from the shim's
 * options (the title, switch and variable names, the start position, `advanced.gameId`, the
 * encryption settings; the common events' lists) and set on the next macrotask; the save
 * contents are only system, switches, variables, map and player; no autosave.
 */
import { DEFAULT_GAME_ID, type Ctx } from "./context.ts";

export function createDataManager(ctx: Ctx) {
  const g = ctx.g;
  const opts = ctx.opts;
  // data/System.json, reduced
  const system = () => ({
    switches: ["", ...Array.from({ length: opts.switches ?? 20 }, (_, i) => `Switch ${i + 1}`)],
    variables: ["", ...Array.from({ length: opts.variables ?? 20 }, (_, i) => `Variable ${i + 1}`)],
    gameTitle: opts.gameTitle ?? "Shim Game",
    startMapId: 1, startX: 8, startY: 6,
    advanced: { gameId: opts.gameId ?? DEFAULT_GAME_ID, screenWidth: opts.width ?? 816, screenHeight: opts.height ?? 624 },
    // the deployment's "Encrypt image files" / "Encrypt audio files" and the project's key
    hasEncryptedImages: !!opts.encryption?.images, hasEncryptedAudio: !!opts.encryption?.audio,
    encryptionKey: opts.encryption?.key ?? "",
  });
  // data/CommonEvents.json, reduced: index 0 is null, as in MZ's database arrays
  const commonEvents = () => [null, ...(opts.commonEvents ?? []).map((e, i) => ({
    id: i + 1, name: e.name ?? `Common Event ${i + 1}`, trigger: 0, switchId: 1, list: e.list,
  }))];
  return {
    _globalInfo: [] as unknown[],
    _lastAccessedId: 1,

    // DataManager.loadDatabase(): every $data* to null, then each file arrives later
    loadDatabase(): void {
      g.$dataSystem = null;
      g.$dataCommonEvents = null;
      setTimeout(() => {
        g.$dataCommonEvents = commonEvents();
        this.onLoad(g.$dataCommonEvents);
        g.$dataSystem = system();
        this.onLoad(g.$dataSystem);
      }, 0);
    },
    // DataManager.onLoad(object)
    onLoad(_object: unknown): void {},
    // DataManager.isDatabaseLoaded()
    isDatabaseLoaded(): boolean {
      return !!g.$dataSystem;
    },

    // DataManager.createGameObjects()
    createGameObjects(): void {
      g.$gameTemp = new g.Game_Temp();
      g.$gameSystem = new g.Game_System();
      g.$gameMessage = new g.Game_Message();
      g.$gameSwitches = new g.Game_Switches();
      g.$gameVariables = new g.Game_Variables();
      g.$gameMap = new g.Game_Map();
      g.$gamePlayer = new g.Game_Player();
    },
    // DataManager.setupNewGame(). Modelled behaviour: fresh game objects, save slot 1 selected, the
    // player at the start position, the frame count back to 0. Each step is a public hook plugins alias.
    setupNewGame(): void {
      const steps: (() => void)[] = [
        () => this.createGameObjects(), () => this.selectSavefileForNewGame(), () => g.$gamePlayer.setupForNewGame(),
      ];
      steps.forEach((step) => step());
      g.Graphics.frameCount = 0;
    },
    // DataManager.setupBattleTest() / setupEventTest(): fresh game objects without setupNewGame,
    // as MZ; their party, troop and transfer set-up is not modelled.
    setupBattleTest(): void {
      this.createGameObjects();
    },
    setupEventTest(): void {
      this.createGameObjects();
      this.selectSavefileForNewGame();
    },
    // DataManager.selectSavefileForNewGame()
    selectSavefileForNewGame(): void {
      this._lastAccessedId = 1;
    },
    // DataManager.isEventTest() / isBattleTest()
    isEventTest(): boolean {
      return false;
    },
    isBattleTest(): boolean {
      return false;
    },
    // DataManager.makeSavename(savefileId)
    makeSavename(savefileId: number): string {
      return "file" + savefileId;
    },
    // DataManager.makeSaveContents()
    makeSaveContents(): Record<string, unknown> {
      return {
        system: g.$gameSystem, switches: g.$gameSwitches, variables: g.$gameVariables, map: g.$gameMap, player: g.$gamePlayer,
      };
    },
    // DataManager.extractSaveContents(contents)
    extractSaveContents(contents: any): void {
      g.$gameSystem = contents.system;
      g.$gameSwitches = contents.switches;
      g.$gameVariables = contents.variables;
      g.$gameMap = contents.map;
      g.$gamePlayer = contents.player;
    },
    // DataManager.saveGame(savefileId) -> Promise of 0. In MZ 1.x's order: contents, then save
    // name; once "file<id>" is written, the global info is saved too (not waited for).
    saveGame(savefileId: number): Promise<number> {
      const contents = this.makeSaveContents();
      const saveName = this.makeSavename(savefileId);
      return g.StorageManager.saveObject(saveName, contents).then(() => this._shimSlotWritten(savefileId));
    },
    // DataManager.loadGame(savefileId) -> Promise of 0. Modelled behaviour: the slot is read, then
    // the game objects are rebuilt and the loaded contents handed to extractSaveContents.
    loadGame(savefileId: number): Promise<number> {
      return g.StorageManager.loadObject(this.makeSavename(savefileId))
        .then((contents: unknown) => this._shimRebuildFrom(contents));
    },
    // (shim) after a slot was written
    _shimSlotWritten(savefileId: number): number {
      this._globalInfo[savefileId] = this.makeSavefileInfo();
      this.saveGlobalInfo();
      return 0;
    },
    // (shim) the game objects from loaded save contents
    _shimRebuildFrom(contents: unknown): number {
      for (const step of [() => this.createGameObjects(), () => this.extractSaveContents(contents), () => this.correctDataErrors()]) {
        step();
      }
      return 0;
    },
    // DataManager.makeSavefileInfo()
    makeSavefileInfo(): Record<string, unknown> {
      return { title: "SCFG shim", playtime: "00:00:00", timestamp: Date.now() };
    },
    // DataManager.saveGlobalInfo()
    saveGlobalInfo(): void {
      g.StorageManager.saveObject("global", this._globalInfo);
    },
    // DataManager.correctDataErrors()
    correctDataErrors(): void {},
  };
}
