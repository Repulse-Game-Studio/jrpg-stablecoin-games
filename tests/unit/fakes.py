"""Shared test doubles. Not collected as tests (no test_ prefix)."""
import functools
import inspect

from scfg_core.coio import TransportError, run_sync


def coop(*names):
    """Class decorator: the named methods become generator functions, like the real
    cooperative RPC objects (they make no request, so run_sync needs no transport)."""
    def wrap(cls):
        for name in names:
            fn = getattr(cls, name)

            def make(fn):
                @functools.wraps(fn)
                def method(*args, **kwargs):
                    return fn(*args, **kwargs)
                    yield  # never reached: makes this a generator function
                return method
            setattr(cls, name, make(fn))
        return cls
    return wrap


def drive(gen, transport=None):
    """Run a cooperative call to completion (fakes need no transport)."""
    return run_sync(gen, transport)


class FakeClock:
    def __init__(self, now=1000.0):
        self.now = now

    def __call__(self):
        return self.now

    def advance(self, seconds):
        self.now += seconds


class FakeTransport:
    """Routes each request to a per-URL handler: handler(method, params) -> response dict.

    A handler may raise TransportError to simulate network/HTTP failures.
    Every request is recorded in .calls as (url, method, params).
    """

    def __init__(self, handlers):
        self.handlers = handlers
        self.calls = []

    def post_json(self, url, payload, timeout):
        self.calls.append((url, payload["method"], payload["params"]))
        return self.handlers[url](payload["method"], payload["params"])


def ok(result):
    return {"jsonrpc": "2.0", "id": 1, "result": result}


def err(code, message):
    return {"jsonrpc": "2.0", "id": 1, "error": {"code": code, "message": message}}


def down(method, params):
    raise TransportError("connection refused")


@coop("block_number", "get_block_hash", "get_transaction_receipt", "get_logs")
class FakeChain:
    """In-memory stand-in for RpcClient (same method names), driven by tests.

    - head: current block number
    - hashes: {block_number: block_hash}; blocks not listed get a default hash
    - logs: list of raw log dicts returned by get_logs (filtered by block range and address)
    - receipts: {tx_hash: receipt dict or None}
    - down: when True every method raises RpcUnavailable
    """

    def __init__(self, head=1000):
        self.head = head
        self.hashes = {}
        self.logs = []
        self.receipts = {}
        self.down = False
        self.calls = []

    def _check(self, name):
        self.calls.append(name)
        if self.down:
            from scfg_core.rpc import RpcUnavailable
            raise RpcUnavailable("fake outage")

    def block_hash(self, n):
        return self.hashes.get(n, "0x" + format(n, "064x"))

    def block_number(self):
        self._check("block_number")
        return self.head

    def get_block_hash(self, n):
        self._check("get_block_hash")
        return self.block_hash(n) if n <= self.head else None

    def get_transaction_receipt(self, tx_hash):
        self._check("get_transaction_receipt")
        return self.receipts.get(tx_hash)

    def get_logs(self, address, topics, from_block, to_block):
        self._check("get_logs")
        self.last_get_logs = (address, topics, from_block, to_block)
        return [lg for lg in self.logs
                if from_block <= int(lg["blockNumber"], 16) <= to_block
                and lg["address"].lower() == address.lower()]


def transfer_log(token, sender, recipient, value, *, block, tx_hash, log_index=0, block_hash=None):
    from scfg_core.evm import TRANSFER_TOPIC, address_topic
    return {
        "address": token.lower(),
        "topics": [TRANSFER_TOPIC, address_topic(sender), address_topic(recipient)],
        "data": "0x" + format(value, "064x"),
        "transactionHash": tx_hash,
        "logIndex": hex(log_index),
        "blockNumber": hex(block),
        "blockHash": block_hash or "0x" + format(block, "064x"),
    }


def immediate_submitter(engine):
    """A Worker.submit stand-in that runs the command at once and returns a done Future."""
    from concurrent.futures import Future

    def submit(fn):
        fut = Future()
        try:
            result = fn(engine)
            if inspect.isgenerator(result):
                result = engine.run(result)
            fut.set_result(result)
        except Exception as e:
            fut.set_exception(e)
        return fut
    return submit


def solana_tx(sig, *, mint, payer, owner, amount, slot, err=None, payer_start=10**9):
    """A getTransaction(json) result where `payer` sends `amount` of `mint` to `owner`.
    accountIndex 1 = payer's token account, 2 = owner's token account."""
    def row(index, who, value):
        return {"accountIndex": index, "mint": mint, "owner": who,
                "uiTokenAmount": {"amount": str(value), "decimals": 6}}
    return {
        "slot": slot,
        "transaction": {"signatures": [sig],
                        "message": {"accountKeys": [payer, "PAYERATA", "OWNERATA", mint]}},
        "meta": {"err": err,
                 "preTokenBalances": [row(1, payer, payer_start), row(2, owner, 0)],
                 "postTokenBalances": [row(1, payer, payer_start - amount), row(2, owner, amount)]},
    }


@coop("get_slot", "get_signatures_for_address", "get_transaction", "get_transaction_any",
      "get_signature_statuses", "account_exists")
class FakeSolanaChain:
    """In-memory stand-in for SolanaRpc (same method names), driven by tests.

    - txs: {signature: getTransaction result}
    - addresses: {address: [signature info dicts, newest first]}
    - statuses: {signature: getSignatureStatuses entry or None}
    - accounts: set of existing account addresses (for account_exists)
    - first_available: slot from which get_transaction_any's null is authoritative
    """

    def __init__(self, slot=1000):
        self.slot = slot
        self.txs = {}
        self.addresses = {}
        self.statuses = {}
        self.accounts = set()
        self.first_available = 0
        self.down = False
        self.calls = []

    def _check(self, name):
        self.calls.append(name)
        if self.down:
            from scfg_core.rpc import RpcUnavailable
            raise RpcUnavailable("fake outage")

    def pay(self, sig, *, mint, payer, owner, amount, slot=None, err=None, addresses=(),
            status="confirmed"):
        tx = solana_tx(sig, mint=mint, payer=payer, owner=owner, amount=amount,
                       slot=self.slot if slot is None else slot, err=err)
        self.txs[sig] = tx
        for a in addresses:
            self.addresses.setdefault(a, []).insert(
                0, {"signature": sig, "slot": tx["slot"], "err": err})
        self.statuses[sig] = {"slot": tx["slot"], "err": err, "confirmationStatus": status,
                              "confirmations": None if status == "finalized" else 1}
        return tx

    def finalize(self, sig):
        self.statuses[sig] = dict(self.statuses[sig], confirmationStatus="finalized",
                                  confirmations=None)

    def get_slot(self, commitment="confirmed"):
        self._check("get_slot")
        return self.slot

    def get_signatures_for_address(self, address, *, until=None, before=None, limit=100,
                                   commitment="confirmed"):
        self._check("get_signatures_for_address")
        sigs = list(self.addresses.get(address, []))
        names = [s["signature"] for s in sigs]
        if until in names:
            sigs = sigs[:names.index(until)]
        names = [s["signature"] for s in sigs]
        if before in names:
            sigs = sigs[names.index(before) + 1:]
        return [dict(s) for s in sigs[:limit]]

    def get_transaction(self, sig, commitment="confirmed"):
        self._check("get_transaction")
        return self.txs.get(sig)

    def get_transaction_any(self, sig, *, min_slot=None, commitment="finalized"):
        self._check("get_transaction_any")
        tx = self.txs.get(sig)
        if tx is not None:
            return tx, False
        return None, min_slot is not None and self.first_available <= min_slot

    def get_signature_statuses(self, sigs):
        self._check("get_signature_statuses")
        return [self.statuses.get(s) for s in sigs]

    def account_exists(self, address):
        self._check("account_exists")
        return address in self.accounts
