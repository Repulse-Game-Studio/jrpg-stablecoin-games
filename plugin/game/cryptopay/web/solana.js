// cryptopay checkout page: Solana. Finds wallet extensions through the Wallet Standard
// (Phantom, Solflare, Backpack, ...), asks the game to build the transfer, has the wallet
// sign and send it, and reports the signature to the game, which verifies it on-chain.
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

  function fromBase64(text) {
    const bin = atob(text);
    const out = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
    return out;
  }

  function start(ctx) {
    const { session, $, setStatus } = ctx;
    const wallets = [];
    let wallet = null;
    let account = null;

    function usable(w) {
      return w && Array.isArray(w.chains) && w.chains.includes(session.wallet_chain) &&
        w.features && w.features["standard:connect"] &&
        w.features["solana:signAndSendTransaction"];
    }

    function register(...list) {
      for (const w of list) if (usable(w) && !wallets.includes(w)) wallets.push(w);
      render();
      return () => {};
    }

    function walletError(err) {
      const msg = (err && err.message) || String(err);
      if ((err && err.code === 4001) || /reject/i.test(msg)) {
        return "You rejected the request in your wallet. You can try again.";
      }
      if (/blockhash|expired/i.test(msg)) return "The request timed out. Click Pay to try again.";
      return "Wallet error: " + msg;
    }

    function render() {
      const list = $("wallets");
      list.textContent = "";
      if (ctx.isSent()) return;
      for (const w of wallets) {
        const b = document.createElement("button");
        b.type = "button";
        if (typeof w.icon === "string" && w.icon.startsWith("data:image/")) {
          const img = document.createElement("img");
          img.src = w.icon;
          img.alt = "";
          b.appendChild(img);
        }
        b.appendChild(document.createTextNode("Pay with " + w.name));
        b.addEventListener("click", () => connect(w));
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
        pay.textContent = "Pay " + session.amount_text + " " + session.token;
        setStatus("Ready. Your wallet will ask you to confirm.");
      } catch (err) {
        if (!ctx.isSent()) setStatus(walletError(err), "bad");
      }
    }

    async function pay() {
      const button = $("pay");
      if (ctx.isSent()) return;
      button.disabled = true;
      let built;
      try {
        setStatus("Preparing the payment…");
        built = await ctx.postJSON("build", { account: account.address });
        if (!built) throw new Error("empty reply");
      } catch (err) {
        if (ctx.isSent()) return;
        setStatus("The game couldn't prepare the payment. Check your connection and try again.", "bad");
        button.disabled = false;
        return;
      }
      // The payment was found while the game was building (e.g. an earlier send landed
      // late): never ask the wallet to sign a second one.
      if (ctx.isSent()) return;
      const balance = BigInt(built.token_balance);
      if (balance < BigInt(session.amount_units)) {
        setStatus("This wallet has " + ctx.formatUnits(balance, session.decimals) + " " +
          session.token + " on " + session.chain_name + ". You need " + session.amount_text + ".", "bad");
        button.disabled = false;
        return;
      }
      if (BigInt(built.sol_balance) < 10000n) {
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
        pay.textContent = "Pay " + session.amount_text + " " + session.token;
      } else {
        pay.classList.add("hidden");
        pay.disabled = true;
      }
      render();
    }

    ctx.onRetry = retry;
    $("pay").addEventListener("click", pay);
    window.addEventListener("wallet-standard:register-wallet", (event) => {
      try { event.detail({ register }); } catch (err) { /* a broken wallet must not break the page */ }
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

  window.cryptopayFamilies = window.cryptopayFamilies || {};
  window.cryptopayFamilies.solana = { start };
})();
