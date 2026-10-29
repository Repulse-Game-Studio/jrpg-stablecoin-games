import base64
import http.client
import json
from concurrent.futures import Future
from concurrent.futures import TimeoutError as FutureTimeout

import pytest
from solders.keypair import Keypair
from solders.transaction import Transaction

from scfg_core.backends.solana import MAX_PRIORITY_FEE, MIN_PRIORITY_FEE, SolanaBackend
from scfg_core.chains import MAINNET
from scfg_core.checkout import CheckoutController
from scfg_core.config import add_item, build_config
from scfg_core.engine import Engine
from scfg_core.local_server import CheckoutServer
from scfg_core.payments import prepare_payment
from scfg_core.solana.pda import associated_token_address

from fakes import FakeChain, FakeClock, FakeSolanaChain, coop, drive, immediate_submitter

DEV = str(Keypair().pubkey())
PLAYER = Keypair()
MINT = MAINNET["solana"].tokens["USDC"].address
BLOCKHASH = "4sGjMW1sUnHzSxGspuhpqLDx6wiyjNtZAMdL4VZHirAn"


@coop("get_token_balance", "get_balance", "get_latest_blockhash", "get_priority_fee")
class BuildRpc(FakeSolanaChain):
    fee = 5_000

    def get_token_balance(self, account):
        return 7_000_000 if account == associated_token_address(str(PLAYER.pubkey()), MINT) else 0

    def get_balance(self, address):
        return 2_000_000

    def get_latest_blockhash(self):
        return BLOCKHASH

    def get_priority_fee(self, accounts):
        return self.fee


def session_rec():
    return {"token": "USDC", "units": 4_993_742, "reference": str(Keypair().pubkey()),
            "dest": associated_token_address(DEV, MINT)}


@pytest.mark.parametrize("fee, expected", [(5_000, 5_000), (0, MIN_PRIORITY_FEE),
                                           (10**12, MAX_PRIORITY_FEE)])
def test_build_payment_transaction(fee, expected):
    rpc = BuildRpc()
    rpc.fee = fee
    rec = session_rec()
    out = drive(SolanaBackend().build_payment(rec, rpc=rpc, chain=MAINNET["solana"],
                                              account=str(PLAYER.pubkey())))
    assert (out["token_balance"], out["sol_balance"]) == ("7000000", "2000000")
    tx = Transaction.from_bytes(base64.b64decode(out["transaction"]))
    keys = [str(k) for k in tx.message.account_keys]
    assert keys[0] == str(PLAYER.pubkey()) and rec["reference"] in keys and rec["dest"] in keys
    assert str(tx.message.recent_blockhash) == BLOCKHASH
    data = [bytes(ix.data) for ix in tx.message.instructions]
    assert data[1] == bytes([3]) + expected.to_bytes(8, "little")
    assert data[2] == bytes([12]) + (4_993_742).to_bytes(8, "little") + bytes([6])
    tx.sign([PLAYER], tx.message.recent_blockhash)        # the wallet can sign it as is
    assert tx.verify() is None


def test_build_payment_rejects_a_bad_account():
    with pytest.raises(ValueError):
        drive(SolanaBackend().build_payment(session_rec(), rpc=BuildRpc(), chain=MAINNET["solana"],
                                            account="0x1234"))


def _engine():
    cfg = build_config("0x52908400098527886E0F7030069857D2E4169EE7", solana_wallet=DEV)
    add_item(cfg, "chapter2", code=37, price="4.99", token="USDC", title="Chapter 2")
    sol = BuildRpc()
    return cfg, Engine(cfg, rpc_factory=lambda c: sol if c.family == "solana" else FakeChain(),
                       clock=FakeClock(0.0))


def test_engine_builds_only_for_solana_sessions():
    _, e = _engine()
    sol = e.start_session("unlock", "solana", item="chapter2")
    assert "transaction" in e.build_solana_payment(sol["id"], str(PLAYER.pubkey()))
    evm = e.start_session("unlock", "base", item="chapter2")
    with pytest.raises(ValueError):
        e.build_solana_payment(evm["id"], str(PLAYER.pubkey()))


class KwServer:
    def __init__(self, **kw):
        self.kw = kw
        self.url = "http://127.0.0.1:1/c/s/"

    def start(self):
        return self.url

    def stop(self):
        pass


def test_controller_passes_family_info_and_a_build_callback():
    cfg, e = _engine()
    made = []
    ctl = CheckoutController(config=cfg, spec=prepare_payment(cfg, "unlock", item="chapter2"),
                             submit=immediate_submitter(e),
                             server_factory=lambda **kw: made.append(KwServer(**kw)) or made[-1])
    ctl.select_chain("solana")
    ctl.poll()
    ctl.start_browser()
    info, build = made[0].kw["session_info"], made[0].kw["build_tx"]
    assert (info["family"], info["wallet_chain"], info["mint"]) == ("solana", "solana:mainnet", MINT)
    assert info["reference"] == ctl.session["reference"] and "chain_id" not in info
    assert "transaction" in build(str(PLAYER.pubkey())).result(timeout=1)
    ctl.close()
    ctl.select_chain("base")
    ctl.poll()
    ctl.start_browser()
    assert made[1].kw["session_info"]["family"] == "evm" and "build_tx" not in made[1].kw


def _post(srv, path, body):
    conn = http.client.HTTPConnection("127.0.0.1", srv.port)
    conn.request("POST", f"/c/{srv.secret}/{path}", json.dumps(body),
                 {"Host": f"127.0.0.1:{srv.port}", "Content-Type": "application/json"})
    r = conn.getresponse()
    return r.status, r.read()


def _done(value=None, error=None):
    fut = Future()
    if error is not None:
        fut.set_exception(error)
    else:
        fut.set_result(value)
    return fut


@pytest.mark.parametrize("build_tx, body, status", [
    (None, {"account": str(PLAYER.pubkey())}, 404),
    (lambda a: _done({"transaction": "AA=="}), {"account": "0xnope"}, 400),
    (lambda a: _done(error=FutureTimeout()), {"account": str(PLAYER.pubkey())}, 503),
    (lambda a: (_ for _ in ()).throw(RuntimeError("submit failed")),
     {"account": str(PLAYER.pubkey())}, 503),
    (lambda a: _done({"transaction": "AA==", "token_balance": "1", "sol_balance": "2"}),
     {"account": str(PLAYER.pubkey())}, 200),
])
def test_build_route(build_tx, body, status):
    srv = CheckoutServer(load_asset=lambda n: b"", session_info={}, get_status=lambda: {},
                         on_tx=lambda tx, s: None, build_tx=build_tx)
    srv.start()
    try:
        got, raw = _post(srv, "build", body)
    finally:
        srv.stop()
    assert got == status
    if status == 200:
        assert json.loads(raw)["transaction"] == "AA=="
