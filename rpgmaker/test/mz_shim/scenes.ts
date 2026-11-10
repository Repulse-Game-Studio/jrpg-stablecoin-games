/**
 * Scenes (rmmz_scenes.js): Scene_Base, Scene_Boot, Scene_Map and Scene_Menu, written from MZ's
 * documented behaviour and reduced to their plugin hooks: `create`/`start`/`update`/`terminate`,
 * the map's per-frame update (map interpreter, then the player) and its menu call, and the menu's
 * `createCommandWindow`.
 *
 * Boot order, as MZ: Scene_Boot.create starts `DataManager.loadDatabase()` and
 * `StorageManager.updateForageKeys()`; Scene_Boot.isReady is false until both are done (then
 * `onDatabaseLoaded()`); start then sets up a new game.
 *
 * Simplified: no sprites, fades, messages, transfers, encounters or title screen. Scene_Boot goes
 * straight to the map after `DataManager.setupNewGame()` (MZ sets up a new game, then shows the
 * title).
 */
import type { Ctx } from "./context.ts";

export function createScenes(ctx: Ctx) {
  const g = ctx.g;

  // Scene_Base
  class Scene_Base {
    declare _started: boolean;
    declare _active: boolean;
    declare _windows: any[];

    constructor(...args: unknown[]) {
      this.initialize(...args);
    }

    initialize(..._args: unknown[]): void {
      this._started = false;
      this._active = false;
      this._windows = [];
    }

    // Scene_Base.prototype.create / isActive / isReady / start / update / stop / isStarted / isBusy / terminate
    create(): void {}

    isActive(): boolean {
      return this._active;
    }

    isReady(): boolean {
      return true;
    }

    start(): void {
      this._started = true;
      this._active = true;
    }

    update(): void {
      this.updateChildren();
    }

    stop(): void {
      this._active = false;
    }

    isStarted(): boolean {
      return this._started;
    }

    isBusy(): boolean {
      return false;
    }

    terminate(): void {}

    // Scene_Base.prototype.updateChildren: each window's update
    updateChildren(): void {
      for (const w of this._windows) w.update();
    }

    // Scene_Base.prototype.addWindow(window)
    addWindow(window: unknown): void {
      this._windows.push(window);
    }

    // Scene_Base.prototype.popScene
    popScene(): void {
      g.SceneManager.pop();
    }
  }

  // Scene_Boot
  class Scene_Boot extends Scene_Base {
    /** (shim) onDatabaseLoaded() has run. */
    declare _shimLoadsDone: boolean;

    initialize(...args: unknown[]): void {
      super.initialize(...args);
      this._shimLoadsDone = false;
    }

    // Scene_Boot.prototype.create: start loading the database and the save keys
    create(): void {
      super.create();
      g.DataManager.loadDatabase();
      g.StorageManager.updateForageKeys();
    }

    // Scene_Boot.prototype.isReady. Modelled behaviour: not ready until the database and the save
    // keys are both loaded; the first frame they are, onDatabaseLoaded() runs once (and that frame
    // still answers false); from then on the base scene's readiness decides.
    isReady(): boolean {
      if (this._shimLoadsDone) return super.isReady();
      const loads = [() => g.DataManager.isDatabaseLoaded(), () => g.StorageManager.forageKeysUpdated()];
      if (loads.every((done) => done())) {
        this._shimLoadsDone = true;
        this.onDatabaseLoaded();
      }
      return false;
    }

    // Scene_Boot.prototype.onDatabaseLoaded. Modelled: only the encryption info (no system images, fonts)
    onDatabaseLoaded(): void {
      this.setEncryptionInfo();
    }

    // Scene_Boot.prototype.setEncryptionInfo: the database's encryption settings to Utils
    setEncryptionInfo(): void {
      const sys = g.$dataSystem;
      g.Utils.setEncryptionInfo(sys.hasEncryptedImages, sys.hasEncryptedAudio, sys.encryptionKey);
    }

    // Scene_Boot.prototype.start (startNormalGame, without the title)
    start(): void {
      super.start();
      g.DataManager.setupNewGame();
      g.SceneManager.goto(g.Scene_Map);
    }
  }

  // Scene_Map
  class Scene_Map extends Scene_Base {
    declare _waitCount: number;
    declare menuCalling: boolean;

    initialize(...args: unknown[]): void {
      super.initialize(...args);
      this._waitCount = 0;
      this.menuCalling = false;
    }

    // Scene_Map.prototype.start
    start(): void {
      super.start();
      this.menuCalling = false;
    }

    // Scene_Map.prototype.update
    update(): void {
      this.updateMainMultiply();
      if (this.isSceneChangeOk()) this.updateScene();
      this.updateWaitCount();
      super.update();
    }

    // Scene_Map.prototype.updateMainMultiply / updateMain: the map (and its interpreter), then the player
    updateMainMultiply(): void {
      this.updateMain();
    }

    updateMain(): void {
      const active = this.isActive();
      g.$gameMap.update(active);
      g.$gamePlayer.update(active);
    }

    // Scene_Map.prototype.isSceneChangeOk
    isSceneChangeOk(): boolean {
      return this.isActive() && !g.$gameMessage.isBusy();
    }

    // Scene_Map.prototype.updateScene
    updateScene(): void {
      if (!g.SceneManager.isSceneChanging()) this.updateCallMenu();
    }

    // Scene_Map.prototype.isBusy: a few frames after calling the menu
    isBusy(): boolean {
      return this._waitCount > 0 || super.isBusy();
    }

    // Scene_Map.prototype.updateWaitCount: one frame of the wait (after calling the menu) is used up
    updateWaitCount(): boolean {
      const left = this._waitCount;
      this._waitCount = Math.max(0, left - 1);
      return left > 0;
    }

    // Scene_Map.prototype.updateCallMenu
    // modelled: a menu call is remembered until the player stops moving; a disabled menu forgets it
    updateCallMenu(): void {
      if (!this.isMenuEnabled()) {
        this.menuCalling = false;
        return;
      }
      this.menuCalling = this.menuCalling || this.isMenuCalled();
      if (this.menuCalling && !g.$gamePlayer.isMoving()) this.callMenu();
    }

    // Scene_Map.prototype.isMenuEnabled
    isMenuEnabled(): boolean {
      return g.$gameSystem.isMenuEnabled() && !g.$gameMap.isEventRunning();
    }

    // Scene_Map.prototype.isMenuCalled: menu/escape key or a right click / two-finger tap
    isMenuCalled(): boolean {
      return g.Input.isTriggered("menu") || g.TouchInput.isCancelled();
    }

    // Scene_Map.prototype.callMenu
    callMenu(): void {
      g.SceneManager.push(g.Scene_Menu);
      g.Window_MenuCommand.initCommandPosition();
      g.$gameTemp.clearDestination();
      this._waitCount = 2;
    }
  }

  // Scene_Menu
  class Scene_Menu extends Scene_Base {
    declare _commandWindow: any;

    // Scene_Menu.prototype.create
    create(): void {
      super.create();
      this.createCommandWindow();
    }

    // Scene_Menu.prototype.createCommandWindow: plugins alias it to add their handlers
    createCommandWindow(): void {
      const commandWindow = new g.Window_MenuCommand(this.commandWindowRect());
      for (const symbol of ["item", "skill", "equip", "status", "formation", "options", "save", "gameEnd"]) {
        commandWindow.setHandler(symbol, () => commandWindow.activate());
      }
      commandWindow.setHandler("cancel", this.popScene.bind(this));
      this.addWindow(commandWindow);
      this._commandWindow = commandWindow;
    }

    // Scene_Menu.prototype.commandWindowRect
    commandWindowRect(): unknown {
      return new g.Rectangle(0, 0, 240, 400);
    }
  }

  return { Scene_Base, Scene_Boot, Scene_Map, Scene_Menu };
}
