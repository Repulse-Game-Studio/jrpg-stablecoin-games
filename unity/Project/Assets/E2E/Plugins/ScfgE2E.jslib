// The web e2e driver's page side (E2EDriver.cs, THE WEB DRIVER): the config the runner put in window.scfgE2EConfig,
// and the command queue and reply list of window.scfgE2E / window.scfgE2EReplies. Dev project only, never in the package.
var ScfgE2ELib = {
  $ScfgE2E__deps: ['$UTF8ToString', '$stringToUTF8', '$lengthBytesUTF8'],
  $ScfgE2E: {
    ret: function (text) {
      if (typeof text !== 'string') return 0;
      var size = lengthBytesUTF8(text) + 1;
      var buffer = _malloc(size);
      stringToUTF8(text, buffer, size);
      return buffer;
    },
  },

  ScfgE2EConfigType__deps: ['$ScfgE2E'],
  ScfgE2EConfigType: function () {
    return ScfgE2E.ret(typeof window.scfgE2EConfig);
  },

  ScfgE2EConfig__deps: ['$ScfgE2E'],
  ScfgE2EConfig: function () {
    return ScfgE2E.ret(window.scfgE2EConfig);
  },

  ScfgE2EStart: function () {
    window.scfgE2EQueue = [];
    window.scfgE2EReplies = [];
    window.scfgE2EProblems = window.scfgE2EProblems || [];
    window.scfgE2E = function (line) {
      window.scfgE2EQueue.push(String(line));
    };
  },

  ScfgE2ETake__deps: ['$ScfgE2E'],
  ScfgE2ETake: function () {
    var q = window.scfgE2EQueue;
    return q && q.length ? ScfgE2E.ret(q.shift()) : 0;
  },

  ScfgE2EReply__deps: ['$UTF8ToString'],
  ScfgE2EReply: function (text) {
    window.scfgE2EReplies.push(UTF8ToString(text));
  },

  ScfgE2EProblem__deps: ['$UTF8ToString'],
  ScfgE2EProblem: function (text) {
    (window.scfgE2EProblems = window.scfgE2EProblems || []).push(UTF8ToString(text));
  },
};

mergeInto(LibraryManager.library, ScfgE2ELib);
