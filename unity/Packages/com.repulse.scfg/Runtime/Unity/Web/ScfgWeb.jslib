// Stablecoin for Games: the WebGL player's way into its own page (the twin of Godot's JavaScriptBridge.eval and of
// Ren'Py's emscripten.run_script). Runtime/Unity/Web/ScfgBrowserJs.cs calls these through [DllImport("__Internal")].
//
//   ScfgWebLoad(code)            runs one of the checkout page's scripts (bridge.js, evm.js, solana.js, app.js, copies
//                                of plugin/game/scfg/web/) in the page's global scope; 1, or 0 when it threw.
//   ScfgWebCall(name, argsJson)  window.scfgBridge[name](...JSON.parse(argsJson)): JSON.stringify of what it returned
//                                (undefined is null); 0 (a null string) when there is no such function or it threw.
//   ScfgWebStorage(op, key, v)   localStorage: "get" answers {"value": <text or null>}, "set" answers {}; either answers
//                                {"error": "<name>: <message>"} when localStorage is missing, blocked or full.
//   ScfgWebSyncFs()              queue a write of Application.persistentDataPath (Unity's IDBFS mount) to IndexedDB.
//   ScfgWebFsPersistent()        1 when the page has IndexedDB (so persistentDataPath outlives the tab), else 0.
//
// Values cross as JSON text, never spliced into JavaScript code. Nothing here may throw into the player: every entry
// point catches. Strings go back as _malloc'd UTF-8, which the IL2CPP marshaller frees.
var ScfgWebLib = {
  $ScfgWeb__deps: ['$UTF8ToString', '$stringToUTF8', '$lengthBytesUTF8'],
  $ScfgWeb: {
    str: function (ptr) {
      return ptr ? UTF8ToString(ptr) : null;
    },
    ret: function (text) {
      if (typeof text !== 'string') return 0;
      var size = lengthBytesUTF8(text) + 1;
      var buffer = _malloc(size);
      stringToUTF8(text, buffer, size);
      return buffer;
    },
    // What a thrown value says, without ever throwing itself (bridge.js's errorText).
    errorText: function (err) {
      try {
        if (err && err.name && err.message) return String(err.name) + ': ' + String(err.message);
      } catch (e) { /* a getter that throws */ }
      try {
        return String(err);
      } catch (e) {
        return 'error';
      }
    },
  },

  ScfgWebLoad__deps: ['$ScfgWeb'],
  ScfgWebLoad: function (code) {
    try {
      (0, eval)(ScfgWeb.str(code) || '');
      return 1;
    } catch (err) {
      console.error('scfg: a checkout page script failed: ' + ScfgWeb.errorText(err));
      return 0;
    }
  },

  ScfgWebCall__deps: ['$ScfgWeb'],
  ScfgWebCall: function (name, argsJson) {
    try {
      var bridge = window.scfgBridge;
      var fn = bridge ? bridge[ScfgWeb.str(name)] : undefined;
      if (typeof fn !== 'function') return 0;
      var args = JSON.parse(ScfgWeb.str(argsJson) || '[]');
      var result = fn.apply(bridge, Array.isArray(args) ? args : []);
      return ScfgWeb.ret(JSON.stringify(result === undefined ? null : result));
    } catch (err) {
      console.error('scfg: scfgBridge.' + ScfgWeb.str(name) + ' failed: ' + ScfgWeb.errorText(err));
      return 0;
    }
  },

  ScfgWebStorage__deps: ['$ScfgWeb'],
  ScfgWebStorage: function (op, key, value) {
    var answer;
    try {
      var s = window.localStorage;
      if (!s) {
        answer = { error: 'no localStorage' };
      } else if (ScfgWeb.str(op) === 'set') {
        s.setItem(ScfgWeb.str(key), ScfgWeb.str(value) || '');
        answer = {};
      } else {
        answer = { value: s.getItem(ScfgWeb.str(key)) };
      }
    } catch (err) {
      answer = { error: ScfgWeb.errorText(err) };
    }
    try {
      return ScfgWeb.ret(JSON.stringify(answer));
    } catch (err) {
      return 0;
    }
  },

  ScfgWebSyncFs: function () {
    try {
      // Unity's own sync (lib/FileSystem.js JS_FileSystem_Sync), without its deprecation warning: batched with any
      // automatic persistence (config.autoSyncPersistentDataPath) already queued.
      if (Module.__unityIdbfsMount && typeof IDBFS !== 'undefined' && IDBFS.queuePersist) {
        IDBFS.queuePersist(Module.__unityIdbfsMount.mount);
        return 1;
      }
      if (typeof FS !== 'undefined' && FS.syncfs) {
        FS.syncfs(false, function () {});
        return 1;
      }
    } catch (err) { /* nothing more to try */ }
    return 0;
  },

  ScfgWebFsPersistent: function () {
    try {
      if (typeof indexedDB === 'undefined' || !indexedDB) return 0;
      // Unity's own mount (prejs/IdbFs.js) populates persistentDataPath from IndexedDB before the game starts: that
      // first sync opens the database, and emscripten's IDBFS caches it in IDBFS.dbs only when the open succeeded. A
      // browser that has indexedDB but refuses the database (blocked site data, a partitioned third-party iframe, some
      // private modes) leaves no entry: Unity then only logs "IndexedDB is not available" and runs on memory.
      var mount = Module.__unityIdbfsMount && Module.__unityIdbfsMount.mount;
      if (mount && typeof IDBFS !== 'undefined' && IDBFS.dbs) return IDBFS.dbs[mount.mountpoint] ? 1 : 0;
      return 1; // a page with its own Module.unityFileSystemInit: nothing to tell, so not warned about
    } catch (err) {
      return 0; // a sandboxed iframe can throw on the access itself
    }
  },
};

mergeInto(LibraryManager.library, ScfgWebLib);
