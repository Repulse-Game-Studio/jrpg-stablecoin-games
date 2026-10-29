// cryptopay checkout page: shared UI.
//
// Runs in two places:
//  * the localhost page the desktop game serves on http://127.0.0.1:<port>/c/<secret>/,
//    talking to the game over HTTP (httpChannel below);
//  * Ren'Py's web build, as an overlay over the game in the game's own page (bridge.js),
//    talking to the game through an in-page message queue.
// It shows what is being paid, polls the game for the payment's status, and hands the
// wallet part to the module for the session's chain family (evm.js, solana.js).
// No libraries, no build step.
"use strict";

(function () {
  function formatUnits(value, decimals) {
    const s = value.toString().padStart(decimals + 1, "0");
    return s.slice(0, -decimals) + "." + s.slice(-decimals);
  }

  // The localhost page's channel: the game's CheckoutServer, same origin.
  function httpChannel() {
    async function postJSON(path, body) {
      const r = await fetch(path, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body),
      });
      if (!r.ok) {
        const e = new Error("game rejected request (" + r.status + ")");
        e.status = r.status;
        throw e;
      }
      const type = r.headers.get("Content-Type") || "";
      return type.startsWith("application/json") ? r.json() : null;
    }

    return {
      embedded: false,
      logoUrl: "logo",
      async loadSession() {
        const r = await fetch("session.json", { cache: "no-store" });
        return r.json();
      },
      async getStatus() {
        const r = await fetch("status", { cache: "no-store" });
        if (!r.ok) throw new Error(String(r.status));
        return r.json();
      },
      post: postJSON,
    };
  }

  // channel: { embedded, logoUrl, loadSession(), getStatus(), post(path, body) }
  // root: the document, or the overlay's shadow root (both have getElementById).
  function start(channel, root) {
    const $ = (id) => root.getElementById(id);
    let session = null;
    let sent = false;
    let sends = 0;      // counts markSent() calls, to ignore statuses fetched before a new send
    let current = null; // signature of the page's current send, if the game may be told of it
    let acked = false;  // the game's status named `current` as the transaction it is checking
    let stopped = false;
    const ctx = {};

    function setStatus(text, kind) {
      const el = $("status");
      el.textContent = text;
      el.className = kind || "";
    }

    function hidePay() {
      $("pay").classList.add("hidden");
      $("pay").disabled = true;
      $("wallets").textContent = "";
    }

    // `signature`: the transaction just sent (Solana). Only a send the game acknowledged
    // can be offered again if it never lands.
    function markSent(signature) {
      sent = true;
      sends += 1;
      current = signature || null;
      acked = false;
      hidePay();
    }

    // The game was not told of the current send; it can find it only by its reference, so
    // it can never say that this send didn't land: Pay is never offered again for it.
    function markUnreported() {
      current = null;
    }

    function isSent() {
      return sent;
    }

    // ------------------------------------------------------------ status from the game

    // A wallet returns the signature before the transaction lands. If it never lands
    // (Solana only), the game drops it and the player may pay again. The family module
    // decides what to show (Pay for a connected account, else the wallet list).
    function allowRetry() {
      sent = false;
      current = null;
      acked = false;
      if (ctx.onRetry) ctx.onRetry();
      setStatus("That transaction didn't reach the network. Click Pay to try again.", "warn");
    }

    // Pay comes back only if the game acknowledged this page's current send and then said
    // that this very send didn't land. Anything else (a status from before the game heard
    // of the send, a send it never heard of) keeps Pay hidden: the game still finds a
    // payment that landed by its reference.
    function droppedOurSend(s, fresh) {
      return sent && fresh && acked && current !== null && s.dropped_tx === current;
    }

    async function pollStatus() {
      if (stopped) return;
      try {
        const seen = sends;
        const s = await channel.getStatus();
        const fresh = seen === sends;           // else: fetched before a newer send
        if (s.tx_hash && !sent) {               // e.g. the page was reloaded after paying
          markSent(s.tx_hash);
          acked = true;
        } else if (fresh && sent && current !== null && s.tx_hash === current) {
          acked = true;
        }
        if (["confirming", "paid", "underpaid"].includes(s.state)) {
          sent = true;                          // found, maybe without this page: never pay again
          hidePay();
        }
        if (s.state === "waiting" && s.error === "tx_not_landed" && !s.tx_hash) {
          if (droppedOurSend(s, fresh)) allowRetry();
          setTimeout(pollStatus, 2000);
          return;
        }
        if (s.state === "paid") {
          setStatus(channel.embedded ? "Payment confirmed. Thank you!"
            : "Payment confirmed. Thank you! You can close this tab and return to the game.", "ok");
          return;
        }
        if (s.state === "underpaid") {
          setStatus("The payment received was less than the price. Please contact the developer.", "bad");
          return;
        }
        if (s.state === "expired") {
          setStatus("The payment window closed. If you already paid, the game will still detect it.", "warn");
        } else if (s.state === "confirming") {
          setStatus("Confirming on " + session.chain_name + "… " + s.confirmations + "/" + s.required);
        } else if (sent && s.error === "network_unavailable") {
          setStatus("The game can't reach the network right now; it will keep checking.", "warn");
        } else if (sent && s.error) {
          setStatus("The game couldn't verify that transaction (" + s.error + ").", "bad");
        }
      } catch (err) {
        if (sent) setStatus("Lost contact with the game. Is it still running?", "warn");
      }
      setTimeout(pollStatus, 2000);
    }

    // ------------------------------------------------------------ appearance

    // The developer's accent color and logo (validated by the game's config).
    function applyAppearance() {
      const hex = /^#[0-9a-f]{6}$/;
      const style = root.host ? root.host.style : document.documentElement.style;
      if (hex.test(session.accent || "")) style.setProperty("--accent", session.accent);
      if (hex.test(session.on_accent || "")) style.setProperty("--on-accent", session.on_accent);
      if (session.logo && channel.logoUrl) {
        const img = $("logo");
        img.addEventListener("load", () => img.classList.remove("hidden"));
        img.src = channel.logoUrl;
      }
    }

    // ------------------------------------------------------------ start

    async function begin() {
      try {
        session = await channel.loadSession();
      } catch (err) {
        setStatus("This checkout link has expired. Start the payment again from the game.", "bad");
        return;
      }
      if (channel.embedded) {
        $("footer").textContent = "The payment goes directly from your wallet to the developer.";
      } else {
        document.title = "Pay: " + session.title;
      }
      applyAppearance();
      $("title").textContent = session.title;
      $("subtitle").textContent = session.kind === "tip" ? "Tip the developer" : "Unlock purchase";
      const token = document.createElement("span");
      token.className = "token";
      token.textContent = session.token;
      $("amount").replaceChildren(document.createTextNode(session.amount_text), token);
      $("network").textContent = "on " + session.chain_name;
      if (session.testnet) $("testnet").classList.remove("hidden");

      const families = window.cryptopayFamilies || {};
      const family = families[session.family || "evm"];
      if (!family) {
        setStatus("This checkout page can't pay on " + session.chain_name + ".", "bad");
        return;
      }
      Object.assign(ctx, { session, $, setStatus, formatUnits, postJSON: channel.post, markSent,
        markUnreported, isSent, onRetry: null });
      family.start(ctx);
      pollStatus();
    }

    begin();
    return { stop() { stopped = true; } };
  }

  window.cryptopayCheckout = { start, httpChannel };
  if (!window.cryptopayEmbedded) start(httpChannel(), document);
})();
