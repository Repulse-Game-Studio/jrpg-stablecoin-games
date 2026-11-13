/**
 * The game objects MZ creates in `DataManager.createGameObjects` (rmmz_objects.js), reduced to
 * what plugins touch: Game_Temp, Game_System, Game_Message, Game_Switches, Game_Variables,
 * Game_Map (with its event interpreter) and Game_Player. Written from MZ's documented behaviour;
 * like MZ, each constructor calls `initialize(...)`, so plugins can alias it.
 *
 * Simplified: Game_Player moves one tile at once (no walking animation, passability, followers or
 * vehicles); Game_Message is busy only while texts were added; Game_Map has no events or scrolling.
 */
import type { Ctx } from "./context.ts";

export function createGameObjects(ctx: Ctx) {
  const g = ctx.g;

  // Game_Temp
  class Game_Temp {
    declare _isPlaytest: boolean;
    declare _destinationX: number | null;
    declare _destinationY: number | null;

    constructor(...args: unknown[]) {
      this.initialize(...args);
    }

    initialize(..._args: unknown[]): void {
      this._isPlaytest = g.Utils.isOptionValid("test");
      this._destinationX = null;
      this._destinationY = null;
    }

    // Game_Temp.prototype.isPlaytest
    isPlaytest(): boolean {
      return this._isPlaytest;
    }

    // Game_Temp.prototype.setDestination / clearDestination / isDestinationValid
    setDestination(x: number, y: number): void {
      this._destinationX = x;
      this._destinationY = y;
    }

    clearDestination(): void {
      this._destinationX = null;
      this._destinationY = null;
    }

    isDestinationValid(): boolean {
      return this._destinationX !== null;
    }
  }

  // Game_System
  class Game_System {
    declare _menuEnabled: boolean;
    declare _saveEnabled: boolean;

    constructor(...args: unknown[]) {
      this.initialize(...args);
    }

    initialize(..._args: unknown[]): void {
      this._menuEnabled = true;
      this._saveEnabled = true;
    }

    // Game_System.prototype.isMenuEnabled / enableMenu / disableMenu / isSaveEnabled
    isMenuEnabled(): boolean {
      return this._menuEnabled;
    }

    enableMenu(): void {
      this._menuEnabled = true;
    }

    disableMenu(): void {
      this._menuEnabled = false;
    }

    isSaveEnabled(): boolean {
      return this._saveEnabled;
    }
  }

  // Game_Message
  class Game_Message {
    declare _texts: string[];

    constructor(...args: unknown[]) {
      this.initialize(...args);
    }

    initialize(..._args: unknown[]): void {
      this.clear();
    }

    // Game_Message.prototype.clear / add / isBusy
    clear(): void {
      this._texts = [];
    }

    add(text: string): void {
      this._texts.push(text);
    }

    isBusy(): boolean {
      return this._texts.length > 0;
    }
  }

  // Game_Switches: ids 1..$dataSystem.switches.length - 1; others are ignored
  class Game_Switches {
    declare _data: boolean[];

    constructor(...args: unknown[]) {
      this.initialize(...args);
    }

    initialize(..._args: unknown[]): void {
      this.clear();
    }

    // Game_Switches.prototype.clear
    clear(): void {
      this._data = [];
    }

    // Game_Switches.prototype.value(switchId)
    value(switchId: number): boolean {
      return !!this._data[switchId];
    }

    // Game_Switches.prototype.setValue(switchId, value)
    setValue(switchId: number, value: boolean): void {
      if (switchId > 0 && switchId < g.$dataSystem.switches.length) {
        this._data[switchId] = value;
        this.onChange();
      }
    }

    // Game_Switches.prototype.onChange: the map re-checks event pages
    onChange(): void {
      g.$gameMap.requestRefresh();
    }
  }

  // Game_Variables: ids 1..$dataSystem.variables.length - 1; numbers are floored
  class Game_Variables {
    declare _data: unknown[];

    constructor(...args: unknown[]) {
      this.initialize(...args);
    }

    initialize(..._args: unknown[]): void {
      this.clear();
    }

    // Game_Variables.prototype.clear
    clear(): void {
      this._data = [];
    }

    // Game_Variables.prototype.value(variableId): 0 when unset
    value(variableId: number): unknown {
      return this._data[variableId] || 0;
    }

    // Game_Variables.prototype.setValue(variableId, value)
    setValue(variableId: number, value: unknown): void {
      if (variableId > 0 && variableId < g.$dataSystem.variables.length) {
        this._data[variableId] = typeof value === "number" ? Math.floor(value) : value;
        this.onChange();
      }
    }

    // Game_Variables.prototype.onChange
    onChange(): void {
      g.$gameMap.requestRefresh();
    }
  }

  // Game_Map: the current map and its event interpreter
  class Game_Map {
    declare _interpreter: any;
    declare _mapId: number;
    declare _needsRefresh: boolean;

    constructor(...args: unknown[]) {
      this.initialize(...args);
    }

    initialize(..._args: unknown[]): void {
      this._interpreter = new g.Game_Interpreter();
      this._mapId = 1;
      this._needsRefresh = false;
    }

    // Game_Map.prototype.mapId
    mapId(): number {
      return this._mapId;
    }

    // Game_Map.prototype.requestRefresh / refreshIfNeeded / refresh
    requestRefresh(): void {
      this._needsRefresh = true;
    }

    refreshIfNeeded(): void {
      if (this._needsRefresh) this.refresh();
    }

    refresh(): void {
      this._needsRefresh = false;
    }

    // Game_Map.prototype.isEventRunning
    isEventRunning(): boolean {
      return this._interpreter.isRunning();
    }

    // Game_Map.prototype.update(sceneActive)
    update(sceneActive: boolean): void {
      this.refreshIfNeeded();
      if (sceneActive) this.updateInterpreter();
    }

    // Game_Map.prototype.updateInterpreter
    updateInterpreter(): void {
      this._interpreter.update();
    }
  }

  // Game_Player: moves by Input.dir4 when nothing blocks it
  class Game_Player {
    declare _x: number;
    declare _y: number;
    declare _direction: number;

    constructor(...args: unknown[]) {
      this.initialize(...args);
    }

    initialize(..._args: unknown[]): void {
      this._x = 0;
      this._y = 0;
      this._direction = 2;
    }

    // Game_CharacterBase x / y / direction()
    get x(): number {
      return this._x;
    }

    get y(): number {
      return this._y;
    }

    direction(): number {
      return this._direction;
    }

    // Game_Player.prototype.setupForNewGame: the start position ($dataSystem.startX/startY)
    setupForNewGame(): void {
      this._x = g.$dataSystem.startX;
      this._y = g.$dataSystem.startY;
      this._direction = 2;
    }

    // Game_Player.prototype.canMove: not while an event runs or a message shows
    canMove(): boolean {
      return !g.$gameMap.isEventRunning() && !g.$gameMessage.isBusy();
    }

    // Game_Player.prototype.getInputDirection
    getInputDirection(): number {
      return g.Input.dir4;
    }

    // Game_Player.prototype.moveByInput
    // modelled: a standing player who may move steps in the input direction, if any
    moveByInput(): void {
      if (this.isMoving() || !this.canMove()) return;
      const direction = this.getInputDirection();
      if (direction === 0) return;
      g.$gameTemp.clearDestination();
      this.executeMove(direction);
    }

    // Game_Player.prototype.executeMove(direction)
    executeMove(direction: number): void {
      this.moveStraight(direction);
    }

    // Game_CharacterBase.prototype.moveStraight(d): one tile, at once
    moveStraight(d: number): void {
      this._direction = d;
      if (d === 2) this._y += 1;
      else if (d === 8) this._y -= 1;
      else if (d === 4) this._x -= 1;
      else if (d === 6) this._x += 1;
    }

    // Game_CharacterBase.prototype.isMoving
    isMoving(): boolean {
      return false;
    }

    // Game_Player.prototype.update(sceneActive)
    update(sceneActive: boolean): void {
      if (sceneActive) this.moveByInput();
    }
  }

  return { Game_Temp, Game_System, Game_Message, Game_Switches, Game_Variables, Game_Map, Game_Player };
}
