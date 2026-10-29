import pytest
from solders.keypair import Keypair

from scfg_core.chains import MAINNET
from scfg_core.config import add_item, build_config
from scfg_core.engine import Engine
from scfg_core.solana.base58 import b58encode
from scfg_core.solana.pda import associated_token_address

from fakes import FakeChain, FakeClock, FakeSolanaChain

DEV_EVM = "0x52908400098527886E0F7030069857D2E4169EE7"
DEV = str(Keypair().pubkey())
PAYER = str(Keypair().pubkey())
MINT = MAINNET["solana"].tokens["USDC"].address
SIG = b58encode((7).to_bytes(64, "big"))


def make():
    cfg = build_config(DEV_EVM, solana_wallet=DEV)
    add_item(cfg, "chapter2", code=37, price="4.99", token="USDC", title="Chapter 2")
    sol, evm = FakeSolanaChain(slot=1000), FakeChain(head=1000)
    sol.accounts.add(associated_token_address(DEV, MINT))
    sol.accounts.add(associated_token_address(DEV, MAINNET["solana"].tokens["USDT"].address))
    factory = lambda chain: sol if chain.family == "solana" else evm  # noqa: E731
    return Engine(cfg, rpc_factory=factory, clock=FakeClock(100.0)), sol


def test_solana_payment_end_to_end():
    e, sol = make()
    rec = e.start_session("unlock", "solana", item="chapter2")
    assert rec["dest"] == associated_token_address(DEV, MINT) and rec["reference"]
    sol.pay(SIG, mint=MINT, payer=PAYER, owner=DEV, amount=rec["units"],
            addresses=[rec["reference"], rec["dest"]])
    assert [o.status for o in e.tick() if o.session_id == rec["id"]] == ["confirming"]
    sol.finalize(SIG)
    [paid] = [o for o in e.tick() if o.status == "paid"]
    assert paid.result.chain == "solana" and e.snapshot().owned == frozenset({"chapter2"})
    assert e.reverify() == [] and e.snapshot().unready == frozenset()
    e.receipts.clear()
    assert [c.item for c in e.restore_lookup(SIG)] == ["chapter2"]


def test_hint_on_solana_session_must_be_a_signature():
    e, _ = make()
    rec = e.start_session("tip", "solana", amount="1", token="USDC")
    e.report_hint(rec["id"], SIG)
    assert e.pending[rec["id"]]["hint_tx"] == SIG
    with pytest.raises(ValueError):
        e.report_hint(rec["id"], "0x" + "ab" * 32)


def test_missing_token_account_marks_token_unready():
    e, sol = make()
    sol.accounts.clear()
    e.reverify()
    assert e.snapshot().unready == frozenset({("solana", "USDC"), ("solana", "USDT")})


def test_readiness_check_failure_assumes_ready():
    e, sol = make()
    sol.accounts.clear()
    sol.down = True
    e.reverify()
    assert e.snapshot().unready == frozenset()


def _evm_session_in_solana_only_game():
    """An EVM session left in persistent by a build that accepted EVM, loaded by a build
    that is Solana-only: its chain is still in the registry but has no wallet."""
    old, _ = make()
    rec = old.start_session("tip", "arbitrum", amount="1", token="USDC")
    cfg = build_config(solana_wallet=DEV)
    add_item(cfg, "chapter2", code=37, price="4.99", token="USDC", title="Chapter 2")
    sol, evm = FakeSolanaChain(slot=1000), FakeChain(head=1000)
    factory = lambda chain: sol if chain.family == "solana" else evm  # noqa: E731
    errors = []
    clock = FakeClock(100.0)
    e = Engine(cfg, rpc_factory=factory, clock=clock, pending=old.pending,
               on_error=errors.append)
    return e, sol, rec, clock, errors


def test_session_without_a_wallet_for_its_chain_does_not_stall_the_others():
    e, sol, stale, _, errors = _evm_session_in_solana_only_game()
    rec = e.start_session("unlock", "solana", item="chapter2")
    sol.pay(SIG, mint=MINT, payer=PAYER, owner=DEV, amount=rec["units"],
            addresses=[rec["reference"], rec["dest"]])
    assert [o.status for o in e.tick() if o.session_id == rec["id"]] == ["confirming"]
    sol.finalize(SIG)
    outcomes = e.tick()
    assert [o.status for o in outcomes if o.session_id == rec["id"]] == ["paid"]
    assert [o for o in outcomes if o.session_id == stale["id"]] == []   # skipped silently
    assert stale["id"] in e.pending and errors == []


def test_session_on_an_unwatchable_chain_is_closed_after_the_horizon():
    from scfg_core.sessions import SCAN_HORIZON_S

    e, _, stale, clock, _ = _evm_session_in_solana_only_game()
    e.pending[stale["id"]]["chain"] = "removed_custom_chain"   # gone from the registry too
    other = dict(e.pending[stale["id"]], id="ps_other", chain="arbitrum")
    e.pending["ps_other"] = other
    clock.advance(SCAN_HORIZON_S)
    outcomes = [o for o in e.tick() if o.status == "closed"]
    assert sorted(o.session_id for o in outcomes) == sorted([stale["id"], "ps_other"])
    assert e.pending == {}


def test_an_exception_in_one_session_does_not_stop_the_others():
    errors = []
    e, sol = make()
    e.on_error = errors.append
    evm_rec = e.start_session("tip", "arbitrum", amount="1", token="USDC")
    rec = e.start_session("unlock", "solana", item="chapter2")
    boom = RuntimeError("boom")

    def broken():
        raise boom

    e.rpc("arbitrum").block_number = broken
    sol.pay(SIG, mint=MINT, payer=PAYER, owner=DEV, amount=rec["units"],
            addresses=[rec["reference"], rec["dest"]])
    outcomes = {o.session_id: o for o in e.tick()}
    assert (outcomes[evm_rec["id"]].status, outcomes[evm_rec["id"]].error) == (
        "waiting", "internal_error")
    assert outcomes[rec["id"]].status == "confirming"
    assert errors == [boom] and evm_rec["id"] in e.pending


def test_default_error_hook_prints_the_traceback(capsys):
    e, _ = make()
    rec = e.start_session("tip", "arbitrum", amount="1", token="USDC")

    def broken():
        raise RuntimeError("boom")

    e.rpc("arbitrum").block_number = broken
    assert [o.error for o in e.tick() if o.session_id == rec["id"]] == ["internal_error"]
    assert "RuntimeError: boom" in capsys.readouterr().err


def test_readiness_check_crash_assumes_ready_and_is_reported():
    errors = []
    e, sol = make()
    e.on_error = errors.append
    sol.accounts.clear()

    def broken(address):
        raise KeyError("value")

    sol.account_exists = broken
    e.reverify()
    assert e.snapshot().unready == frozenset()
    assert len(errors) == 1 and isinstance(errors[0], KeyError)
