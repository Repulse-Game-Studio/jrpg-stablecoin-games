"""Simulated JSON-RPC nodes for recording Tier 2 scenarios.

A `World` routes each request URL to the node serving it and returns the node's raw response
text (what an HTTP body would carry), or raises `TransportError` for a connection or HTTP
failure. Parsing the text is the transport's job (scenario.RecordingTransport), exactly as in
rpc.UrllibTransport, so an `invalid_json` fault reaches the core as a TransportError.

Nodes answer at the JSON-RPC level with the semantics of the unit-test fakes
(tests/unit/fakes.py FakeChain / FakeSolanaChain), plus what a real node does on top:
`eth_getLogs` filters by topics and hides blocks above `head`, a receipt of an unmined block
is null, and a Solana endpoint whose history starts after a transaction's slot does not serve
it. Every URL of a node has an `Endpoint` holding its faults (see `Endpoint`).

Only the replay-free recording side uses this module: replay.py never imports it.
"""

from __future__ import annotations

import hashlib
import json
from typing import Any

from scfg_core.coio import TransportError
from scfg_core.evm import TRANSFER_TOPIC, address_topic
from scfg_core.solana.base58 import b58encode
from scfg_core.solana.pda import TOKEN_PROGRAM_ID, associated_token_address

from . import canonical

__all__ = ["Endpoint", "EvmNode", "SolanaNode", "World", "solana_tx"]

_NOT_JSON = "<html><body><h1>502 Bad Gateway</h1></body></html>"


class _Reply(Exception):
    """A JSON-RPC error answer, raised from inside a method handler."""

    def __init__(self, code: int, message: str):
        super().__init__(message)
        self.code, self.message = code, message


class Endpoint:
    """Faults of one URL. All are off by default and persist until changed.

    - down: connection refused (TransportError, status None)
    - http_status: e.g. 429 -> TransportError("HTTP 429", status=429)
    - invalid_json: the body is not JSON (the transport raises "invalid JSON response")
    - rate_limited: JSON-RPC error -32005 "rate limit exceeded"
    - max_log_range: eth_getLogs spanning more blocks answers
      -32005 "query exceeds max block range {N}"
    - max_log_range_error: (code, message) replacing the default -32005 answer to a too-wide
      eth_getLogs (e.g. a phrase that states no limit, or one that says "archive")
    - errors[method] = (code, message): that method answers this JSON-RPC error
    - error_objects[method] = obj: that method answers {"error": obj} with obj verbatim (a
      bare string, a non-numeric code, no code: providers are inconsistent)
    - raw_bodies[method] = obj: that method answers obj as the whole body (not a dict, or a
      dict with neither result nor error, or {"result": null})
    - fail_from(method, nth, code, message): the nth call of that method to this URL (counted
      from now) and every later one answer this JSON-RPC error; `del errors_from[method]`
      clears it. For a failure in the middle of a multi-request walk.
    - first_available (Solana): this URL's getFirstAvailableBlock, overriding the node's; it
      does not serve transactions from older slots
    - tx_version_max (Solana): highest maxSupportedTransactionVersion this URL understands; a
      request above it answers -32602 (an older node)
    """

    def __init__(self, url: str):
        self.url = url
        self.down = False
        self.http_status: int | None = None
        self.invalid_json = False
        self.rate_limited = False
        self.max_log_range: int | None = None
        self.max_log_range_error: tuple[int, str] | None = None
        self.errors: dict[str, tuple[int, str]] = {}
        self.error_objects: dict[str, Any] = {}
        self.raw_bodies: dict[str, Any] = {}
        self.errors_from: dict[str, list] = {}   # method -> [calls still answered, code, msg]
        self.first_available: int | None = None
        self.tx_version_max: int | None = None

    def fail_from(self, method: str, nth: int, code: int, message: str) -> None:
        if nth < 1:
            raise ValueError("nth counts calls from 1")
        self.errors_from[method] = [nth - 1, code, message]


class _Node:
    METHODS: tuple[str, ...] = ()

    def __init__(self, urls):
        urls = list(urls)
        if not urls:
            raise ValueError("a node needs at least one URL")
        self.urls = urls
        self._endpoints = {u: Endpoint(u) for u in urls}

    def endpoint(self, url: str) -> Endpoint:
        return self._endpoints[url]

    def handle(self, url: str, payload: dict) -> str:
        ep = self._endpoints[url]
        if ep.down:
            raise TransportError("connection refused")
        if ep.http_status is not None:
            raise TransportError(f"HTTP {ep.http_status}", status=ep.http_status)
        if ep.invalid_json:
            return _NOT_JSON
        method, params = payload.get("method"), payload.get("params") or []
        body: dict[str, Any] = {"jsonrpc": "2.0", "id": payload.get("id")}
        try:
            if ep.rate_limited:
                raise _Reply(-32005, "rate limit exceeded")
            if method in ep.errors:
                raise _Reply(*ep.errors[method])
            late = ep.errors_from.get(method)
            if late is not None:
                if late[0] > 0:
                    late[0] -= 1
                else:
                    raise _Reply(late[1], late[2])
            if method in ep.raw_bodies:
                return json.dumps(canonical.to_jsonable(ep.raw_bodies[method]), sort_keys=True)
            if method in ep.error_objects:
                body["error"] = canonical.to_jsonable(ep.error_objects[method])
                return json.dumps(body, sort_keys=True)
            if method not in self.METHODS:
                raise _Reply(-32601, f"the method {method} does not exist/is not available")
            body["result"] = getattr(self, "_" + method)(ep, params)
        except _Reply as e:
            body["error"] = {"code": e.code, "message": e.message}
        # Ports parse this text with their own JSON reader (Godot: every number a float).
        return json.dumps(canonical.to_jsonable(body), sort_keys=True)


class World:
    """Routes URLs to nodes. A URL no node serves is refused like a dead host."""

    def __init__(self):
        self._routes: dict[str, _Node] = {}

    def add(self, node: _Node) -> _Node:
        for url in node.urls:
            if url in self._routes:
                raise ValueError(f"{url} is already served by another node")
        for url in node.urls:
            self._routes[url] = node
        return node

    def handle(self, url: str, payload: dict) -> str:
        node = self._routes.get(url)
        if node is None:
            raise TransportError("connection refused")
        return node.handle(url, payload)


# --- EVM ---------------------------------------------------------------------------------------


def _block_arg(value, head: int) -> int:
    return head if value in (None, "latest", "safe", "finalized") else int(value, 16)


def _topic_matches(spec, topic) -> bool:
    if spec is None:
        return True
    if topic is None:
        return False
    if isinstance(spec, list):
        return any(_topic_matches(s, topic) for s in spec)
    return spec.lower() == topic.lower()


class EvmNode(_Node):
    """One EVM chain behind one or more URLs.

    - head: current block number
    - hashes: {block: hash}; blocks not listed hash to "0x" + the number as 64 hex digits
    - logs: raw logs (blocks above head are not served yet)
    - receipts: {lowercase tx hash: eth_getTransactionReceipt result or None}

    add_transfer() appends an ERC-20 Transfer log and adds it to its transaction's receipt
    (status "0x1"), so the transfer is found by eth_getLogs and by its hash. Use reorg() rather
    than editing `hashes` so logs and receipts carry the new block hashes.
    """

    METHODS = ("eth_blockNumber", "eth_getBlockByNumber", "eth_getLogs",
               "eth_getTransactionReceipt")

    def __init__(self, urls, head: int = 1000):
        super().__init__(urls)
        self.head = head
        self.hashes: dict[int, str] = {}
        self.logs: list[dict] = []
        self.receipts: dict[str, dict | None] = {}
        self._forks = 0

    def block_hash(self, n: int) -> str:
        return self.hashes.get(n, "0x" + format(n, "064x"))

    def add_transfer(self, token: str, sender: str, recipient: str, value: int, *, block: int,
                     tx_hash: str, log_index: int = 0, address: str | None = None) -> dict:
        """A Transfer(sender, recipient, value) log emitted by `address` (default: the token
        contract; Arc USDC logs at its system emitter, with 18-decimal values)."""
        tx = tx_hash.lower()
        log = {
            "address": (address or token).lower(),
            "topics": [TRANSFER_TOPIC, address_topic(sender), address_topic(recipient)],
            "data": "0x" + format(value, "064x"),
            "transactionHash": tx,
            "logIndex": hex(log_index),
            "blockNumber": hex(block),
            "blockHash": self.block_hash(block),
        }
        self.logs.append(log)
        receipt = self.receipts.get(tx)
        if receipt is None:
            receipt = self.receipts[tx] = {
                "transactionHash": tx, "blockNumber": hex(block),
                "blockHash": self.block_hash(block), "status": "0x1", "logs": []}
        receipt["logs"].append(log)
        return log

    def mine(self, n: int = 1) -> None:
        self.head += n

    def reorg(self, from_block: int) -> None:
        """Every block from `from_block` up gets a new hash. Logs stay (their transactions are
        re-included at the same height); remove a log from `logs` to drop it."""
        self._forks += 1
        top = max([self.head] + [int(lg["blockNumber"], 16) for lg in self.logs
                                 if "blockNumber" in lg])
        for n in range(from_block, top + 1):
            digest = hashlib.sha256(f"fork {self._forks} block {n}".encode()).hexdigest()
            self.hashes[n] = "0x" + digest
        for item in self.logs + [r for r in self.receipts.values() if r]:
            if "blockNumber" in item and int(item["blockNumber"], 16) >= from_block:
                item["blockHash"] = self.block_hash(int(item["blockNumber"], 16))

    # JSON-RPC methods: _<method>(endpoint, params) -> result

    def _eth_blockNumber(self, ep, params):
        return hex(self.head)

    def _eth_getBlockByNumber(self, ep, params):
        n = _block_arg(params[0], self.head)
        if n > self.head:
            return None
        parent = self.block_hash(n - 1) if n > 0 else "0x" + "00" * 32
        return {"number": hex(n), "hash": self.block_hash(n), "parentHash": parent}

    def _eth_getLogs(self, ep, params):
        f = params[0]
        lo = _block_arg(f.get("fromBlock"), self.head)
        hi = _block_arg(f.get("toBlock"), self.head)
        if ep.max_log_range is not None and hi - lo + 1 > ep.max_log_range:
            raise _Reply(*(ep.max_log_range_error
                           or (-32005, f"query exceeds max block range {ep.max_log_range}")))
        addresses = f.get("address")
        if isinstance(addresses, str):
            addresses = [addresses]
        wanted = {a.lower() for a in addresses} if addresses else None
        topics = f.get("topics") or []
        out = []
        for lg in self.logs:
            block = int(lg["blockNumber"], 16)
            if not lo <= block <= min(hi, self.head):
                continue
            if wanted is not None and lg["address"].lower() not in wanted:
                continue
            have = lg.get("topics") or []
            if all(_topic_matches(spec, have[i] if i < len(have) else None)
                   for i, spec in enumerate(topics)):
                out.append(lg)
        return out

    def _eth_getTransactionReceipt(self, ep, params):
        receipt = self.receipts.get(params[0].lower())
        if receipt is None or int(receipt["blockNumber"], 16) > self.head:
            return None
        return receipt


# --- Solana ------------------------------------------------------------------------------------


def solana_tx(sig: str, *, mint: str, payer: str, owner: str, amount: int, slot: int,
              err=None, payer_start: int = 10**9, version=0) -> dict:
    """A getTransaction(json) result where `payer` sends `amount` of `mint` to `owner`
    (tests/unit/fakes.solana_tx with the real token accounts). accountIndex 1 is the payer's
    token account, 2 the owner's."""
    def row(index, who, value):
        return {"accountIndex": index, "mint": mint, "owner": who,
                "uiTokenAmount": {"amount": str(value), "decimals": 6}}
    keys = [payer, associated_token_address(payer, mint), associated_token_address(owner, mint),
            mint]
    tx = {
        "slot": slot,
        "transaction": {"signatures": [sig], "message": {"accountKeys": keys}},
        "meta": {"err": err,
                 "preTokenBalances": [row(1, payer, payer_start), row(2, owner, 0)],
                 "postTokenBalances": [row(1, payer, payer_start - amount),
                                       row(2, owner, amount)]},
    }
    if version is not None:
        tx["version"] = version
    return tx


class SolanaNode(_Node):
    """One Solana cluster behind one or more URLs (FakeSolanaChain's fields, over JSON-RPC).

    - slot: current slot (getSlot, any commitment)
    - txs: {signature: getTransaction result}; a tx's "version" (int) above the request's
      maxSupportedTransactionVersion answers -32015 naming it
    - addresses: {address: [signature infos, newest first]} (getSignaturesForAddress)
    - statuses: {signature: getSignatureStatuses entry or None}
    - accounts: existing accounts (getAccountInfo base64); token_balances' keys exist too
    - first_available: getFirstAvailableBlock (per URL: Endpoint.first_available)
    - blockhash: getLatestBlockhash
    - balances: {address: lamports} (getBalance); token_balances: {token account: units}
    - priority_fees: getRecentPrioritizationFees values, one per recent slot
    """

    METHODS = ("getSlot", "getSignaturesForAddress", "getTransaction", "getFirstAvailableBlock",
               "getSignatureStatuses", "getAccountInfo", "getLatestBlockhash", "getBalance",
               "getRecentPrioritizationFees")

    def __init__(self, urls, slot: int = 1000):
        super().__init__(urls)
        self.slot = slot
        self.txs: dict[str, dict] = {}
        self.addresses: dict[str, list[dict]] = {}
        self.statuses: dict[str, dict | None] = {}
        self.accounts: set[str] = set()
        self.first_available = 0
        self.blockhash = b58encode(hashlib.sha256(b"simnode blockhash").digest())
        self.balances: dict[str, int] = {}
        self.token_balances: dict[str, int] = {}
        self.priority_fees: list[int] = []

    def pay(self, sig: str, *, mint: str, payer: str, owner: str, amount: int, slot=None,
            err=None, addresses=(), status: str = "confirmed", version=0) -> dict:
        """FakeSolanaChain.pay: store the transaction, list it on `addresses` (pass the
        session's reference and dest to be found by both) and give it a status."""
        tx = solana_tx(sig, mint=mint, payer=payer, owner=owner, amount=amount,
                       slot=self.slot if slot is None else slot, err=err, version=version)
        self.txs[sig] = tx
        for a in addresses:
            self.addresses.setdefault(a, []).insert(
                0, {"signature": sig, "slot": tx["slot"], "err": err})
        self.statuses[sig] = {"slot": tx["slot"], "err": err, "confirmationStatus": status,
                              "confirmations": None if status == "finalized" else 1}
        return tx

    def finalize(self, sig: str) -> None:
        self.statuses[sig] = dict(self.statuses[sig], confirmationStatus="finalized",
                                  confirmations=None)

    def _first(self, ep) -> int:
        return self.first_available if ep.first_available is None else ep.first_available

    def _context(self, value):
        return {"context": {"slot": self.slot}, "value": value}

    # JSON-RPC methods: _<method>(endpoint, params) -> result

    def _getSlot(self, ep, params):
        return self.slot

    def _getSignaturesForAddress(self, ep, params):
        opts = params[1] if len(params) > 1 else {}
        sigs = list(self.addresses.get(params[0], []))
        names = [s["signature"] for s in sigs]
        if opts.get("until") in names:
            sigs = sigs[:names.index(opts["until"])]
        names = [s["signature"] for s in sigs]
        if opts.get("before") in names:
            sigs = sigs[names.index(opts["before"]) + 1:]
        return [dict(s) for s in sigs[:opts.get("limit", 1000)]]

    def _getTransaction(self, ep, params):
        opts = params[1] if len(params) > 1 else {}
        asked = opts.get("maxSupportedTransactionVersion")
        if ep.tx_version_max is not None and asked is not None and asked > ep.tx_version_max:
            raise _Reply(-32602, "Invalid params: unknown maxSupportedTransactionVersion "
                                 f"{asked}")
        tx = self.txs.get(params[0])
        if tx is None or tx["slot"] < self._first(ep):
            return None
        version = tx.get("version")
        if isinstance(version, int) and (asked is None or version > asked):
            raise _Reply(-32015, f"Transaction version ({version}) is not supported by the "
                                 "requesting client. Please try the request again with the "
                                 "following configuration parameter: "
                                 f"\"maxSupportedTransactionVersion\": {version}")
        return tx

    def _getFirstAvailableBlock(self, ep, params):
        return self._first(ep)

    def _getSignatureStatuses(self, ep, params):
        return self._context([self.statuses.get(s) for s in params[0]])

    def _getAccountInfo(self, ep, params):
        address = params[0]
        opts = params[1] if len(params) > 1 else {}
        if address not in self.accounts and address not in self.token_balances:
            return self._context(None)
        common = {"executable": False, "lamports": 2_039_280, "owner": TOKEN_PROGRAM_ID,
                  "rentEpoch": 0, "space": 165}
        if opts.get("encoding") == "jsonParsed":
            amount = self.token_balances.get(address, 0)
            data = {"program": "spl-token", "space": 165, "parsed": {
                "type": "account",
                "info": {"tokenAmount": {"amount": str(amount), "decimals": 6}}}}
        else:
            data = ["", "base64"]
        return self._context(dict(common, data=data))

    def _getLatestBlockhash(self, ep, params):
        return self._context({"blockhash": self.blockhash,
                              "lastValidBlockHeight": self.slot + 150})

    def _getBalance(self, ep, params):
        return self._context(self.balances.get(params[0], 0))

    def _getRecentPrioritizationFees(self, ep, params):
        return [{"slot": self.slot - i, "prioritizationFee": fee}
                for i, fee in enumerate(self.priority_fees)]
