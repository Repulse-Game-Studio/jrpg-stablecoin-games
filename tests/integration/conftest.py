"""Shared fixtures for integration tests: a local Anvil chain with mock USDC/USDT, and a
local Solana cluster (solana-test-validator) with a mock 6-decimal USDC."""
import shutil
import socket
import subprocess
import time
from dataclasses import replace

import pytest
from solders.keypair import Keypair

from cryptopay_core import chains as chains_module
from cryptopay_core.chains import Token
from cryptopay_core.coio import Sync
from cryptopay_core.config import add_item, build_config
from cryptopay_core.engine import Engine
from cryptopay_core.rpc import RpcClient, UrllibTransport

from mock_token import BYTECODE, MINT_SELECTOR, TRANSFER_SELECTOR
from solana_local import LocalSolana


def _free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


@pytest.fixture
def anvil():
    exe = shutil.which("anvil")
    if exe is None:
        pytest.skip("anvil not installed (https://getfoundry.sh)")
    port = _free_port()
    proc = subprocess.Popen([exe, "--port", str(port), "--silent"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    url = f"http://127.0.0.1:{port}"
    rpc = Sync(RpcClient([url]), UrllibTransport())
    deadline = time.time() + 10
    while True:
        try:
            rpc.call("eth_blockNumber", [])
            break
        except Exception:
            if time.time() > deadline:
                proc.kill()
                raise
            rpc = Sync(RpcClient([url]), UrllibTransport())   # reset endpoint cooldown
            time.sleep(0.1)
    yield url, rpc
    proc.terminate()
    proc.wait(5)


def word(value):
    return format(value, "064x")


def addr_word(address):
    return "0" * 24 + address[2:].lower()


def send(rpc, frm, to=None, data="0x"):
    tx = {"from": frm, "data": data, "gas": hex(3_000_000)}
    if to:
        tx["to"] = to
    tx_hash = rpc.call("eth_sendTransaction", [tx])
    deadline = time.time() + 5
    receipt = None
    while receipt is None:          # automine is fast, but the receipt can lag a moment
        receipt = rpc.call("eth_getTransactionReceipt", [tx_hash])
        if receipt is None:
            assert time.time() < deadline, "transaction was never mined"
            time.sleep(0.02)
    assert receipt["status"] == "0x1"
    return tx_hash, receipt


def mine(rpc, n):
    rpc.call("anvil_mine", [hex(n)])


@pytest.fixture
def world(anvil, monkeypatch):
    url, rpc = anvil
    accounts = rpc.call("eth_accounts", [])
    deployer, dev, payer = accounts[0], accounts[1], accounts[2]
    usdc = send(rpc, deployer, data=BYTECODE)[1]["contractAddress"]
    usdt = send(rpc, deployer, data=BYTECODE)[1]["contractAddress"]
    send(rpc, deployer, usdc, "0x" + MINT_SELECTOR + addr_word(payer) + word(100_000_000))
    # Point the "arbitrum" registry entry at the local chain and mock tokens.
    local = replace(chains_module.MAINNET["arbitrum"], rpc=(url,), confirmations=3,
                    tokens={"USDC": Token(usdc), "USDT": Token(usdt)})
    monkeypatch.setitem(chains_module.MAINNET, "arbitrum", local)
    cfg = build_config(dev, chains=["arbitrum"])
    add_item(cfg, "chapter2", code=37, price="4.99", token="USDC", title="Chapter 2")
    engine = Engine(cfg, transport=UrllibTransport())
    return {"rpc": rpc, "engine": engine, "dev": dev, "payer": payer, "usdc": usdc,
            "url": url, "config": cfg}


def pay(w, units):
    data = "0x" + TRANSFER_SELECTOR + addr_word(w["dev"]) + word(units)
    return send(w["rpc"], w["payer"], w["usdc"], data)[0]


def tick_until(engine, status, rounds=10, on_outcome=None):
    seen = []
    for _ in range(rounds):
        for o in engine.tick():
            seen.append(o.status)
            if on_outcome:
                on_outcome(o)
            if o.status == status:
                return o
    raise AssertionError(f"never reached {status}; saw {seen}")


@pytest.fixture(scope="session")
def solana_cluster():
    exe = shutil.which("solana-test-validator")
    if exe is None:
        pytest.skip("solana-test-validator not installed (https://github.com/anza-xyz/agave/releases)")
    cluster = LocalSolana(exe)
    yield cluster
    cluster.close()


@pytest.fixture
def solana_world(solana_cluster, monkeypatch):
    c = solana_cluster
    mint = c.create_mint(6)
    dev, player = Keypair(), Keypair()
    c.airdrop(player.pubkey(), 2 * 10**9)
    c.create_ata(str(dev.pubkey()), mint)
    c.create_ata(str(player.pubkey()), mint)
    c.mint_to(mint, str(player.pubkey()), 100_000_000)
    local = replace(chains_module.TESTNET["solana"], rpc=(c.url,), tokens={"USDC": Token(mint)})
    monkeypatch.setitem(chains_module.TESTNET, "solana", local)
    cfg = build_config(solana_wallet=str(dev.pubkey()), testnet=True, chains=["solana"])
    add_item(cfg, "chapter2", code=37, price="4.99", token="USDC", title="Chapter 2")
    engine = Engine(cfg, transport=UrllibTransport())
    return {"cluster": c, "engine": engine, "dev": dev, "player": player, "mint": mint,
            "config": cfg}


def tick_until_status(engine, status, timeout=90, on_outcome=None):
    seen = []
    deadline = time.time() + timeout
    while time.time() < deadline:
        for o in engine.tick():
            seen.append(o.status)
            if on_outcome:
                on_outcome(o)
            if o.status == status:
                return o
        time.sleep(0.5)
    raise AssertionError(f"never reached {status}; saw {seen[-20:]}")
