import pytest
from solders.keypair import Keypair

from cryptopay_core.chains import MAINNET
from cryptopay_core.checkout import MESSAGES, CheckoutController
from cryptopay_core.config import add_item, build_config
from cryptopay_core.engine import Engine
from cryptopay_core.local_server import CheckoutServer
from cryptopay_core.payments import prepare_payment
from cryptopay_core.restore_flow import RestoreController
from cryptopay_core.solana.base58 import b58encode
from cryptopay_core.solana.pda import associated_token_address

from fakes import FakeChain, FakeClock, FakeSolanaChain, immediate_submitter

DEV_EVM = "0x52908400098527886E0F7030069857D2E4169EE7"
DEV = str(Keypair().pubkey())
MINT = MAINNET["solana"].tokens["USDC"].address
SIG = b58encode((7).to_bytes(64, "big"))
EVM_TX = "0x" + "ab" * 32


def setup(unready=frozenset()):
    cfg = build_config(DEV_EVM, solana_wallet=DEV)
    add_item(cfg, "chapter2", code=37, price="4.99", token="USDC", title="Chapter 2")
    sol, evm = FakeSolanaChain(), FakeChain()
    e = Engine(cfg, rpc_factory=lambda c: sol if c.family == "solana" else evm,
               clock=FakeClock(0.0))
    spec = prepare_payment(cfg, "unlock", item="chapter2")
    ctl = CheckoutController(config=cfg, spec=spec, submit=immediate_submitter(e),
                             unready=lambda: unready, game_name="Ruby Dreams")
    return ctl, e


def started(chain):
    ctl, e = setup()
    ctl.select_chain(chain)
    ctl.poll()
    return ctl, e


def test_solana_is_offered_and_unready_tokens_are_hidden():
    ctl, _ = setup()
    assert ("solana", "Solana", "lowest") in ctl.chain_options()
    ctl, _ = setup(unready=frozenset({("solana", "USDC")}))
    assert "solana" not in [k for k, _, _ in ctl.chain_options()]
    assert ctl.dev_warnings() == ["Your Solana wallet can't receive USDC yet: "
                                  "receive any amount once."]


def test_only_unready_routes_shows_the_unavailable_message():
    cfg = build_config(solana_wallet=DEV)
    add_item(cfg, "chapter2", code=37, price="4.99", token="USDC", title="Chapter 2")
    spec = prepare_payment(cfg, "unlock", item="chapter2")
    ctl = CheckoutController(config=cfg, spec=spec, submit=lambda fn: None,
                             unready=lambda: frozenset({("solana", "USDC")}))
    assert ctl.chain_options() == [] and ctl.message() == MESSAGES["solana_unavailable"]


def test_solana_recipient_and_payment_link():
    ctl, _ = started("solana")
    assert ctl.family == "solana" and ctl.recipient == DEV and ctl.token_address == MINT
    uri = ctl.payment_uri()
    assert uri.startswith(f"solana:{DEV}?amount=4.9937") and f"spl-token={MINT}" in uri
    assert f"reference={ctl.session['reference']}" in uri and "label=Ruby%20Dreams" in uri
    assert ctl.session["dest"] == associated_token_address(DEV, MINT)


def test_evm_chain_still_uses_eip681():
    ctl, _ = started("base")
    assert ctl.family == "evm" and ctl.recipient == DEV_EVM
    assert ctl.payment_uri().startswith("ethereum:")


def test_pasting_the_wrong_family_is_an_invalid_hash():
    """Review focus 1."""
    ctl, e = started("solana")
    assert ctl.submit_hash(EVM_TX) is False and ctl.error == "invalid_hash"
    assert ctl.submit_hash(SIG) is True and e.pending[ctl.session_id]["hint_tx"] == SIG
    ctl, _ = started("base")
    assert ctl.submit_hash(SIG) is False and ctl.error == "invalid_hash"
    assert "signature" in MESSAGES["invalid_hash"]


def test_restore_accepts_signatures():
    submitted = []
    ctl = RestoreController(submit=lambda fn: submitted.append(fn))
    assert ctl.check(SIG) is True and submitted
    assert ctl.check("nonsense") is False and ctl.error == "invalid_hash"


def test_local_server_accepts_a_signature_hint():
    import http.client
    import json
    got = []
    srv = CheckoutServer(load_asset=lambda n: b"", session_info={}, get_status=lambda: {},
                         on_tx=lambda tx, sender: got.append((tx, sender)))
    srv.start()
    try:
        conn = http.client.HTTPConnection("127.0.0.1", srv.port)
        conn.request("POST", f"/c/{srv.secret}/tx", json.dumps({"tx_hash": SIG, "from": DEV}),
                     {"Host": f"127.0.0.1:{srv.port}", "Content-Type": "application/json"})
        assert conn.getresponse().status == 202
    finally:
        srv.stop()
    assert got == [(SIG, DEV)]


def test_transaction_that_never_landed_lets_the_player_pay_again():
    from cryptopay_core.watcher import StepOutcome

    ctl, _ = started("solana")
    assert ctl.submit_hash(SIG) is True and ctl.hint_tx == SIG
    ctl.on_outcome(StepOutcome(ctl.session_id, "waiting", error="tx_not_landed"))
    assert ctl.hint_tx is None and ctl.state == "waiting"
    assert ctl.message() == "That transaction didn't reach the network. You can pay again."
    assert ctl.status_info()["tx_hash"] is None
    assert ctl.status_info()["error"] == "tx_not_landed"


SIG2 = b58encode((8).to_bytes(64, "big"))


def test_status_names_the_signature_that_never_landed():
    from cryptopay_core.watcher import StepOutcome

    ctl, _ = started("solana")
    assert ctl.status_info()["dropped_tx"] is None
    ctl.submit_hash(SIG)
    ctl.on_outcome(StepOutcome(ctl.session_id, "waiting", error="tx_not_landed", hint_tx=SIG))
    info = ctl.status_info()
    assert (info["error"], info["tx_hash"], info["dropped_tx"]) == ("tx_not_landed", None, SIG)


def test_a_new_hint_clears_the_old_not_landed_status():
    from cryptopay_core.watcher import StepOutcome

    ctl, e = started("solana")
    ctl.submit_hash(SIG)
    ctl.on_outcome(StepOutcome(ctl.session_id, "waiting", error="tx_not_landed", hint_tx=SIG))
    ctl._report_hint(SIG2, DEV)                         # the page's POST /tx for the next send
    info = ctl.status_info()
    assert (info["error"], info["tx_hash"], info["dropped_tx"]) == (None, SIG2, None)
    assert ctl.message() is None and e.pending[ctl.session_id]["hint_tx"] == SIG2


@pytest.mark.parametrize("error", ["tx_not_landed", "tx_reverted", "tx_not_a_payment"])
def test_a_verdict_on_an_older_hint_does_not_touch_the_newer_one(error):
    """The worker may judge the old hint just before the page reports the next one."""
    from cryptopay_core.watcher import StepOutcome

    ctl, _ = started("solana")
    ctl.submit_hash(SIG)
    ctl._report_hint(SIG2, DEV)
    ctl.on_outcome(StepOutcome(ctl.session_id, "waiting", error=error, hint_tx=SIG))
    info = ctl.status_info()
    assert (info["error"], info["tx_hash"], info["dropped_tx"]) == (None, SIG2, None)
