"""Mock browser wallets for the integration tests and the web e2e runner: an EIP-6963
wallet backed by an RPC client (Anvil), and a Wallet Standard Solana wallet that signs
with a local keypair and sends through the local cluster."""
from scfg_core.rpc import RpcError
from scfg_core.solana.base58 import b58decode

MOCK_WALLET = """
(() => {
  const account = "%(account)s";
  const reject = %(reject)s;
  let chainId = "0x1";
  const provider = {
    async request({ method, params }) {
      params = params || [];
      if (method === "eth_requestAccounts" || method === "eth_accounts") return [account];
      if (method === "eth_chainId") return chainId;
      if (method === "wallet_switchEthereumChain") { chainId = params[0].chainId; return null; }
      if (method === "eth_sendTransaction" && reject) {
        const e = new Error("User rejected the request."); e.code = 4001; throw e;
      }
      const res = await window.__walletRpc(method, params);
      if (res.error) { const e = new Error(res.error.message); e.code = res.error.code; throw e; }
      return res.result;
    },
  };
  const info = { uuid: "mock-1", name: "Mock Wallet", rdns: "test.mock",
                 icon: "data:image/svg+xml,%%3Csvg xmlns='http://www.w3.org/2000/svg'/%%3E" };
  window.addEventListener("eip6963:requestProvider", () => {
    window.dispatchEvent(new CustomEvent("eip6963:announceProvider",
      { detail: Object.freeze({ info, provider }) }));
  });
})();
"""

MOCK_SOLANA_WALLET = """
(() => {
  const address = "%(address)s";
  const account = { address, publicKey: new Uint8Array(%(pubkey)s),
                    chains: ["solana:devnet"], features: ["solana:signAndSendTransaction"] };
  const reject = %(reject)s;
  const wallet = {
    version: "1.0.0", name: "Mock Solana Wallet",
    icon: "data:image/svg+xml,%%3Csvg xmlns='http://www.w3.org/2000/svg'/%%3E",
    chains: ["solana:devnet"], accounts: [account],
    features: {
      "standard:connect": { version: "1.0.0", connect: async () => ({ accounts: [account] }) },
      "solana:signAndSendTransaction": {
        version: "1.0.0", supportedTransactionVersions: ["legacy", 0],
        signAndSendTransaction: async (...inputs) => {
          if (reject) { const e = new Error("User rejected the request."); e.code = 4001; throw e; }
          const out = [];
          for (const input of inputs) {
            const sig = await window.__solanaSignAndSend(Array.from(input.transaction));
            out.push({ signature: new Uint8Array(sig) });
          }
          return out;
        },
      },
    },
  };
  const callback = ({ register }) => register(wallet);
  window.addEventListener("wallet-standard:app-ready", (e) => callback(e.detail));
  window.dispatchEvent(new CustomEvent("wallet-standard:register-wallet", { detail: callback }));
})();
"""


def install_mock_wallet(page, rpc, account, reject=False):
    """Before page.goto(): an EIP-6963 wallet for `account` whose RPC goes to `rpc`."""
    def wallet_rpc(method, params):
        try:
            return {"result": rpc.call(method, params)}
        except RpcError as e:
            return {"error": {"code": e.code, "message": e.message}}

    page.expose_function("__walletRpc", wallet_rpc)
    page.add_init_script(MOCK_WALLET % {"account": account,
                                        "reject": "true" if reject else "false"})


def install_mock_solana_wallet(page, cluster, player, reject=False, drop_first=False):
    """Before page.goto(): a Wallet Standard wallet for `player`. Returns the list the
    signatures it really sent are appended to. drop_first: the first transaction is signed
    and its signature returned, but it is never sent (as when it's dropped)."""
    sent, dropped = [], []

    def sign_and_send(tx):
        if drop_first and not dropped:
            from solders.transaction import Transaction

            signed = Transaction.from_bytes(bytes(tx))
            signed.sign([player], signed.message.recent_blockhash)
            dropped.append(str(signed.signatures[0]))
            return list(bytes(signed.signatures[0]))
        signature = cluster.sign_and_send(bytes(tx), player)
        sent.append(signature)
        return list(b58decode(signature))

    page.expose_function("__solanaSignAndSend", sign_and_send)
    page.add_init_script(MOCK_SOLANA_WALLET % {
        "address": str(player.pubkey()), "pubkey": list(bytes(player.pubkey())),
        "reject": "true" if reject else "false"})
    return sent
