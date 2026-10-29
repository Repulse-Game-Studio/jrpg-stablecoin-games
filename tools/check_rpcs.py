"""Health check for the built-in public RPC endpoints.

For every endpoint in the registry it checks that:
  * eth_chainId matches the chain,
  * eth_getLogs accepts a range of `max_log_range` blocks, using the same query shape
    the plugin uses (USDC transfers *to one wallet*, so responses stay small).
For Solana endpoints it checks getSlot, getSignaturesForAddress (on the first token's mint)
and getFirstAvailableBlock, and reports how much history the endpoint keeps.

DEGRADED endpoints work but accept smaller eth_getLogs ranges than the
registry's max_log_range (the client adapts automatically, just with more calls).
Exits non-zero if any chain is left with no working endpoint. Intended to run
weekly in CI so dead endpoints are replaced in the next plugin release.
It also checks that each endpoint answers browsers (CORS), which Ren'Py web builds need:
endpoints marked "web: BLOCKED" are skipped by web players.

Usage: uv run python tools/check_rpcs.py [--testnet]
"""

from __future__ import annotations

import argparse
import http.client
import json
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "plugin" / "game" / "python-packages"))

from cryptopay_core.chains import MAINNET, TESTNET  # noqa: E402
from cryptopay_core.coio import Sync  # noqa: E402
from cryptopay_core.evm import TRANSFER_TOPIC, address_topic  # noqa: E402
from cryptopay_core.rpc import (  # noqa: E402
    RpcClient,
    RpcError,
    RpcUnavailable,
    UrllibTransport,
    _ssl_context,
)


def check(url: str, chain) -> tuple[str, str]:
    """Returns (status, detail); status is OK, DEGRADED (works, smaller log range) or FAIL."""
    rpc = Sync(RpcClient([url], log_range=chain.max_log_range, timeout=15), UrllibTransport())
    started = time.time()
    try:
        chain_id = int(rpc.call("eth_chainId", []), 16)
        if chain_id != chain.chain_id:
            return "FAIL", f"wrong chain id {chain_id}"
        head = rpc.block_number()
        token = next(iter(chain.tokens.values())).address
        wallet = "0x52908400098527886E0F7030069857D2E4169EE7"  # any address works for the probe
        rpc.get_logs(token, [TRANSFER_TOPIC, None, address_topic(wallet)],
                     head - chain.max_log_range + 1, head)
        elapsed = f"{(time.time() - started) * 1000:.0f} ms"
        if rpc.log_range < chain.max_log_range:
            return "DEGRADED", f"getLogs limited to {rpc.log_range} blocks, {elapsed}"
    except (RpcError, RpcUnavailable, ValueError) as e:
        return "FAIL", str(e)[:120]
    return "OK", elapsed


def history_depth(first_block: int, slot: int) -> str:
    """'full' when the endpoint keeps everything, else the approximate age of its oldest block."""
    if first_block == 0:
        return "full"
    days = (slot - first_block) * 0.4 / 86400  # about 0.4 s per slot
    return f"~{days:.0f} days" if days >= 1 else f"~{days * 24:.0f} hours"


def check_solana(url: str, chain) -> tuple[str, str]:
    """Returns (status, detail) for one Solana endpoint; status is OK or FAIL."""
    rpc = Sync(RpcClient([url], log_range=1, timeout=15), UrllibTransport())
    started = time.time()
    try:
        slot = int(rpc.call("getSlot", []))
        mint = next(iter(chain.tokens.values())).address
        rpc.call("getSignaturesForAddress", [mint, {"limit": 1}])
        first_block = int(rpc.call("getFirstAvailableBlock", []))
    except (RpcError, RpcUnavailable, ValueError, TypeError) as e:
        return "FAIL", str(e)[:120]
    elapsed = f"{(time.time() - started) * 1000:.0f} ms"
    return "OK", f"slot {slot}, history {history_depth(first_block, slot)}, {elapsed}"


WEB_ORIGIN = "https://html-classic.itch.zone"  # where itch.io serves HTML5 games


def check_cors(url: str, origin: str = WEB_ORIGIN, timeout: float = 15) -> bool:
    """Would a web build served from `origin` be allowed to call this endpoint? The preflight
    must answer 2xx, allow the origin and permit the JSON content type; the POST itself must
    allow the origin. A malformed or dropped response counts as blocked."""
    def allowed(headers) -> bool:
        return headers.get("Access-Control-Allow-Origin", "") in ("*", origin)

    def preflight_ok(status: int, headers) -> bool:
        permitted = [h.strip().lower()
                     for h in headers.get("Access-Control-Allow-Headers", "").split(",")]
        return (200 <= status < 300 and allowed(headers)
                and ("*" in permitted or "content-type" in permitted))

    def post_ok(_status: int, headers) -> bool:
        return allowed(headers)

    ctx = _ssl_context() if url.startswith("https:") else None
    preflight = urllib.request.Request(url, method="OPTIONS", headers={
        "Origin": origin, "Access-Control-Request-Method": "POST",
        "Access-Control-Request-Headers": "content-type", "User-Agent": "cryptopay-check"})
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "eth_chainId", "params": []})
    post = urllib.request.Request(url, data=body.encode(), method="POST", headers={
        "Origin": origin, "Content-Type": "application/json", "User-Agent": "cryptopay-check"})
    def ask(req, judge) -> bool:
        try:
            with urllib.request.urlopen(req, timeout=timeout, context=ctx) as resp:
                return judge(resp.status, resp.headers)
        except urllib.error.HTTPError as e:   # an error status still carries the CORS headers
            return judge(e.code, e.headers)

    try:
        return ask(preflight, preflight_ok) and ask(post, post_ok)
    except (urllib.error.URLError, OSError, http.client.HTTPException):
        return False


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--testnet", action="store_true")
    args = ap.parse_args()
    registry = TESTNET if args.testnet else MAINNET
    dead_chains = []
    web_results = []
    for key, chain in registry.items():
        working = 0
        for url in chain.rpc:
            status, detail = (check_solana if chain.family == "solana" else check)(url, chain)
            working += status != "FAIL"
            web_ok = check_cors(url)
            web_results.append((url, web_ok))
            web = "web: ok" if web_ok else "web: BLOCKED"
            print(f"{status:<8} {chain.name:<18} {url:<48} {detail}, {web}")
        if working == 0:
            dead_chains.append(key)
    blocked = [u for u, web_ok in web_results if not web_ok]
    if blocked:
        print(f"\n{len(blocked)} endpoint(s) refuse browsers; web builds skip them:")
        for u in blocked:
            print("  " + u)
    if dead_chains:
        print(f"\nNo working endpoint for: {', '.join(dead_chains)}")
        return 1
    print("\nEvery chain has at least one working endpoint.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
