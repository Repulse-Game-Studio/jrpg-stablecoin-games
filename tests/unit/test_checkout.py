from decimal import Decimal

import pytest

from scfg_core.chains import MAINNET
from scfg_core.checkout import CheckoutController
from scfg_core.config import add_item, build_config
from scfg_core.engine import Engine
from scfg_core.payments import prepare_payment

from fakes import FakeChain, FakeClock, immediate_submitter, transfer_log

DEV = "0x52908400098527886E0F7030069857D2E4169EE7"
PAYER = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed"
USDC_ARB = MAINNET["arbitrum"].tokens["USDC"].address
TX = "0x" + "ab" * 32


class FakeServer:
    def __init__(self, session_info, on_tx, get_status):
        self.session_info, self.on_tx, self.get_status = session_info, on_tx, get_status
        self.started = self.stopped = False
        self.url = "http://127.0.0.1:1234/c/secret/"

    def start(self):
        self.started = True
        return self.url

    def stop(self):
        self.stopped = True


def setup(kind="unlock", mobile=False, methods=None, appearance=None, **spec_kw):
    cfg = build_config(DEV, methods=methods, **(appearance or {}))
    add_item(cfg, "chapter2", code=37, price="4.99", token="USDC", title="Chapter 2")
    fakes = {}

    def factory(chain):
        return fakes.setdefault(chain.key, FakeChain(head=1000))

    clock = FakeClock(100.0)
    engine = Engine(cfg, rpc_factory=factory, clock=clock)
    if kind == "unlock":
        spec = prepare_payment(cfg, "unlock", item="chapter2")
    else:
        spec = prepare_payment(cfg, "tip", amount="3", token="USDC", **spec_kw)
    servers = []

    def server_factory(**kw):
        s = FakeServer(**kw)
        servers.append(s)
        return s

    ctl = CheckoutController(config=cfg, spec=spec, submit=immediate_submitter(engine),
                             mobile=mobile, server_factory=server_factory)
    return ctl, engine, fakes, clock, servers


def started(**kw):
    ctl, engine, fakes, clock, servers = setup(**kw)
    ctl.select_chain("arbitrum")
    ctl.poll()
    return ctl, engine, fakes, clock, servers


def test_initial_state_and_chain_options():
    ctl, *_ = setup()
    assert ctl.state == "choose" and ctl.session is None
    assert [k for k, _, _ in ctl.chain_options()] == ["arbitrum", "base", "polygon", "ethereum"]
    assert ctl.pay_amount_text == "4.99"


def test_select_chain_starts_session():
    ctl, engine, *_ = started()
    assert ctl.state == "waiting"
    assert ctl.session["id"] in engine.pending
    assert ctl.method == "browser"
    assert ctl.required == 20
    assert ctl.pay_amount_text.startswith("4.9937")


def test_select_unknown_chain_raises():
    ctl, *_ = setup()
    with pytest.raises(ValueError):
        ctl.select_chain("solana")


def test_switching_chain_discards_old_session():
    ctl, engine, *_ = started()
    old = ctl.session_id
    ctl.select_chain("base")
    ctl.poll()
    assert old not in engine.pending and ctl.session_id in engine.pending
    assert ctl.session["chain"] == "base"


def test_start_failure_returns_to_choose_with_error():
    ctl, engine, fakes, *_ = setup()
    fakes["arbitrum"] = FakeChain()
    fakes["arbitrum"].down = True
    ctl.select_chain("arbitrum")
    ctl.poll()
    assert ctl.state == "choose" and ctl.error == "network_unavailable"
    assert ctl.message()


def test_methods_on_mobile_and_config():
    ctl, *_ = started(mobile=True)
    assert ctl.methods() == ["qr", "manual"]
    ctl, *_ = started(methods={"qr": {"chains": ["base"]}})
    assert ctl.methods() == ["browser", "manual"]      # QR limited to base, we're on arbitrum


def test_payment_uri():
    ctl, *_ = started()
    assert ctl.payment_uri() == (f"ethereum:{USDC_ARB}@42161/transfer?address={DEV}"
                                 f"&uint256={ctl.session['units']}")


def test_submit_hash_validation_and_hint():
    ctl, engine, *_ = started()
    assert ctl.submit_hash("garbage") is False and ctl.error == "invalid_hash"
    ctl.tx_input = TX
    assert ctl.submit_hash() is True
    assert engine.pending[ctl.session_id]["hint_tx"] == TX
    assert ctl.hint_tx == TX and ctl.tx_input == "" and ctl.error is None


def test_outcomes_drive_states_to_paid():
    ctl, engine, fakes, *_ = started()
    fakes["arbitrum"].logs.append(transfer_log(USDC_ARB, PAYER, DEV, ctl.session["units"],
                                               block=999, tx_hash=TX))
    for o in engine.tick():
        ctl.on_outcome(o)
    assert ctl.state == "confirming" and ctl.confirmations == 2
    fakes["arbitrum"].head = 1100
    for o in engine.tick():
        ctl.on_outcome(o)
    assert ctl.state == "paid" and ctl.result.status == "paid"
    assert ctl.explorer_url() == f"https://arbiscan.io/tx/{TX}"
    assert ctl.finish().status == "paid"


def test_outcomes_for_other_sessions_are_ignored():
    from scfg_core.watcher import StepOutcome
    ctl, *_ = started()
    ctl.on_outcome(StepOutcome("ps_other", "paid"))
    assert ctl.state == "waiting"


def test_reorg_and_hint_errors():
    from scfg_core.watcher import StepOutcome
    ctl, *_ = started()
    ctl.hint_tx = TX
    ctl.on_outcome(StepOutcome(ctl.session_id, "confirming", confirmations=3, required=20))
    ctl.on_outcome(StepOutcome(ctl.session_id, "reorged"))
    assert ctl.state == "waiting" and ctl.confirmations == 0
    ctl.on_outcome(StepOutcome(ctl.session_id, "waiting", error="tx_not_a_payment"))
    assert ctl.error == "tx_not_a_payment" and ctl.hint_tx is None


def test_expiry_shows_expired_state():
    ctl, engine, fakes, clock, _ = started()
    clock.advance(1800)
    for o in engine.tick():
        ctl.on_outcome(o)
    assert ctl.state == "expired" and ctl.finish().status == "expired"


def test_cancel_marks_session_and_returns_result():
    ctl, engine, *_ = started()
    result = ctl.cancel()
    assert result.status == "cancelled" and result.session_id == ctl.session_id
    assert engine.pending[ctl.session_id]["ui_status"] == "cancelled"


def test_keep_playing():
    ctl, engine, *_ = started()
    assert ctl.keep_playing().status == "pending"
    assert engine.pending[ctl.session_id]["ui_status"] == "pending"


def test_cancel_before_choosing_chain():
    ctl, *_ = setup()
    assert ctl.cancel().status == "cancelled"


def test_tip_amount_changes_restart_session():
    ctl, engine, *_ = started(kind="tip")
    first = ctl.session_id
    assert ctl.set_tip_amount("5") is True
    ctl.poll()
    assert ctl.amount == Decimal("5.00") and ctl.session["price"] == "5.00"
    assert first not in engine.pending
    assert ctl.set_tip_amount("0.10") is False and ctl.error == "invalid_amount"
    ctl.custom_amount = " 7.50 "
    assert ctl.apply_custom_amount() is True and ctl.amount == Decimal("7.50")


def test_tip_amount_ignored_for_unlocks():
    ctl, *_ = started()
    assert ctl.set_tip_amount("1") is False


def test_browser_server_lifecycle_and_hints():
    ctl, engine, _, _, servers = started()
    url = ctl.start_browser()
    assert url == "http://127.0.0.1:1234/c/secret/"
    assert ctl.start_browser() == url and len(servers) == 1        # reused
    s = servers[0]
    assert s.started and s.session_info["amount_units"] == str(ctl.session["units"])
    assert s.session_info["chain_id_hex"] == "0xa4b1"
    assert s.session_info["logo"] is False
    assert s.session_info["accent"] == "#0e7c74" and s.session_info["on_accent"] == "#ffffff"
    s.on_tx(TX, PAYER)                                               # page reports the tx
    assert engine.pending[ctl.session_id]["hint_tx"] == TX
    assert s.get_status()["tx_hash"] == TX
    ctl.cancel()
    assert s.stopped


def test_browser_failure_falls_back():
    ctl, *_ = started()

    def broken(**kw):
        raise OSError("no sockets")
    ctl.server_factory = broken
    assert ctl.start_browser() is None
    assert ctl.error == "browser_unavailable"
    assert "browser" not in ctl.methods() and ctl.method == "qr"


def test_browser_session_info_carries_appearance():
    ctl, *_, servers = started(appearance={"logo": "gui/logo.png", "accent": "#ffcc00"})
    ctl.start_browser()
    info = servers[0].session_info
    assert info["logo"] is True
    assert info["accent"] == "#ffcc00" and info["on_accent"] == "#17202e"
