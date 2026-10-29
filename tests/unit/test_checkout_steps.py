"""The stepped payment sheet: CheckoutController.step and its navigation."""

from cryptopay_core.chains import MAINNET
from cryptopay_core.checkout import CheckoutController
from cryptopay_core.config import add_item, build_config
from cryptopay_core.engine import Engine
from cryptopay_core.payments import prepare_payment
from cryptopay_core.watcher import StepOutcome

from fakes import FakeChain, FakeClock, immediate_submitter, transfer_log

DEV = "0x52908400098527886E0F7030069857D2E4169EE7"
PAYER = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed"
USDC_ARB = MAINNET["arbitrum"].tokens["USDC"].address
TX = "0x" + "ab" * 32
ONLY_MANUAL = {"browser": {"enabled": False}, "qr": {"enabled": False}}


class FakeServer:
    url = "http://127.0.0.1:1234/c/secret/"

    def __init__(self, **kw):
        self.on_tx = kw["on_tx"]

    def start(self):
        return self.url

    def stop(self):
        pass


def setup(kind="unlock", chains=None, methods=None, browser_wallet=None):
    cfg = build_config(DEV, chains=chains, methods=methods)
    add_item(cfg, "chapter2", code=37, price="4.99", token="USDC", title="Chapter 2")
    fakes = {}
    clock = FakeClock(100.0)
    engine = Engine(cfg, rpc_factory=lambda c: fakes.setdefault(c.key, FakeChain(head=1000)),
                    clock=clock)
    if kind == "unlock":
        spec = prepare_payment(cfg, "unlock", item="chapter2")
    else:
        spec = prepare_payment(cfg, "tip", amount="3", token="USDC")
    ctl = CheckoutController(config=cfg, spec=spec, submit=immediate_submitter(engine),
                             server_factory=FakeServer, browser_wallet=browser_wallet)
    return ctl, engine, fakes, clock


def on_method_step(**kw):
    ctl, engine, fakes, clock = setup(**kw)
    ctl.choose_network("arbitrum")
    ctl.poll()
    return ctl, engine, fakes, clock


def deliver(ctl, engine):
    for o in engine.tick():
        ctl.on_outcome(o)


# --- where the sheet starts ---------------------------------------------------------

def test_unlock_starts_on_network_and_tip_on_amount():
    ctl, *_ = setup()
    assert ctl.step == "network" and ctl.can_go_back is False
    ctl, *_ = setup(kind="tip")
    assert ctl.step == "amount" and ctl.can_go_back is False


def test_step_path_counts_the_steps_shown():
    ctl, *_ = setup()
    assert ctl.step_path() == ["network", "method", "browser", "waiting"]
    assert ctl.step_dots() == (0, 4)
    ctl, *_ = setup(kind="tip")
    assert ctl.step_path() == ["amount", "network", "method", "browser", "waiting"]


# --- forward ------------------------------------------------------------------------

def test_network_waits_for_the_session_then_moves_to_method():
    ctl, engine, *_ = setup()
    ctl.choose_network("arbitrum")
    assert ctl.step == "network" and ctl.state == "starting"
    ctl.poll()
    assert ctl.step == "method" and ctl.nav_dir == 1
    assert ctl.session_id in engine.pending
    assert ctl.step_dots() == (1, 4)


def test_choosing_a_method_opens_its_page():
    for name in ("browser", "qr", "manual"):
        ctl, *_ = on_method_step()
        ctl.choose_method(name)
        assert ctl.step == name and ctl.method == name


def test_unknown_method_is_ignored():
    ctl, *_ = on_method_step(methods={"qr": {"enabled": False}})
    ctl.choose_method("qr")
    assert ctl.step == "method"


def test_opening_the_browser_moves_to_waiting():
    ctl, *_ = on_method_step()
    ctl.choose_method("browser")
    assert ctl.open_browser() == FakeServer.url
    assert ctl.step == "waiting" and ctl.can_go_back is True


def test_browser_failure_returns_to_method():
    ctl, *_ = on_method_step()
    ctl.choose_method("browser")

    def broken(**kw):
        raise OSError("no sockets")
    ctl.server_factory = broken
    assert ctl.open_browser() is None
    assert ctl.step == "method" and ctl.nav_dir == -1
    assert ctl.error == "browser_unavailable" and ctl.method == "qr"


def test_ive_sent_it_moves_qr_and_manual_to_waiting():
    for name in ("qr", "manual"):
        ctl, *_ = on_method_step()
        ctl.choose_method(name)
        ctl.mark_sent()
        assert ctl.step == "waiting" and ctl.can_go_back is True


def test_tip_amount_step_applies_a_typed_amount():
    ctl, *_ = setup(kind="tip")
    ctl.custom_amount = "0.10"
    assert ctl.next_step() is False
    assert ctl.step == "amount" and ctl.error == "invalid_amount"
    ctl.custom_amount = "7.50"
    assert ctl.next_step() is True
    assert ctl.step == "network" and str(ctl.amount) == "7.50"


# --- skipping single-option steps -------------------------------------------------------

def test_single_network_is_skipped_for_unlocks():
    ctl, engine, *_ = setup(chains=["base"])
    assert ctl.state == "starting" and ctl.chain_key == "base"
    ctl.poll()
    assert ctl.step == "method" and ctl.can_go_back is False
    assert ctl.step_path() == ["method", "browser", "waiting"]


def test_single_network_is_skipped_for_tips_both_ways():
    ctl, *_ = setup(kind="tip", chains=["base"])
    ctl.next_step()
    ctl.poll()
    assert ctl.step == "method"
    ctl.back()
    assert ctl.step == "amount" and ctl.nav_dir == -1


def test_single_method_is_skipped_both_ways():
    ctl, *_ = on_method_step(methods=ONLY_MANUAL)
    assert ctl.step == "manual"
    ctl.back()
    assert ctl.step == "network"


# --- back -------------------------------------------------------------------------------

def test_back_walks_the_path_and_keeps_the_session():
    ctl, engine, *_ = on_method_step()
    sid = ctl.session_id
    ctl.choose_method("qr")
    ctl.mark_sent()
    ctl.back()
    assert ctl.step == "qr" and ctl.nav_dir == -1
    ctl.back()
    assert ctl.step == "method"
    ctl.back()
    assert ctl.step == "network" and ctl.can_go_back is False
    ctl.back()                                       # nothing before network on an unlock
    assert ctl.step == "network"
    assert ctl.session_id == sid and sid in engine.pending


def test_choosing_the_same_network_again_keeps_the_session():
    ctl, engine, *_ = on_method_step()
    sid = ctl.session_id
    ctl.back()
    ctl.choose_network("arbitrum")
    assert ctl.step == "method" and ctl.session_id == sid and ctl.nav_dir == 1


def test_choosing_another_network_restarts_the_session():
    ctl, engine, *_ = on_method_step()
    sid = ctl.session_id
    ctl.back()
    ctl.choose_network("base")
    ctl.poll()
    assert ctl.step == "method" and ctl.session_id != sid
    assert sid not in engine.pending


def test_back_to_amount_and_same_amount_keeps_the_session():
    ctl, engine, *_ = on_method_step(kind="tip")
    sid = ctl.session_id
    ctl.back()
    ctl.back()
    assert ctl.step == "amount"
    assert ctl.set_tip_amount("3") is True and ctl.session_id == sid
    assert ctl.set_tip_amount("5") is True
    ctl.poll()
    assert ctl.session_id != sid and ctl.step == "amount"
    ctl.next_step()
    ctl.choose_network("arbitrum")
    assert ctl.step == "method"


def test_back_clears_the_message():
    ctl, *_ = on_method_step()
    ctl.choose_method("manual")
    ctl.submit_hash("garbage")
    assert ctl.error == "invalid_hash"
    ctl.back()
    assert ctl.error is None


# --- automatic moves ----------------------------------------------------------------------

def test_a_seen_payment_moves_to_waiting_and_hides_back():
    ctl, engine, fakes, _ = on_method_step()
    ctl.choose_method("qr")
    fakes["arbitrum"].logs.append(transfer_log(USDC_ARB, PAYER, DEV, ctl.session["units"],
                                               block=999, tx_hash=TX))
    deliver(ctl, engine)
    assert ctl.state == "confirming"
    assert ctl.step == "waiting" and ctl.nav_dir == 1 and ctl.can_go_back is False


def test_a_pasted_hash_moves_to_waiting_and_hides_back():
    ctl, *_ = on_method_step()
    ctl.choose_method("manual")
    assert ctl.submit_hash(TX) is True
    assert ctl.step == "waiting" and ctl.can_go_back is False


def test_a_hash_from_the_browser_page_moves_to_waiting():
    ctl, *_ = on_method_step()
    ctl.choose_method("browser")
    ctl.open_browser()
    ctl._server.on_tx(TX, PAYER)
    assert ctl.step == "waiting" and ctl.can_go_back is False


def test_a_dropped_transaction_gives_back_the_arrow():
    ctl, *_ = on_method_step()
    ctl.choose_method("manual")
    ctl.submit_hash(TX)
    ctl.on_outcome(StepOutcome(ctl.session_id, "waiting", error="tx_not_landed", hint_tx=TX))
    assert ctl.step == "waiting" and ctl.can_go_back is True
    ctl.back()
    assert ctl.step == "manual"


def test_final_states_move_to_done():
    ctl, engine, fakes, _ = on_method_step()
    ctl.choose_method("qr")
    fakes["arbitrum"].logs.append(transfer_log(USDC_ARB, PAYER, DEV, ctl.session["units"],
                                               block=999, tx_hash=TX))
    deliver(ctl, engine)
    fakes["arbitrum"].head = 1100
    deliver(ctl, engine)
    assert ctl.state == "paid" and ctl.step == "done" and ctl.can_go_back is False

    ctl, engine, _, clock = on_method_step()
    ctl.choose_method("browser")
    clock.advance(1800)
    deliver(ctl, engine)
    assert ctl.state == "expired" and ctl.step == "done"


def test_start_failure_returns_to_network_with_the_message():
    ctl, engine, fakes, _ = setup(chains=["base"])
    ctl, engine, fakes, _ = setup()
    fakes["arbitrum"] = FakeChain()
    fakes["arbitrum"].down = True
    ctl.choose_network("arbitrum")
    ctl.poll()
    assert ctl.step == "network" and ctl.error == "network_unavailable"


def test_start_failure_on_a_skipped_network_shows_the_network_step():
    cfg_chains = ["base"]
    ctl, engine, fakes, _ = setup(kind="tip", chains=cfg_chains)
    fakes["base"] = FakeChain()
    fakes["base"].down = True
    ctl.next_step()
    ctl.poll()
    assert ctl.step == "network" and ctl.error == "network_unavailable"
    assert ctl.can_go_back is True
    fakes["base"].down = False
    ctl.choose_network("base")
    ctl.poll()
    assert ctl.step == "method"



def test_a_session_restarted_on_the_amount_step_waits_for_a_network_pick():
    ctl, *_ = setup(kind="tip")
    ctl.next_step()
    ctl.choose_network("arbitrum")
    ctl.poll()
    ctl.back()
    ctl.back()
    assert ctl.step == "amount"
    ctl.set_tip_amount("5")                          # restarts the session on arbitrum
    ctl.next_step()
    ctl.poll()                                       # ... which is ready now
    assert ctl.step == "network"                     # but the player hasn't picked yet
    ctl.choose_network("arbitrum")
    assert ctl.step == "method"


def test_leaving_the_network_step_while_starting_stays_put():
    ctl, *_ = setup(kind="tip")
    ctl.next_step()
    ctl.choose_network("arbitrum")
    ctl.back()
    ctl.poll()
    assert ctl.step == "amount" and ctl.state == "waiting"


# --- web build ----------------------------------------------------------------------

def test_web_offers_browser_wallet_only_when_the_page_has_one():
    ctl, *_ = on_method_step(browser_wallet=lambda chain: False)
    assert ctl.methods() == ["qr", "manual"]
    ctl, *_ = on_method_step(browser_wallet=lambda chain: chain.key == "arbitrum")
    assert ctl.methods() == ["browser", "qr", "manual"]


def test_web_browser_wallet_ignores_the_mobile_flag():
    ctl, *_ = setup(browser_wallet=lambda chain: True)
    ctl.mobile = True
    ctl.choose_network("arbitrum")
    ctl.poll()
    assert "browser" in ctl.methods()


class PageWithPump(FakeServer):
    def __init__(self, **kw):
        super().__init__(**kw)
        self.pumps = self.reopens = 0

    def pump(self):
        self.pumps += 1

    def reopen(self):
        self.reopens += 1


def test_the_controller_pumps_and_reopens_an_overlay_page():
    ctl, *_ = on_method_step()
    ctl.server_factory = PageWithPump
    ctl.choose_method("browser")
    ctl.open_browser()
    ctl.poll()
    assert ctl._server.pumps == 1
    ctl.open_browser()
    assert ctl._server.reopens == 1
