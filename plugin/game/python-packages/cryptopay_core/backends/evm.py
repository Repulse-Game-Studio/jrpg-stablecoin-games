"""EVM chains: the original watcher, re-verification and restore code behind the
Backend interface. Behaviour is unchanged."""

from __future__ import annotations

from .. import restore, reverify, watcher
from ..rpc import RpcClient
from ..sessions import normalize_tx_hash

START_BLOCK_MARGIN = 5


class EvmBackend:
    family = "evm"

    def make_rpc(self, chain):
        return RpcClient(chain.rpc, log_range=chain.max_log_range)

    def normalize_tx(self, text):
        return normalize_tx_hash(text)

    def start(self, *, rpc, chain, wallet, token):
        head = yield from rpc.block_number()
        return {"start_block": max(0, head - START_BLOCK_MARGIN)}

    def advance(self, pending, receipts, sid, *, rpc, chain, wallet, items, now):
        return (yield from watcher.advance(pending, receipts, sid, rpc=rpc, chain=chain,
                                           wallet=wallet, items=items, now=now))

    def reverify(self, receipt, *, rpc, chain, wallets):
        return (yield from reverify.check_evm_receipt(receipt, rpc=rpc, chain=chain,
                                                      wallets=wallets))

    def lookup(self, tx, *, config, chain, rpc, receipts):
        return (yield from restore.lookup_evm(tx, config=config, chain=chain, rpc=rpc,
                                              receipts=receipts))

    def check_ready(self, *, rpc, chain, wallet):
        return {name: True for name in chain.tokens}
        yield  # never reached: cooperative like every backend call, but needs no network
