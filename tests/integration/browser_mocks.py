"""Mock browser wallets for the integration tests and the web e2e runner: an EIP-6963
wallet backed by an RPC client (Anvil), and a Wallet Standard Solana wallet that signs
with a local keypair and sends through the local cluster."""
from scfg_core.rpc import RpcError
from scfg_core.solana.base58 import b58decode

MOCK_WALLET = """
(() => {
  const account = "%(account)s";
  const reject = %(reject)s;
  const oddHash = %(odd_hash)s;
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
      // odd_hash: the transaction is sent, but its hash comes back as a BigInt (no JSON for it)
      if (method === "eth_sendTransaction" && oddHash) return BigInt(res.result);
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


def install_mock_wallet(page, rpc, account, reject=False, odd_hash=False):
    """Before page.goto(): an EIP-6963 wallet for `account` whose RPC goes to `rpc`.
    odd_hash: it sends the transaction but answers its hash as a BigInt."""
    def wallet_rpc(method, params):
        try:
            return {"result": rpc.call(method, params)}
        except RpcError as e:
            return {"error": {"code": e.code, "message": e.message}}

    page.expose_function("__walletRpc", wallet_rpc)
    page.add_init_script(MOCK_WALLET % {"account": account,
                                        "reject": "true" if reject else "false",
                                        "odd_hash": "true" if odd_hash else "false"})


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


# What a broken or hostile extension can hand the page: EIP-6963 announcements without a detail,
# with a null provider, with getters and proxies that throw; Wallet Standard registrations of
# wallets whose fields throw, callbacks that throw or reject; and two wallets that look fine but
# whose every request rejects with an odd value (window.__rejectWith, one of REJECTIONS): "Bad
# Wallet" (EVM) and "Bad Solana Wallet". "Trap Wallet" (EVM) is a valid announcement whose
# provider throws on any use. With ESCAPE_RECORDER first (see install_malformed_extensions).
MALFORMED_EXTENSIONS = """
(() => {
  const boom = () => { throw new Error("malformed extension"); };
  const trap = () => new Proxy({}, { get: boom, has: boom, ownKeys: boom, getPrototypeOf: boom,
                                     getOwnPropertyDescriptor: boom, apply: boom });
  const icon = "data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg'/%3E";
  const rejections = {
    null: () => null,
    undefined: () => undefined,
    nullproto: () => Object.create(null),
    getters: () => ({ get code() { return boom(); }, get message() { return boom(); }, toString: boom }),
    proxy: trap,
    symbol: () => Symbol("odd"),
  };
  const rejection = () => (rejections[window.__rejectWith] || rejections.null)();

  // ---- EIP-6963 (evm.js, bridge.js)
  const badProvider = { request: async () => { throw rejection(); } };
  const announce = (detail) => window.dispatchEvent(
    new CustomEvent("eip6963:announceProvider", { detail }));
  window.addEventListener("eip6963:requestProvider", () => {
    window.dispatchEvent(new Event("eip6963:announceProvider"));      // detail undefined
    announce(null);
    announce(42);
    announce({ info: { uuid: "bad-0", name: "No Provider", icon }, provider: null });
    announce({ get info() { return boom(); }, get provider() { return boom(); } });
    announce({ info: { get uuid() { return boom(); }, get name() { return boom(); },
                       get icon() { return boom(); } }, provider: badProvider });
    announce(trap());
    announce({ info: trap(), provider: trap() });
    announce({ info: { uuid: "bad-1", name: "Trap Wallet", icon }, provider: trap() });
    announce({ info: { uuid: "bad-2", name: "Bad Wallet", icon }, provider: badProvider });
  });

  // ---- Wallet Standard (solana.js, bridge.js)
  const chain = "solana:devnet";
  const fails = async () => { throw rejection(); };
  const features = () => ({
    "standard:connect": { version: "1.0.0", connect: fails },
    "solana:signAndSendTransaction": { version: "1.0.0", signAndSendTransaction: fails },
  });
  const wallets = [
    null, 42, trap(),
    { version: "1.0.0", name: "Getters", get chains() { return boom(); }, get features() { return boom(); } },
    { version: "1.0.0", get name() { return boom(); }, get icon() { return boom(); },
      chains: [chain], accounts: [], features: features() },
    { version: "1.0.0", name: "Trap Features", icon, chains: [chain], accounts: [], features: trap() },
    { version: "1.0.0", name: "Bad Solana Wallet", icon, chains: [chain], accounts: [], features: features() },
  ];
  // A careless wallet: nothing it does is wrapped in a try.
  const register = (api) => api.register(...wallets);
  window.addEventListener("wallet-standard:app-ready", (e) => {
    register(e.detail);
    // and, as wallets that load after the page, through register-wallet
    const registerWallet = (detail) => window.dispatchEvent(
      new CustomEvent("wallet-standard:register-wallet", { detail }));
    window.dispatchEvent(new Event("wallet-standard:register-wallet"));   // no callback
    registerWallet(null);
    registerWallet(boom);
    registerWallet(async () => boom());                                   // a callback that rejects
    registerWallet(register);
  });
  window.dispatchEvent(new CustomEvent("wallet-standard:register-wallet", { detail: register }));
})();
"""

# Each kind of rejection and the status the page shows for it (walletError).
# The page's own record of what escaped it (uncaught errors and unhandled rejections):
# window.__escaped.
ESCAPE_RECORDER = """
(() => {
  window.__escaped = [];
  const note = (kind, value) => {
    let text;
    try { text = String((value && value.stack) || value); } catch (e) { text = "(unprintable)"; }
    window.__escaped.push(kind + ": " + text);
  };
  window.addEventListener("error", (e) => note("error", e.error || e.message));
  window.addEventListener("unhandledrejection", (e) => note("unhandledrejection", e.reason));
})();
"""

REJECTIONS = {
    "null": "Wallet error: null",
    "undefined": "Wallet error: undefined",
    "nullproto": "Wallet error: unknown error",
    "getters": "Wallet error: unknown error",
    "proxy": "Wallet error: unknown error",
    "symbol": "Wallet error: Symbol(odd)",
}

# Extensions that look like wallets but aren't usable ones: an announcement whose name is not a
# string, a Wallet Standard wallet whose name is not a string. Neither is listed by the page,
# so the game must not offer "Browser wallet" for them either.
NAMELESS_WALLETS = """
(() => {
  const provider = { request: async () => { throw new Error("nameless"); } };
  window.addEventListener("eip6963:requestProvider", () => {
    window.dispatchEvent(new CustomEvent("eip6963:announceProvider",
      { detail: { info: { uuid: "nameless-1", name: 42 }, provider } }));
  });
  const fails = async () => { throw new Error("nameless"); };
  const wallet = { version: "1.0.0", name: 42, chains: ["solana:devnet"], accounts: [], features: {
    "standard:connect": { version: "1.0.0", connect: fails },
    "solana:signAndSendTransaction": { version: "1.0.0", signAndSendTransaction: fails } } };
  window.addEventListener("wallet-standard:app-ready", (e) => e.detail.register(wallet));
})();
"""

# window.ethereum whose getter throws (two extensions fighting over it), and no EIP-6963 wallet.
THROWING_ETHEREUM = """
Object.defineProperty(window, "ethereum", { configurable: true,
  get() { throw new Error("window.ethereum getter"); } });
"""


def record_escapes(page):
    """Before page.goto(): record what escapes the page. Returns the list Playwright's page
    errors and console errors are appended to; see escaped()."""
    errors = []

    def console(m):
        # the browser's own request for /favicon.ico (404 on the localhost page) is no error
        if m.type == "error" and not (m.location or {}).get("url", "").endswith("/favicon.ico"):
            errors.append(f"console.error: {m.text}")

    page.on("pageerror", lambda e: errors.append(f"pageerror: {e}"))
    page.on("console", console)
    page.add_init_script(ESCAPE_RECORDER)
    return errors


def install_malformed_extensions(page):
    """Before page.goto() (and before or after the good mock wallets): the broken extensions
    of MALFORMED_EXTENSIONS. Returns record_escapes()'s list."""
    errors = record_escapes(page)
    page.add_init_script(MALFORMED_EXTENSIONS)
    return errors


def escaped(page, errors):
    """Everything that escaped the page so far: Playwright's page and console errors and the
    page's own record of uncaught errors and unhandled rejections."""
    return list(errors) + page.evaluate("window.__escaped || []")


def assert_nothing_escaped(page, errors):
    out = escaped(page, errors)
    assert out == [], "the page let errors escape:\n" + "\n".join(out)


def wait_for_wallet_error(page, errors, timeout=10):
    """Wait until the page's status is a "bad" one (the wallet error state), failing at once
    on anything that escaped the page. Returns the status text."""
    import time

    status = page.locator("#status")
    deadline = time.time() + timeout
    while True:
        assert_nothing_escaped(page, errors)
        if status.get_attribute("class") == "bad":
            return status.inner_text()
        assert time.time() < deadline, f"no wallet error shown; status is {status.inner_text()!r}"
        page.wait_for_timeout(50)


def try_bad_wallets(page, errors, family="evm"):
    """Click the bad wallets once per kind of rejection: each ends as the page's wallet error
    state, and nothing escapes. EVM: also Trap Wallet, whose provider throws on any use."""
    names = ["Bad Wallet", "Trap Wallet"] if family == "evm" else ["Bad Solana Wallet"]
    for kind, want in REJECTIONS.items():
        page.evaluate("(k) => { window.__rejectWith = k; }", kind)
        for name in names:
            page.locator("#status").evaluate("(el) => { el.className = ''; }")
            page.get_by_role("button", name="Pay with " + name, exact=True).click()
            text = wait_for_wallet_error(page, errors)
            if name == "Trap Wallet":
                want = "Wallet error: malformed extension"
            assert text == want, (kind, name, text)
