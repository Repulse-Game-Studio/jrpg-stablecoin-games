// Stablecoin for Games checkout page: EVM chains. Finds browser wallet extensions (EIP-6963),
// asks the chosen one to send one ERC-20 transfer, and reports the tx hash to the game.
// What an extension announces or throws is never trusted to be well-formed: a broken one is
// left out or shown as a wallet error, never thrown into the page.
"use strict";

(function () {
  function start(ctx) {
    const { session, $, setStatus, field } = ctx;
    // Kept free for the network fee when the token is also the gas coin: 0.01 token.
    const FEE_MARGIN_UNITS = 10000n;
    let provider = null;
    let account = null;
    const discovered = new Map();

    function pad32(hexNo0x) {
      return hexNo0x.toLowerCase().padStart(64, "0");
    }

    // ERC-20 calldata: 4-byte selector + 32-byte arguments.
    function transferData(recipient, units) {
      return "0xa9059cbb" + pad32(recipient.slice(2)) + pad32(BigInt(units).toString(16));
    }

    function balanceOfData(owner) {
      return "0x70a08231" + pad32(owner.slice(2));
    }

    function walletError(err) {
      const code = field(err, "code");
      if (code === 4001) return "You rejected the request in your wallet. You can try again.";
      if (code === -32002) return "Your wallet already has a pending request. Open it to continue.";
      return "Wallet error: " + ctx.errorText(err);
    }

    // A click handler: whatever the flow throws ends as the wallet error state.
    function guarded(flow) {
      return (...args) => {
        flow(...args).catch((err) => {
          if (!ctx.isSent()) setStatus(walletError(err), "bad");
        });
      };
    }

    // An extension's window.ethereum (its getter may throw), else null. Twin of bridge.js
    // injected().
    function injected() {
      try {
        return window.ethereum || null;
      } catch (err) {
        return null;
      }
    }

    // An EIP-6963 announcement as { info: { uuid, name, icon }, provider } of plain values,
    // read once; null for one that isn't (no detail, no provider, fields that throw). Twin of
    // bridge.js announcedUuid(): the same announcements are wallets for the game's offer.
    function announced(event) {
      try {
        const { info, provider: p } = event.detail || {};
        if (!info || !p) return null;
        const { uuid, name, icon } = info;
        if (typeof uuid !== "string" || typeof name !== "string") return null;
        return { info: { uuid, name, icon: typeof icon === "string" ? icon : "" }, provider: p };
      } catch (err) {
        return null;
      }
    }

    function renderWallets() {
      const list = $("wallets");
      list.textContent = "";
      if (ctx.isSent()) return;
      const entries = [...discovered.values()];
      const fallback = entries.length === 0 ? injected() : null;
      if (fallback) {
        entries.push({ info: { name: "Browser wallet", icon: "" }, provider: fallback });
      }
      for (const { info, provider: p } of entries) {
        const b = document.createElement("button");
        b.type = "button";
        if (info.icon.startsWith("data:image/")) {
          const img = document.createElement("img");
          img.src = info.icon;
          img.alt = "";
          b.appendChild(img);
        }
        b.appendChild(document.createTextNode("Pay with " + info.name));
        b.addEventListener("click", guarded(() => connect(p)));
        list.appendChild(b);
      }
      if (entries.length > 0) {
        setStatus("Choose your wallet.");
      }
    }

    // ---------------------------------------------------------------- payment flow

    async function ensureChain(p) {
      const current = await p.request({ method: "eth_chainId" });
      if (parseInt(current, 16) === session.chain_id) return;
      try {
        await p.request({ method: "wallet_switchEthereumChain", params: [{ chainId: session.chain_id_hex }] });
      } catch (err) {
        if (field(err, "code") === 4902 || field(field(field(err, "data"), "originalError"), "code") === 4902) {
          await p.request({
            method: "wallet_addEthereumChain",
            params: [{
              chainId: session.chain_id_hex,
              chainName: session.chain_name,
              nativeCurrency: { name: session.native_symbol, symbol: session.native_symbol, decimals: 18 },
              rpcUrls: [session.rpc_url],
              blockExplorerUrls: [session.explorer_tx.split("/tx/")[0]],
            }],
          });
        } else {
          throw err;
        }
      }
    }

    async function connect(p) {
      provider = p;
      try {
        setStatus("Connecting to your wallet…");
        const accounts = await p.request({ method: "eth_requestAccounts" });
        account = accounts[0];
        setStatus("Switching to " + session.chain_name + "…");
        await ensureChain(p);
        await preflight();
      } catch (err) {
        if (!ctx.isSent()) setStatus(walletError(err), "bad");
      }
    }

    async function preflight() {
      const need = BigInt(session.amount_units);
      const bal = BigInt(await provider.request({
        method: "eth_call",
        params: [{ to: session.token_address, data: balanceOfData(account) }, "latest"],
      }));
      const gas = BigInt(await provider.request({ method: "eth_getBalance", params: [account, "latest"] }));
      if (ctx.isSent()) return;   // the payment was found while the wallet was answering
      const pay = $("pay");
      pay.classList.remove("hidden");
      pay.textContent = "Pay " + (session.price_text || session.amount_text) + " " + session.token;
      if (bal < need) {
        setStatus("This wallet has " + ctx.formatUnits(bal, session.decimals) + " " + session.token +
          " on " + session.chain_name + ". You need " + session.amount_text + ".", "bad");
        pay.disabled = true;
        return;
      }
      pay.disabled = false;
      if (session.gas_is_token) {
        // The fee is paid in the token itself (Arc: USDC), from the same balance.
        if (bal < need + FEE_MARGIN_UNITS) {
          setStatus("Warning: after this payment less than 0.01 " + session.token +
            " would be left for the network fee, which " + session.chain_name +
            " charges in " + session.token + ". The transaction may fail.", "warn");
        } else {
          setStatus("Ready. Your wallet will ask you to confirm.");
        }
      } else if (gas === 0n) {
        setStatus("Warning: you have no " + session.native_symbol + " on " + session.chain_name +
          " to pay the network fee. The transaction will probably fail.", "warn");
      } else {
        setStatus("Ready. Your wallet will ask you to confirm.");
      }
    }

    async function pay() {
      const button = $("pay");
      if (ctx.isSent()) return;
      button.disabled = true;
      let txHash;
      try {
        // The player may have switched networks in the wallet since connecting.
        await ensureChain(provider);
        if (ctx.isSent()) return;
        const tx = {
          from: account,
          to: session.token_address,
          value: "0x0",
          data: transferData(session.recipient, session.amount_units),
        };
        const minFee = BigInt(session.min_fee_wei || "0");
        if (minFee > 0n) {
          // Arc's mempool silently drops transactions priced under its floor.
          const price = BigInt(await provider.request({ method: "eth_gasPrice" }));
          const fee = 2n * price > minFee ? 2n * price : minFee;
          tx.maxFeePerGas = "0x" + fee.toString(16);
        }
        if (ctx.isSent()) return;
        setStatus("Confirm the payment in your wallet…");
        txHash = await provider.request({ method: "eth_sendTransaction", params: [tx] });
      } catch (err) {
        if (ctx.isSent()) return;
        button.disabled = false;
        setStatus(walletError(err), "bad");
        return;
      }
      ctx.markSent();
      renderWallets();
      setStatus("Sent! Waiting for the network to confirm…");
      try {
        await ctx.postJSON("tx", { tx_hash: txHash, from: account });
      } catch (err) {
        // The payment is on its way; the game finds it by its reference without the hash.
        setStatus("Sent. The game will detect the payment; keep this page open.", "warn");
      }
    }

    window.addEventListener("eip6963:announceProvider", (event) => {
      const wallet = announced(event);
      if (wallet && !discovered.has(wallet.info.uuid)) {
        discovered.set(wallet.info.uuid, wallet);
        renderWallets();
      }
    });
    $("pay").addEventListener("click", guarded(pay));
    window.dispatchEvent(new Event("eip6963:requestProvider"));
    setTimeout(() => {
      renderWallets();
      if (discovered.size === 0 && !injected()) {
        setStatus("No wallet extension found in this browser. Install MetaMask, Rabby or " +
          "Coinbase Wallet, or use the QR code in the game.", "warn");
      }
    }, 600);
  }

  window.scfgFamilies = window.scfgFamilies || {};
  window.scfgFamilies.evm = { start };
})();
