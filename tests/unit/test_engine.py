from decimal import Decimal

import pytest

from cryptopay_core.chains import MAINNET
from cryptopay_core.config import add_item, build_config
from cryptopay_core.engine import MIN_TIP, START_BLOCK_MARGIN, Engine
from cryptopay_core.errors import ConfigError

from fakes import FakeChain, FakeClock, transfer_log

DEV = "0x52908400098527886E0F7030069857D2E4169EE7"
PAYER = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed"
USDC_ARB = MAINNET["arbitrum"].tokens["USDC"].address
TX = "0x" + "ab" * 32


def make_engine(**cfg_kw):
    cfg = build_config(DEV, **cfg_kw)
    add_item(cfg, "chapter2", code=37, price="4.99", token="USDC", title="Chapter 2")
    chains = {}

    def factory(chain):
        chains.setdefault(chain.key, FakeChain(head=1000))
        chains[chain.key].chain_obj = chain
        return chains[chain.key]

    clock = FakeClock(100.0)
    return Engine(cfg, rpc_factory=factory, clock=clock), chains, clock


def test_start_unlock_session_uses_item_defaults():
    e, chains, _ = make_engine()
    rec = e.start_session("unlock", "arbitrum", item="chapter2")
    assert (rec["kind"], rec["item"], rec["token"], rec["title"], rec["code"]) == (
        "unlock", "chapter2", "USDC", "Chapter 2", 37)
    assert rec["price"] == "4.99"
    assert rec["start_block"] == 1000 - START_BLOCK_MARGIN
    assert rec["created_at"] == 100.0
    assert rec["id"] in e.pending


def test_start_session_returns_a_copy():
    e, _, _ = make_engine()
    rec = e.start_session("unlock", "arbitrum", item="chapter2")
    rec["state"] = "hacked"
    assert e.pending[rec["id"]]["state"] == "awaiting"


def test_start_tip_session():
    e, _, _ = make_engine()
    rec = e.start_session("tip", "base", amount="3", token="USDC")
    assert (rec["code"], rec["title"], rec["price"], rec["item"]) == (0, "Tip", "3.00", None)


@pytest.mark.parametrize("args, kwargs", [
    (("unlock", "arbitrum"), dict(item="missing")),
    (("unlock", "arbitrum"), dict(item="chapter2", amount="1.00")),        # below min_price
    (("tip", "arbitrum"), dict(token="USDC")),                             # no amount
    (("tip", "arbitrum"), dict(amount="1")),                               # no token
    (("tip", "arbitrum"), dict(amount=str(MIN_TIP - Decimal("0.01")), token="USDC")),
    (("refund", "arbitrum"), dict()),
    (("tip", "base"), dict(amount="3", token="USDT")),                    # no USDT on base
    (("tip", "solana"), dict(amount="3", token="USDC")),
])
def test_start_session_rejects(args, kwargs):
    e, _, _ = make_engine()
    with pytest.raises(ConfigError):
        e.start_session(*args, **kwargs)
    assert e.pending == {}


def test_start_session_rejects_float_amount():
    e, _, _ = make_engine()
    with pytest.raises(TypeError):
        e.start_session("tip", "base", amount=3.0, token="USDC")


def test_start_session_rejects_disabled_chain():
    e, _, _ = make_engine(chains=["base"])
    with pytest.raises(ConfigError):
        e.start_session("unlock", "arbitrum", item="chapter2")


def test_chain_applies_config_overrides():
    e, _, _ = make_engine(rpc_urls={"arbitrum": ["https://mine"]}, confirmations={"arbitrum": 2})
    c = e.chain("arbitrum")
    assert c.rpc[0] == "https://mine" and c.confirmations == 2
    assert set(c.tokens) == {"USDC", "USDT"}     # full registry: old receipts stay verifiable


def test_rpc_clients_are_cached_per_chain():
    e, chains, _ = make_engine()
    assert e.rpc("arbitrum") is e.rpc("arbitrum")
    assert e.rpc("arbitrum") is not e.rpc("base")


def test_end_to_end_tick_pays_and_updates_snapshot():
    e, chains, clock = make_engine()
    rec = e.start_session("unlock", "arbitrum", item="chapter2")
    chain = chains["arbitrum"]
    chain.logs.append(transfer_log(USDC_ARB, PAYER, DEV, rec["units"], block=999, tx_hash=TX))
    assert [o.status for o in e.tick()] == ["confirming"]
    assert "chapter2" not in e.snapshot().owned
    chain.head = 999 + 19
    outcomes = e.tick()
    assert outcomes[0].status == "paid"
    snap = e.snapshot()
    assert snap.owned == frozenset({"chapter2"}) and snap.pending == {}
    assert len(snap.receipts) == 1


def test_report_hint_and_ui_status():
    e, _, clock = make_engine()
    rec = e.start_session("unlock", "arbitrum", item="chapter2")
    e.report_hint(rec["id"], TX.upper().replace("0X", "0x"))
    e.set_ui_status(rec["id"], "pending")
    stored = e.pending[rec["id"]]
    assert stored["hint_tx"] == TX and stored["hint_at"] == 100.0 and stored["ui_status"] == "pending"


def test_engine_copies_initial_state():
    pending = {"ps_x": {"chain": "arbitrum"}}
    cfg = build_config(DEV)
    e = Engine(cfg, pending=pending, receipts={}, rpc_factory=lambda c: FakeChain())
    e.pending["ps_x"]["chain"] = "base"
    assert pending["ps_x"]["chain"] == "arbitrum"


def test_snapshot_is_detached_from_engine_state():
    e, _, _ = make_engine()
    e.start_session("tip", "base", amount="3", token="USDC")
    snap = e.snapshot()
    next(iter(snap.pending.values()))["state"] = "x"
    assert next(iter(e.pending.values()))["state"] == "awaiting"


def test_reverify_uses_accepted_wallets():
    e, chains, _ = make_engine(previous_wallets=[PAYER])
    e.receipts["k"] = {"chain": "arbitrum", "tx_hash": TX, "log_index": 0, "block": 900,
                       "amount": "1.000000", "token": "USDC", "kind": "tip", "item": None}
    chains_seen = []
    e._rpc_factory = lambda chain: chains_seen.append(chain.key) or FakeChain(head=1000)
    assert e.reverify() == ["k"]           # no matching log in the fake chain -> revoked
    assert chains_seen == ["arbitrum"]


# --- expiry, discard, restore -------------------------------------------------

def test_window_expiry_is_reported_once_and_scanning_continues():
    e, chains, clock = make_engine()
    rec = e.start_session("unlock", "arbitrum", item="chapter2")
    clock.advance(30 * 60)
    expired = [o for o in e.tick() if o.status == "expired"]
    assert len(expired) == 1
    assert expired[0].result.status == "expired" and expired[0].result.session_id == rec["id"]
    assert e.pending[rec["id"]]["ui_status"] == "expired"
    assert [o for o in e.tick() if o.status == "expired"] == []   # not repeated
    assert rec["id"] in e.pending                                   # still watched


def test_cancelled_sessions_do_not_report_expiry():
    e, _, clock = make_engine()
    rec = e.start_session("unlock", "arbitrum", item="chapter2")
    e.set_ui_status(rec["id"], "cancelled")
    clock.advance(31 * 60)
    assert [o for o in e.tick() if o.status == "expired"] == []


def test_set_ui_status_ignores_finished_sessions():
    e, _, _ = make_engine()
    e.set_ui_status("ps_gone", "cancelled")       # no KeyError


def test_discard_session_only_drops_untouched_sessions():
    e, _, _ = make_engine()
    a = e.start_session("unlock", "arbitrum", item="chapter2")
    b = e.start_session("unlock", "arbitrum", item="chapter2")
    e.report_hint(b["id"], TX)
    e.discard_session(a["id"])
    e.discard_session(b["id"])
    assert list(e.pending) == [b["id"]]


def test_restore_through_engine():
    e, chains, _ = make_engine()
    chain = FakeChain()
    chain.receipts[TX] = {"status": "0x1", "logs": [
        transfer_log(USDC_ARB, PAYER, DEV, 4_993_742, block=10, tx_hash=TX)]}
    e._rpcs = {k: (chain if k == "arbitrum" else FakeChain()) for k in MAINNET}
    cands = e.restore_lookup(TX)
    assert cands[0].item == "chapter2"
    assert len(e.restore_apply(cands)) == 1
    assert e.snapshot().owned == frozenset({"chapter2"})


class _Transport:
    """Answers eth_blockNumber until `exc` is set, then raises it from post_json."""

    def __init__(self):
        self.exc = None

    def post_json(self, url, payload, timeout):
        if self.exc is not None:
            raise self.exc
        return {"jsonrpc": "2.0", "id": payload["id"], "result": "0x3e8"}


def test_tick_survives_any_transport_exception_in_each_session():
    """A transport failure that is not a TransportError (a dropped HTTP body, say) must
    still end up in the engine's per-session handler: no crash, no session left out."""
    cfg = build_config(DEV)
    add_item(cfg, "chapter2", code=37, price="4.99", token="USDC", title="Chapter 2")
    transport, errors = _Transport(), []
    e = Engine(cfg, transport=transport, clock=FakeClock(100.0), on_error=errors.append)
    sids = {e.start_session("unlock", "arbitrum", item="chapter2")["id"],
            e.start_session("tip", "arbitrum", amount="3", token="USDC")["id"]}
    transport.exc = RuntimeError("incomplete read")
    out = e.tick()
    assert {o.session_id for o in out} == sids
    assert all((o.status, o.error) == ("waiting", "internal_error") for o in out)
    assert len(errors) == 2 and all(isinstance(x, RuntimeError) for x in errors)
