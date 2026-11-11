// Stablecoin for Games checkout page: Solana. Finds wallet extensions through the Wallet Standard
// (Phantom, Solflare, Backpack, ...), asks the game to build the transfer, has the wallet
// sign and send it, and reports the signature to the game, which verifies it on-chain.
// What an extension registers or throws is never trusted to be well-formed: a broken wallet
// is left out or shown as a wallet error, never thrown into the page.
"use strict";

(function () {
  const ALPHABET = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

  function base58(bytes) {
    let n = 0n;
    for (const b of bytes) n = n * 256n + BigInt(b);
    let out = "";
    while (n > 0n) {
      out = ALPHABET[Number(n % 58n)] + out;
      n /= 58n;
    }
    for (const b of bytes) {
      if (b !== 0) break;
      out = "1" + out;
    }
    return out;
  }

  // Can this Wallet Standard wallet pay on `chain`, as this page lists it? Twin of bridge.js
  // canPaySolana(): the game offers "Browser wallet" for exactly the wallets this page shows.
  function canPay(w, chain) {
    try {
      return !!(w && Array.isArray(w.chains) && w.chains.includes(chain) &&
        w.features && w.features["standard:connect"] &&
        w.features["solana:signAndSendTransaction"] && typeof w.name === "string");
    } catch (err) {
      return false;                   // a wallet whose fields throw can't pay
    }
  }

  function fromBase64(text) {
    const bin = atob(text);
    const out = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
    return out;
  }

  function start(ctx) {
    const { session, $, setStatus, field } = ctx;
    const wallets = [];   // { wallet, name, icon }: the name and icon read once
    let wallet = null;
    let account = null;

    // The wallet as { wallet, name, icon } if it can pay on this chain, else null (also for
    // one whose fields throw).
    function usable(w) {
      if (!canPay(w, session.wallet_chain)) return null;
      try {
        const { name, icon } = w;
        if (typeof name !== "string") return null;   // a getter that changed its mind
        return { wallet: w, name, icon: typeof icon === "string" ? icon : "" };
      } catch (err) {
        return null;
      }
    }

    // The Wallet Standard's register(): a wallet calls it, so it never throws.
    function register(...list) {
      try {
        for (const w of list) {
          if (wallets.some((e) => e.wallet === w)) continue;
          const entry = usable(w);
          if (entry) wallets.push(entry);
        }
        render();
      } catch (err) { /* the wallets registered so far stay */ }
      return () => {};
    }

    // A click handler: whatever the flow throws ends as the wallet error state.
    function guarded(flow) {
      return (...args) => {
        flow(...args).catch((err) => {
          if (!ctx.isSent()) setStatus(walletError(err), "bad");
        });
      };
    }

    function walletError(err) {
      const msg = ctx.errorText(err);
      if (field(err, "code") === 4001 || /reject/i.test(msg)) {
        return "You rejected the request in your wallet. You can try again.";
      }
      if (/blockhash|expired/i.test(msg)) return "The request timed out. Click Pay to try again.";
      return "Wallet error: " + msg;
    }

    function render() {
      const list = $("wallets");
      list.textContent = "";
      if (ctx.isSent()) return;
      for (const { wallet: w, name, icon } of wallets) {
        const b = document.createElement("button");
        b.type = "button";
        if (icon.startsWith("data:image/")) {
          const img = document.createElement("img");
          img.src = icon;
          img.alt = "";
          b.appendChild(img);
        }
        b.appendChild(document.createTextNode("Pay with " + name));
        b.addEventListener("click", guarded(() => connect(w)));
        list.appendChild(b);
      }
      if (wallets.length > 0 && !account) setStatus("Choose your wallet.");
    }

    async function connect(w) {
      try {
        setStatus("Connecting to your wallet…");
        const res = await w.features["standard:connect"].connect();
        account = (res && res.accounts && res.accounts[0]) || (w.accounts && w.accounts[0]);
        if (!account) throw new Error("the wallet shared no account");
        wallet = w;
        if (ctx.isSent()) return;   // the payment was found while the wallet was connecting
        const pay = $("pay");
        pay.classList.remove("hidden");
        pay.disabled = false;
        pay.textContent = "Pay " + (session.price_text || session.amount_text) + " " + session.token;
        setStatus("Ready. Your wallet will ask you to confirm.");
      } catch (err) {
        if (!ctx.isSent()) setStatus(walletError(err), "bad");
      }
    }

    async function pay() {
      const button = $("pay");
      if (ctx.isSent()) return;
      button.disabled = true;
      let built, balance, solBalance;
      try {
        setStatus("Preparing the payment…");
        built = await ctx.postJSON("build", { account: account.address });
        if (!built) throw new Error("empty reply");
        balance = BigInt(built.token_balance);
        solBalance = BigInt(built.sol_balance);
      } catch (err) {
        if (ctx.isSent()) return;
        setStatus("The game couldn't prepare the payment. Check your connection and try again.", "bad");
        button.disabled = false;
        return;
      }
      // The payment was found while the game was building (e.g. an earlier send landed
      // late): never ask the wallet to sign a second one.
      if (ctx.isSent()) return;
      if (balance < BigInt(session.amount_units)) {
        setStatus("This wallet has " + ctx.formatUnits(balance, session.decimals) + " " +
          session.token + " on " + session.chain_name + ". You need " + session.amount_text + ".", "bad");
        button.disabled = false;
        return;
      }
      if (solBalance < 10000n) {
        setStatus("Warning: you have almost no SOL for the network fee. The transaction will probably fail.", "warn");
      } else {
        setStatus("Confirm the payment in your wallet…");
      }
      let signature;
      try {
        const [out] = await wallet.features["solana:signAndSendTransaction"].signAndSendTransaction({
          account, transaction: fromBase64(built.transaction), chain: session.wallet_chain,
        });
        signature = base58(out.signature);
      } catch (err) {
        if (ctx.isSent()) return;
        button.disabled = false;
        setStatus(walletError(err), "bad");
        return;
      }
      ctx.markSent(signature);
      render();
      setStatus("Sent! Waiting for the network to confirm…");
      try {
        await ctx.postJSON("tx", { tx_hash: signature, from: account.address });
      } catch (err) {
        // The payment is on its way; the game finds it by its reference without the signature.
        ctx.markUnreported();
        setStatus("Sent. The game will detect the payment; keep this page open.", "warn");
      }
    }

    // The game dropped a transaction that never landed: offer the wallets again, and Pay
    // if an account is connected (after a reload none is: the player picks the wallet).
    function retry() {
      const pay = $("pay");
      if (account) {
        pay.classList.remove("hidden");
        pay.disabled = false;
        pay.textContent = "Pay " + (session.price_text || session.amount_text) + " " + session.token;
      } else {
        pay.classList.add("hidden");
        pay.disabled = true;
      }
      render();
    }

    ctx.onRetry = retry;
    $("pay").addEventListener("click", guarded(pay));
    window.addEventListener("wallet-standard:register-wallet", (event) => {
      // a broken wallet must not break the page: neither its callback's throw nor its rejection
      try {
        Promise.resolve(event.detail({ register })).catch(() => {});
      } catch (err) { /* nothing registered */ }
    });
    window.dispatchEvent(new CustomEvent("wallet-standard:app-ready", { detail: { register } }));
    setTimeout(() => {
      render();
      if (wallets.length === 0) {
        setStatus("No Solana wallet extension found in this browser. Install Phantom, Solflare or " +
          "Backpack, or use the QR code in the game.", "warn");
      }
    }, 600);
  }

  window.scfgFamilies = window.scfgFamilies || {};
  window.scfgFamilies.solana = { start };
})();
