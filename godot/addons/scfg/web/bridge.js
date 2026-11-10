// Stablecoin for Games: the game's side of the browser in Ren'Py's web build.
//
// scfg_core/web_page.py loads this file, then evm.js, solana.js and app.js, into the
// game's own page (emscripten.run_script). The game's Python can't block or use threads
// there, so everything here is asynchronous and the game polls for results every frame:
//   * post/result: JSON-RPC requests through fetch(), for pump_worker.PumpWorker;
//   * hasWallet: wallet extensions seen in the page (EIP-6963, window.ethereum, Wallet
//     Standard), so the game offers "Browser wallet" only when one is there;
//   * openCheckout and friends: the checkout page (app.js) over the game in a shadow
//     root, talking to the game through a message queue instead of HTTP;
//   * copy, requestPaste/takePaste, openUrl: what Ren'Py can't do on the web.
// Values from the game arrive as JSON literals; nothing here evaluates game data as code.
"use strict";

(function () {
  if (window.scfgBridge) return;
  window.scfgEmbedded = true;   // app.js: don't start the localhost page

  // ---------------------------------------------------------------- requests

  let nextRequest = 1;
  const results = new Map();          // id -> null while in flight, then the envelope

  function post(url, body, timeoutMs) {
    const id = nextRequest++;
    results.set(id, null);
    const abort = new AbortController();
    const timer = setTimeout(() => abort.abort(), timeoutMs);
    fetch(url, {
      method: "POST", body, signal: abort.signal, cache: "no-store", credentials: "omit",
      referrerPolicy: "no-referrer", headers: { "Content-Type": "application/json" },
    })
      .then(async (r) => ({ status: r.status, body: await r.text() }))
      .catch((err) => ({ error: abort.signal.aborted ? "timed out"
        : String((err && err.message) || err) }))
      .then((envelope) => {
        clearTimeout(timer);
        if (results.has(id)) results.set(id, envelope);
      });
    return id;
  }

  // "" while the request is in flight; its envelope as JSON once, then it is forgotten.
  function result(id) {
    if (!results.has(id)) return JSON.stringify({ error: "unknown request" });
    const envelope = results.get(id);
    if (envelope === null) return "";
    results.delete(id);
    return JSON.stringify(envelope);
  }

  // ---------------------------------------------------------------- wallets

  const evmWallets = new Set();
  const solanaWallets = [];

  window.addEventListener("eip6963:announceProvider", (event) => {
    const { info, provider } = event.detail || {};
    if (info && provider) evmWallets.add(info.uuid);
  });
  window.dispatchEvent(new Event("eip6963:requestProvider"));

  function registerSolana(...list) {
    for (const w of list) if (w && !solanaWallets.includes(w)) solanaWallets.push(w);
    return () => {};
  }
  window.addEventListener("wallet-standard:register-wallet", (event) => {
    try { event.detail({ register: registerSolana }); } catch (err) { /* a broken wallet must not break the game */ }
  });
  window.dispatchEvent(new CustomEvent("wallet-standard:app-ready",
    { detail: { register: registerSolana } }));

  // 1 if the page has a wallet that can pay on this chain (the tests evm.js/solana.js use).
  function hasWallet(family, walletChain) {
    if (family === "solana") {
      return solanaWallets.some((w) => Array.isArray(w.chains) && w.chains.includes(walletChain) &&
        w.features && w.features["standard:connect"] &&
        w.features["solana:signAndSendTransaction"]) ? 1 : 0;
    }
    return evmWallets.size > 0 || window.ethereum ? 1 : 0;
  }

  // ---------------------------------------------------------------- checkout overlay

  const OVERLAY_CSS = `
    .cp-body { background: rgba(23, 32, 46, .55); }
    main { position: relative; }
    #cp-close { position: absolute; top: 12px; right: 12px; width: 34px; height: 34px; padding: 0;
                justify-content: center; border: none; background: transparent;
                color: var(--muted); font-size: 22px; font-weight: 400; }
  `;
  let page = null;                    // { style, main } from index.html
  const overlays = new Map();         // key -> { host, queue, status, replies, nextReply, checkout }

  function setPage(html) {
    const doc = new DOMParser().parseFromString(html, "text/html");
    page = { style: doc.querySelector("style").textContent,
             main: doc.querySelector("main").outerHTML };
  }

  function hide(key) {
    const o = overlays.get(key);
    if (o) o.host.style.display = "none";
  }

  function show(key) {
    const o = overlays.get(key);
    if (!o) return;
    // Nothing outside a fullscreen element can be seen: leave fullscreen first.
    const full = document.fullscreenElement;
    if (full && !full.contains(o.host)) document.exitFullscreen().catch(() => {});
    o.host.style.display = "";
  }

  function openCheckout(key, info, logoUrl) {
    if (overlays.has(key)) {
      show(key);
      return;
    }
    const host = document.createElement("div");
    host.style.cssText = "position:fixed;inset:0;z-index:2147483000;overflow:auto;";
    const root = host.attachShadow({ mode: "open" });
    root.innerHTML = "<style>" + page.style + OVERLAY_CSS + "</style>" +
      "<div class=\"cp-body\">" + page.main + "</div>";
    const close = document.createElement("button");
    close.id = "cp-close";
    close.type = "button";
    close.textContent = "×";
    close.setAttribute("aria-label", "Back to the game");
    close.addEventListener("click", () => hide(key));
    root.querySelector("main").prepend(close);
    const o = { host, queue: [], status: null, replies: new Map(), nextReply: 1, checkout: null };
    overlays.set(key, o);
    document.body.appendChild(host);
    show(key);
    const channel = {
      embedded: true,
      logoUrl: logoUrl || null,
      loadSession: async () => info,
      getStatus: async () => o.status || { state: "waiting" },
      post(path, body) {
        if (path === "tx") {
          o.queue.push({ type: "tx", body });
          setTimeout(() => hide(key), 1500);  // the game's own sheet shows the rest
          return Promise.resolve({ ok: true });
        }
        return new Promise((resolve, reject) => {
          const id = o.nextReply++;
          o.replies.set(id, { resolve, reject });
          o.queue.push({ type: path, id, body });
        });
      },
    };
    o.checkout = window.scfgCheckout.start(channel, root);
  }

  function take(key) {
    const o = overlays.get(key);
    return JSON.stringify(o ? o.queue.splice(0) : []);
  }

  function setStatus(key, status) {
    const o = overlays.get(key);
    if (o) o.status = status;
  }

  function reply(key, id, status, body) {
    const o = overlays.get(key);
    const waiting = o && o.replies.get(id);
    if (!waiting) return;
    o.replies.delete(id);
    if (status >= 200 && status < 300) {
      waiting.resolve(body);
    } else {
      const e = new Error("game rejected request (" + status + ")");
      e.status = status;
      waiting.reject(e);
    }
  }

  function closeCheckout(key) {
    const o = overlays.get(key);
    if (!o) return;
    overlays.delete(key);
    if (o.checkout) o.checkout.stop();
    o.host.remove();
  }

  // ---------------------------------------------------------------- clipboard and links

  let copyFailed = false;

  function copyFallback(text) {
    const focused = document.activeElement;
    const area = document.createElement("textarea");
    area.value = text;
    area.setAttribute("readonly", "");
    area.style.cssText = "position:fixed;top:-1000px;opacity:0;";
    document.body.appendChild(area);
    area.select();
    let ok = false;
    try { ok = document.execCommand("copy"); } catch (err) { /* nothing more to try */ }
    if (!ok) copyFailed = true;
    area.remove();
    if (focused && focused.focus) focused.focus();
  }

  function copy(text) {
    if (navigator.clipboard && window.isSecureContext) {
      navigator.clipboard.writeText(text).catch(() => copyFallback(text));
    } else {
      copyFallback(text);
    }
  }

  // 1 once after a copy the browser refused (the game tells the player), else 0.
  function takeCopyFailed() {
    const failed = copyFailed;
    copyFailed = false;
    return failed ? 1 : 0;
  }

  let pasted = null;                  // null: no answer yet; "": the browser refused

  function requestPaste() {
    pasted = null;
    if (!(navigator.clipboard && navigator.clipboard.readText && window.isSecureContext)) {
      pasted = "";
      return;
    }
    navigator.clipboard.readText().then((text) => { pasted = text || ""; }, () => { pasted = ""; });
  }

  function takePaste() {
    const text = pasted;
    if (text !== null) pasted = null;
    return JSON.stringify(text);
  }

  function openUrl(url) {
    window.open(url, "_blank", "noopener");
  }

  // Ren'Py's run_script* is a bare eval: an exception here would unwind through the game's
  // Python and wasm and could abort it. Every entry point catches, logs, and returns what
  // Python expects for "nothing happened" (fallback: a value, or a function of the error).
  function safe(name, fn, fallback) {
    return function (...args) {
      try {
        return fn(...args);
      } catch (err) {
        console.error("scfg: " + name + " failed", err);
        return typeof fallback === "function" ? fallback(err) : fallback;
      }
    };
  }

  function failedRequest(err) {
    const id = nextRequest++;
    results.set(id, { error: String((err && err.message) || err) });
    return id;
  }

  const unknownRequest = JSON.stringify({ error: "bridge error" });

  window.scfgBridge = {
    post: safe("post", post, failedRequest),
    result: safe("result", result, unknownRequest),
    hasWallet: safe("hasWallet", hasWallet, 0),
    setPage: safe("setPage", setPage),
    openCheckout: safe("openCheckout", openCheckout),
    show: safe("show", show),
    hide: safe("hide", hide),
    take: safe("take", take, "[]"),
    setStatus: safe("setStatus", setStatus),
    reply: safe("reply", reply),
    closeCheckout: safe("closeCheckout", closeCheckout),
    copy: safe("copy", copy),
    takeCopyFailed: safe("takeCopyFailed", takeCopyFailed, 0),
    requestPaste: safe("requestPaste", requestPaste),
    takePaste: safe("takePaste", takePaste, JSON.stringify("")),
    openUrl: safe("openUrl", openUrl),
  };
})();
