"""Solana JSON-RPC on top of the shared failover client (rpc.RpcClient).

History-sensitive lookups (restore, launch re-check) ask endpoints one by one, because
public endpoints keep very different amounts of history: publicnode about two days,
api.mainnet-beta everything. A missing transaction only counts as "doesn't exist" when
an endpoint whose history reaches back to the slot in question says so.
"""

from __future__ import annotations

import re

from ..rpc import RpcError, RpcUnavailable

TX_VERSION = 1  # highest transaction version this build can read (only meta is used)
_UNKNOWN_HISTORY = 1 << 62
_UNSUPPORTED_VERSION = -32015
_NAMED_VERSION = re.compile(r"version \((\d+)\)", re.IGNORECASE)


class SolanaRpc:
    def __init__(self, client):
        self.client = client
        self.tx_version = TX_VERSION
        self._first_slot: dict[str, int] = {}

    def call(self, method: str, params: list):
        return (yield from self.client.call(method, params))

    def get_slot(self, commitment: str = "confirmed"):
        return int((yield from self.call("getSlot", [{"commitment": commitment}])))

    def get_signatures_for_address(self, address: str, *, until=None, before=None,
                                   limit: int = 100, commitment: str = "confirmed"):
        opts = {"limit": limit, "commitment": commitment}
        if until:
            opts["until"] = until
        if before:
            opts["before"] = before
        return (yield from self.call("getSignaturesForAddress", [address, opts])) or []

    def _tx_params(self, sig: str, commitment: str, version: int | None = None) -> list:
        return [sig, {"encoding": "json", "commitment": commitment,
                      "maxSupportedTransactionVersion":
                          self.tx_version if version is None else version}]

    def _named_version(self, error: RpcError) -> int | None:
        """The newer version a -32015 "Transaction version (N) is not supported" names.
        Only meta, slot and accountKeys are read, so any version can be read."""
        if error.code != _UNSUPPORTED_VERSION:
            return None
        found = _NAMED_VERSION.search(error.message)
        if found is None or int(found.group(1)) <= self.tx_version:
            return None
        return int(found.group(1))

    def _lower_version(self, error: RpcError) -> bool:
        """Older nodes reject a maxSupportedTransactionVersion they don't know (-32602)."""
        if self.tx_version > 0 and error.code == -32602 and "version" in error.message.lower():
            self.tx_version -= 1
            return True
        return False

    def get_transaction(self, sig: str, commitment: str = "confirmed"):
        try:
            return (yield from self.call("getTransaction", self._tx_params(sig, commitment)))
        except RpcError as e:
            if self._lower_version(e):
                return (yield from self.get_transaction(sig, commitment))
            version = self._named_version(e)
            if version is None:
                raise
            return (yield from self.call("getTransaction",
                                         self._tx_params(sig, commitment, version)))

    def _first_available(self, url: str):
        if url not in self._first_slot:
            try:
                self._first_slot[url] = int((yield from self.client.call_at(
                    url, "getFirstAvailableBlock", [])))
            except (RpcUnavailable, RpcError, TypeError, ValueError):
                return _UNKNOWN_HISTORY  # unknown: treat as pruned, ask again next time
        return self._first_slot[url]

    def get_transaction_any(self, sig: str, *, min_slot=None, commitment: str = "finalized"):
        answered = authoritative = False
        for url in self.client.live_urls():
            attempt = yield from self.client.try_at(url, "getTransaction",
                                                    self._tx_params(sig, commitment))
            if attempt is None:
                continue
            res = attempt[1]
            if isinstance(res, RpcError):
                if self._lower_version(res):
                    return (yield from self.get_transaction_any(sig, min_slot=min_slot,
                                                                commitment=commitment))
                version = self._named_version(res)
                if version is None:
                    continue
                try:
                    res = yield from self.client.call_at(
                        url, "getTransaction", self._tx_params(sig, commitment, version))
                except (RpcUnavailable, RpcError):
                    continue
            answered = True
            if res is not None:
                return res, False
            if min_slot is not None and (yield from self._first_available(url)) <= min_slot:
                authoritative = True
        if not answered:
            raise RpcUnavailable(f"no endpoint answered getTransaction for {sig}")
        return None, authoritative

    def get_signature_statuses(self, sigs):
        res = yield from self.call("getSignatureStatuses",
                                   [list(sigs), {"searchTransactionHistory": True}])
        return res["value"]

    def account_exists(self, address: str):
        res = yield from self.call("getAccountInfo", [address, {"encoding": "base64",
                                                                "commitment": "confirmed"}])
        return res["value"] is not None

    def get_latest_blockhash(self):
        res = yield from self.call("getLatestBlockhash", [{"commitment": "confirmed"}])
        return res["value"]["blockhash"]

    def get_token_balance(self, token_account: str):
        res = yield from self.call("getAccountInfo", [token_account, {
            "encoding": "jsonParsed", "commitment": "confirmed"}])
        value = res["value"]
        if value is None:
            return 0
        return int(value["data"]["parsed"]["info"]["tokenAmount"]["amount"])

    def get_balance(self, address: str):
        res = yield from self.call("getBalance", [address, {"commitment": "confirmed"}])
        return int(res["value"])

    def get_priority_fee(self, accounts):
        rows = yield from self.call("getRecentPrioritizationFees", [list(accounts)])
        fees = sorted(int(f["prioritizationFee"]) for f in rows or [])
        return fees[len(fees) // 2] if fees else 0
