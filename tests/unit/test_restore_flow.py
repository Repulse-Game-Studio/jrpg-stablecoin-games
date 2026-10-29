from scfg_core.chains import MAINNET
from scfg_core.config import add_item, build_config
from scfg_core.engine import Engine
from scfg_core.restore import TIP_CHOICE
from scfg_core.restore_flow import RestoreController

from fakes import FakeChain, immediate_submitter, transfer_log

DEV = "0x52908400098527886E0F7030069857D2E4169EE7"
PAYER = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed"
USDC_ARB = MAINNET["arbitrum"].tokens["USDC"].address
TX = "0x" + "ab" * 32


def setup(*logs, status="0x1"):
    cfg = build_config(DEV)
    add_item(cfg, "chapter2", code=37, price="4.99", token="USDC", title="Chapter 2")
    chains = {k: FakeChain() for k in MAINNET}
    if logs or status == "0x0":
        chains["arbitrum"].receipts[TX] = {"status": status, "logs": list(logs)}
    engine = Engine(cfg, rpc_factory=lambda c: chains[c.key])
    return RestoreController(submit=immediate_submitter(engine)), engine


def run(ctl, text=TX):
    ctl.tx_input = text
    ctl.check()
    ctl.poll()
    if ctl.state == "saving":
        ctl.poll()


def test_automatic_restore():
    ctl, engine = setup(transfer_log(USDC_ARB, PAYER, DEV, 4_993_742, block=5, tx_hash=TX))
    run(ctl)
    assert ctl.state == "done"
    assert ctl.result().status == "restored" and ctl.result().restored[0]["item"] == "chapter2"
    assert engine.snapshot().owned == frozenset({"chapter2"})


def test_already_restored_is_nothing_found():
    ctl, engine = setup(transfer_log(USDC_ARB, PAYER, DEV, 4_993_742, block=5, tx_hash=TX))
    run(ctl)
    ctl2 = RestoreController(submit=ctl.submit)
    run(ctl2)
    assert ctl2.state == "done" and ctl2.result().status == "nothing_found"


def test_ambiguous_amount_needs_choice():
    ctl, engine = setup(transfer_log(USDC_ARB, PAYER, DEV, 5_000_000, block=5, tx_hash=TX))
    run(ctl)
    assert ctl.state == "choose" and ctl.pending_choices() == [0]
    assert ctl.candidates[0].choices == ("chapter2", TIP_CHOICE)
    ctl.choose(0, "chapter2")
    ctl.confirm()
    ctl.poll()
    assert ctl.state == "done" and engine.snapshot().owned == frozenset({"chapter2"})


def test_restore_waits_until_every_choice_is_answered():
    ctl, engine = setup(transfer_log(USDC_ARB, PAYER, DEV, 5_000_000, block=5, tx_hash=TX))
    run(ctl)
    assert ctl.can_confirm is False
    ctl.confirm()  # e.g. a click that raced the screen: nothing happens
    assert ctl.state == "choose"
    ctl.choose(0, TIP_CHOICE)
    assert ctl.can_confirm is True


def test_errors_return_to_input():
    ctl, _ = setup()
    run(ctl, "nonsense")
    assert ctl.state == "input" and ctl.error == "invalid_hash"
    run(ctl)
    assert ctl.state == "input" and ctl.error == "tx_not_found" and ctl.message()


def test_reverted():
    ctl, _ = setup(status="0x0")
    run(ctl)
    assert ctl.error == "tx_reverted"


def test_cancel_result():
    ctl, _ = setup()
    assert ctl.result().status == "cancelled"


def test_a_blocked_paste_shows_the_restore_wording():
    ctl, _ = setup()
    ctl.error = "paste_blocked"
    assert "Type the hash instead" in ctl.message()
