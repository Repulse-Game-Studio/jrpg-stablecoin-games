// Stablecoin for Games checkout page: EVM chains. Finds browser wallet extensions (EIP-6963),
// asks the chosen one to send one ERC-20 transfer, and reports the tx hash to the game.
"use strict";

(function () {
  function start(ctx) {
    const { session, $, setStatus } = ctx;
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
      if (err && err.code === 4001) return "You rejected the request in your wallet. You can try again.";
      if (err && err.code === -32002) return "Your wallet already has a pending request. Open it to continue.";
      return "Wallet error: " + ((err && err.message) || String(err));
    }

    function renderWallets() {
      const list = $("wallets");
      list.textContent = "";
      if (ctx.isSent()) return;
      const entries = [...discovered.values()];
      if (entries.length === 0 && window.ethereum) {
        entries.push({ info: { name: "Browser wallet", icon: "" }, provider: window.ethereum });
      }
      for (const { info, provider: p } of entries) {
        const b = document.createElement("button");
        b.type = "button";
        if (typeof info.icon === "string" && info.icon.startsWith("data:image/")) {
          const img = document.createElement("img");
          img.src = info.icon;
          img.alt = "";
          b.appendChild(img);
        }
        b.appendChild(document.createTextNode("Pay with " + info.name));
        b.addEventListener("click", () => connect(p));
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
        if (err && (err.code === 4902 || (err.data && err.data.originalError && err.data.originalError.code === 4902))) {
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
      pay.textContent = "Pay " + session.amount_text + " " + session.token;
      if (bal < need) {
        setStatus("This wallet has " + ctx.formatUnits(bal, session.decimals) + " " + session.token +
          " on " + session.chain_name + ". You need " + session.amount_text + ".", "bad");
        pay.disabled = true;
        return;
      }
      pay.disabled = false;
      if (gas === 0n) {
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
        setStatus("Confirm the payment in your wallet…");
        txHash = await provider.request({
          method: "eth_sendTransaction",
          params: [{
            from: account,
            to: session.token_address,
            value: "0x0",
            data: transferData(session.recipient, session.amount_units),
          }],
        });
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
      const { info, provider: p } = event.detail || {};
      if (info && p && !discovered.has(info.uuid)) {
        discovered.set(info.uuid, { info, provider: p });
        renderWallets();
      }
    });
    $("pay").addEventListener("click", pay);
    window.dispatchEvent(new Event("eip6963:requestProvider"));
    setTimeout(() => {
      renderWallets();
      if (discovered.size === 0 && !window.ethereum) {
        setStatus("No wallet extension found in this browser. Install MetaMask, Rabby or " +
          "Coinbase Wallet, or use the QR code in the game.", "warn");
      }
    }, 600);
  }

  window.scfgFamilies = window.scfgFamilies || {};
  window.scfgFamilies.evm = { start };
})();
