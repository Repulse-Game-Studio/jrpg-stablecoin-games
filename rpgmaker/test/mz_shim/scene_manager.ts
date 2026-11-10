/**
 * SceneManager (rmmz_managers.js): the scene stack and the per-frame `updateMain`, which plugins
 * alias to run code every frame. Written from MZ's documented behaviour: `updateMain` counts the
 * frame, reads input (`updateInputData`), changes scene if one is queued and the current one is
 * not busy, then starts or updates the current scene.
 *
 * In a page `run(sceneClass)` drives `update` from `requestAnimationFrame`, one `updateMain` per
 * animation frame (MZ runs a fixed 60 updates a second whatever the display rate). In Node nothing
 * runs by itself: tests call `shim.frame(n)`.
 *
 * Errors, as MZ: `run()` (MZ's main.js) calls `initialize()`, whose `setupEventHandlers()` listens
 * for the window's "error" (an uncaught exception) and "unhandledrejection" (a Promise rejected
 * with no handler). `onReject` hands the rejection's reason to `onError`, which prints the error,
 * stops the game loop (`stop()`: Graphics.stopGameLoop) and shows MZ's error screen
 * (Graphics.printError). The game is over: nothing runs any more. In Node the shim stands in for
 * the browser and turns the process's unhandled rejections and uncaught exceptions into those
 * window events (index.ts). Simplified: MZ's other handlers there (F5 reload, F8 dev tools, the
 * unload) are not modelled.
 */
import type { Ctx } from "./context.ts";

export function createSceneManager(ctx: Ctx, onLoop: (stop: () => void) => void) {
  const g = ctx.g;
  return {
    _scene: null as any,
    _nextScene: null as any,
    _stack: [] as any[],
    _exiting: false,
    _previousClass: null as any,

    _shimHandlers: false,

    // SceneManager.run(sceneClass)
    run(sceneClass: new () => unknown): void {
      this.initialize();
      this.goto(sceneClass);
      const raf = g.requestAnimationFrame as ((cb: () => void) => number) | undefined;
      if (typeof raf !== "function") return;
      let id = 0;
      let stopped = false;
      const loop = () => {
        if (stopped || g.Graphics._shimStopped) return;
        try {
          this.update();
        } finally {
          id = raf.call(g, loop);
        }
      };
      id = raf.call(g, loop);
      onLoop(() => {
        stopped = true;
        g.cancelAnimationFrame?.(id);
      });
    },
    // SceneManager.initialize(). Modelled: only setupEventHandlers (no graphics, audio, video or
    // input set-up: the shim made those at install).
    initialize(): void {
      this.setupEventHandlers();
    },
    // SceneManager.setupEventHandlers(): the window's "error" and "unhandledrejection" (once here)
    setupEventHandlers(): void {
      if (this._shimHandlers) return;
      this._shimHandlers = true;
      ctx.listen(ctx.win, "error", (event: any) => this.onError(event));
      ctx.listen(ctx.win, "unhandledrejection", (event: any) => this.onReject(event));
    },
    // SceneManager.onError(event). Modelled behaviour: the error's message goes to the console, the
    // game stops (no frame runs again) and MZ's error screen shows it.
    onError(event: any): void {
      const error = event?.error ?? event?.reason ?? event?.message;
      const message = error instanceof Error ? `${error.name}: ${error.message}` : String(event?.message ?? error);
      ctx.errors.push(error);
      console.error("mz shim: SceneManager.onError stopped the game:", error instanceof Error ? (error.stack ?? message) : message);
      try {
        this.stop();
        g.Graphics.printError("Error", message, error);
      } catch {
        // MZ ignores a failure while it shows the error
      }
    },
    // SceneManager.onReject(event): a rejection nobody handled is an error like any other
    onReject(event: any): void {
      this.onError({ error: event?.reason, message: event?.reason instanceof Error ? event.reason.message : String(event?.reason) });
    },
    // SceneManager.stop(): the game loop stops
    stop(): void {
      g.Graphics.stopGameLoop();
    },
    // SceneManager.update(deltaTime)
    update(_deltaTime?: number): void {
      this.updateMain();
    },
    // SceneManager.updateMain()
    updateMain(): void {
      this.updateFrameCount();
      this.updateInputData();
      this.changeScene();
      this.updateScene();
    },
    // SceneManager.updateFrameCount()
    updateFrameCount(): void {
      g.Graphics.frameCount++;
    },
    // SceneManager.updateInputData()
    updateInputData(): void {
      g.Input.update();
      g.TouchInput.update();
    },
    // SceneManager.changeScene(). Modelled behaviour: once a change is asked for and the current
    // scene is not busy, the current scene is terminated, the next one becomes current and is
    // created, and an exit then ends the game.
    changeScene(): void {
      if (!this.isSceneChanging() || this.isCurrentSceneBusy()) return;
      const leaving = this._scene;
      if (leaving) {
        leaving.terminate();
        this.onSceneTerminate();
      }
      const arriving = this._nextScene;
      this._nextScene = null;
      this._scene = arriving;
      if (arriving) {
        arriving.create();
        this.onSceneCreate();
      }
      if (this._exiting) this.terminate();
    },
    // SceneManager.updateScene(). Modelled behaviour: a scene that has not started starts on the
    // first frame it is ready; a started one updates while the game is active.
    updateScene(): void {
      const scene = this._scene;
      if (!scene) return;
      if (scene.isStarted()) {
        if (this.isGameActive()) scene.update();
        return;
      }
      if (!scene.isReady()) return;
      this.onBeforeSceneStart();
      scene.start();
      this.onSceneStart();
    },
    // SceneManager.isGameActive(): MZ also pauses an unfocused NW.js window
    isGameActive(): boolean {
      return true;
    },
    // SceneManager.onSceneTerminate / onSceneCreate / onBeforeSceneStart / onSceneStart
    onSceneTerminate(): void {
      this._previousClass = this._scene.constructor;
    },
    onSceneCreate(): void {},
    onBeforeSceneStart(): void {},
    onSceneStart(): void {},
    // SceneManager.isSceneChanging()
    isSceneChanging(): boolean {
      return this._exiting || !!this._nextScene;
    },
    // SceneManager.isCurrentSceneBusy()
    isCurrentSceneBusy(): boolean {
      return !!this._scene && this._scene.isBusy();
    },
    // SceneManager.isNextScene(sceneClass) / isPreviousScene(sceneClass)
    isNextScene(sceneClass: unknown): boolean {
      return !!this._nextScene && this._nextScene.constructor === sceneClass;
    },
    isPreviousScene(sceneClass: unknown): boolean {
      return this._previousClass === sceneClass;
    },
    // SceneManager.goto(sceneClass)
    goto(sceneClass: (new () => unknown) | null): void {
      if (sceneClass) this._nextScene = new sceneClass();
      if (this._scene) this._scene.stop();
    },
    // SceneManager.push(sceneClass)
    push(sceneClass: new () => unknown): void {
      this._stack.push(this._scene.constructor);
      this.goto(sceneClass);
    },
    // SceneManager.pop()
    pop(): void {
      if (this._stack.length > 0) this.goto(this._stack.pop());
      else this.exit();
    },
    // SceneManager.exit()
    exit(): void {
      this.goto(null);
      this._exiting = true;
    },
    // SceneManager.terminate(): MZ closes the window
    terminate(): void {},
  };
}
