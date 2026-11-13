"""Simulated JSON-RPC nodes: routing, EVM and Solana answers, and per-URL faults."""

from __future__ import annotations

import json
import re
from pathlib import Path

import pytest

from scfg_conformance import simnode
from scfg_conformance.scenario import RecordingTransport
from scfg_conformance.scenarios.fixtures import (
    DEV_EVM,
    DEV_SOL,
    PAYER_EVM,
    PAYER_SOL,
    signature,
    tx_hash,
)
from scfg_core.chains import TESTNET
from scfg_core.coio import Sync, TransportError
from scfg_core.evm import TRANSFER_TOPIC, address_topic
from scfg_core.rpc import RpcClient, RpcError, RpcUnavailable
from scfg_core.solana.pda import associated_token_address
from scfg_core.solana.rpc import SolanaRpc

ROOT = Path(__file__).resolve().parents[2]
A, B = "https://a.example", "https://b.example"
USDC = TESTNET["base"].tokens["USDC"].address
OTHER_TOKEN = TESTNET["ethereum"].tokens["USDC"].address
MINT = TESTNET["solana"].tokens["USDC"].address


def world_with(node):
    world = simnode.World()
    world.add(node)
    return world


def ask(world, method, params, url=A):
    """One raw JSON-RPC exchange, parsed."""
    text = world.handle(url, {"jsonrpc": "2.0", "id": 7, "method": method, "params": params})
    return json.loads(text)


def result(world, method, params, url=A):
    body = ask(world, method, params, url)
    assert "error" not in body, body
    return body["result"]


def get_logs(world, lo, hi, address=USDC, topics=None, url=A):
    return result(world, "eth_getLogs", [{
        "address": address, "topics": topics or [TRANSFER_TOPIC],
        "fromBlock": hex(lo), "toBlock": hex(hi)}], url)


# --- EVM -------------------------------------------------------------------------------------


def test_simnode_get_logs_respects_range_and_address():
    node = simnode.EvmNode([A])
    node.head = 100
    world = world_with(node)
    a = node.add_transfer(USDC, PAYER_EVM, DEV_EVM, 5, block=10, tx_hash=tx_hash(1))
    b = node.add_transfer(USDC, PAYER_EVM, DEV_EVM, 6, block=20, tx_hash=tx_hash(2))
    node.add_transfer(OTHER_TOKEN, PAYER_EVM, DEV_EVM, 7, block=15, tx_hash=tx_hash(3))
    future = node.add_transfer(USDC, PAYER_EVM, DEV_EVM, 8, block=101, tx_hash=tx_hash(4))
    assert get_logs(world, 10, 19) == [a]
    assert get_logs(world, 11, 19) == []
    assert get_logs(world, 0, 200) == [a, b]                 # block 101 is not mined yet
    assert get_logs(world, 0, 200, address=USDC.lower()) == [a, b]
    assert get_logs(world, 0, 200, address=OTHER_TOKEN)[0]["data"] == "0x" + format(7, "064x")
    to_dev = [TRANSFER_TOPIC, None, address_topic(DEV_EVM)]
    assert get_logs(world, 0, 200, topics=to_dev) == [a, b]
    assert get_logs(world, 0, 200, topics=[TRANSFER_TOPIC, None, address_topic(PAYER_EVM)]) == []
    either = [TRANSFER_TOPIC, None, [address_topic(PAYER_EVM), address_topic(DEV_EVM)]]
    assert get_logs(world, 0, 200, topics=either) == [a, b]
    node.mine(1)
    assert get_logs(world, 0, 200) == [a, b, future]
    assert ask(world, "eth_blockNumber", [])["id"] == 7      # the request id is echoed


def test_simnode_evm_blocks_receipts_and_reorg():
    node = simnode.EvmNode([A])
    node.head = 100
    world = world_with(node)
    log = node.add_transfer(USDC, PAYER_EVM, DEV_EVM, 5, block=90, tx_hash=tx_hash(1))
    assert result(world, "eth_blockNumber", []) == hex(100)
    block = result(world, "eth_getBlockByNumber", [hex(90), False])
    assert block["hash"] == log["blockHash"] == node.block_hash(90)
    assert result(world, "eth_getBlockByNumber", [hex(101), False]) is None
    receipt = result(world, "eth_getTransactionReceipt", [tx_hash(1)])
    assert receipt["status"] == "0x1" and receipt["logs"] == [log]
    assert result(world, "eth_getTransactionReceipt", [tx_hash(9)]) is None
    node.receipts[tx_hash(1)]["status"] = "0x0"
    assert result(world, "eth_getTransactionReceipt", [tx_hash(1)])["status"] == "0x0"

    old = {n: node.block_hash(n) for n in (89, 90, 100)}
    node.reorg(90)
    assert node.block_hash(89) == old[89]
    assert node.block_hash(90) != old[90] and node.block_hash(100) != old[100]
    assert get_logs(world, 90, 90)[0]["blockHash"] == node.block_hash(90)
    assert result(world, "eth_getBlockByNumber", [hex(90), False])["hash"] == node.block_hash(90)
    assert ask(world, "eth_chainId", [])["error"]["code"] == -32601


def test_simnode_faults():
    node = simnode.EvmNode([A, B])
    world = world_with(node)
    ep = node.endpoint(A)

    ep.down = True
    with pytest.raises(TransportError) as e:
        ask(world, "eth_blockNumber", [])
    assert e.value.status is None
    assert result(world, "eth_blockNumber", [], url=B) == hex(node.head)   # B is unaffected
    ep.down = False

    ep.http_status = 429
    with pytest.raises(TransportError) as e:
        ask(world, "eth_blockNumber", [])
    assert e.value.status == 429 and str(e.value) == "HTTP 429"
    ep.http_status = None

    ep.invalid_json = True
    text = world.handle(A, {"jsonrpc": "2.0", "id": 1, "method": "eth_blockNumber", "params": []})
    with pytest.raises(ValueError):
        json.loads(text)
    transport = RecordingTransport(world)
    with pytest.raises(TransportError) as e:
        transport.post_json(A, {"jsonrpc": "2.0", "id": 1, "method": "eth_blockNumber",
                                "params": []}, 10)
    assert (str(e.value), e.value.status) == ("invalid JSON response", None)
    assert transport.calls == [{"url": A, "method": "eth_blockNumber", "params": [],
                                "transport_error": {"message": "invalid JSON response",
                                                    "status": None}}]
    ep.invalid_json = False

    ep.rate_limited = True
    assert "rate limit" in ask(world, "eth_blockNumber", [])["error"]["message"]
    client = Sync(RpcClient([A, B], clock=lambda: 0), RecordingTransport(world))
    assert client.block_number() == node.head                # A cools down, B answers
    ep.rate_limited = False

    ep.max_log_range = 50
    assert get_logs(world, 1, 50) == []
    assert ask(world, "eth_getLogs", [{"address": USDC, "topics": [TRANSFER_TOPIC],
                                       "fromBlock": hex(1), "toBlock": hex(51)}])["error"] == {
        "code": -32005, "message": "query exceeds max block range 50"}
    ep.max_log_range = None

    ep.errors["eth_blockNumber"] = (-32000, "header not found")
    for _ in range(2):  # persistent until cleared
        assert ask(world, "eth_blockNumber", [])["error"] == {"code": -32000,
                                                              "message": "header not found"}
    assert result(world, "eth_getBlockByNumber", [hex(1), False])["number"] == hex(1)
    del ep.errors["eth_blockNumber"]
    assert result(world, "eth_blockNumber", []) == hex(node.head)

    with pytest.raises(TransportError) as e:
        world.handle("https://nobody.example", {"jsonrpc": "2.0", "id": 1,
                                                "method": "eth_blockNumber", "params": []})
    assert e.value.status is None


def test_endpoint_fail_from_answers_normally_until_the_nth_call():
    node = simnode.EvmNode([A])
    world, ep = world_with(node), node.endpoint(A)
    ep.fail_from("eth_blockNumber", 3, -32005, "rate limit exceeded")
    assert [("error" in ask(world, "eth_blockNumber", [])) for _ in range(5)] == [
        False, False, True, True, True]
    assert result(world, "eth_getBlockByNumber", [hex(1), False])["number"] == hex(1)
    del ep.errors_from["eth_blockNumber"]
    assert "error" not in ask(world, "eth_blockNumber", [])
    with pytest.raises(ValueError):
        ep.fail_from("eth_blockNumber", 0, -1, "x")


def test_endpoint_raw_answers_and_custom_range_error():
    node = simnode.EvmNode([A])
    world, ep = world_with(node), node.endpoint(A)
    ep.error_objects["eth_blockNumber"] = "query timeout"
    assert ask(world, "eth_blockNumber", [])["error"] == "query timeout"
    ep.error_objects["eth_blockNumber"] = {"code": "bad", "message": "odd"}
    assert ask(world, "eth_blockNumber", [])["error"] == {"code": "bad", "message": "odd"}
    ep.raw_bodies["eth_blockNumber"] = "not a dict"
    assert world.handle(A, {"method": "eth_blockNumber", "params": []}) == '"not a dict"'
    ep.raw_bodies["eth_blockNumber"] = {"result": None}
    assert ask(world, "eth_blockNumber", []) == {"result": None}
    flt = {"address": USDC, "topics": [], "fromBlock": hex(0), "toBlock": hex(99)}
    ep.max_log_range = 50
    assert ask(world, "eth_getLogs", [flt])["error"]["message"] == (
        "query exceeds max block range 50")
    ep.max_log_range_error = (-32600, "block range too large")
    assert ask(world, "eth_getLogs", [flt])["error"] == {
        "code": -32600, "message": "block range too large"}


def test_world_rejects_a_url_served_twice_and_unknown_endpoints():
    world = world_with(simnode.EvmNode([A]))
    with pytest.raises(ValueError):
        world.add(simnode.EvmNode([A]))
    with pytest.raises(KeyError):
        simnode.EvmNode([A]).endpoint(B)


def test_response_with_unsafe_int_is_refused():
    node = simnode.SolanaNode([A])
    node.balances[PAYER_SOL] = 2**53
    with pytest.raises(ValueError):
        ask(world_with(node), "getBalance", [PAYER_SOL, {"commitment": "confirmed"}])


# --- Solana ----------------------------------------------------------------------------------


def solana(node, *urls):
    world = world_with(node)
    rpc = SolanaRpc(RpcClient(list(urls) or node.urls, clock=lambda: 0))
    return Sync(rpc, RecordingTransport(world)), rpc


def test_simnode_solana_answers_every_rpc_method():
    source = (ROOT / "plugin/game/python-packages/scfg_core/solana/rpc.py").read_text("utf-8")
    used = set(re.findall(r'"(get[A-Z]\w+)"', source))
    assert used and used <= set(simnode.SolanaNode.METHODS)

    node = simnode.SolanaNode([A])
    dest = associated_token_address(DEV_SOL, MINT)
    ref = associated_token_address(PAYER_SOL, MINT)
    sig1, sig2 = signature(1), signature(2)
    node.pay(sig1, mint=MINT, payer=PAYER_SOL, owner=DEV_SOL, amount=5, addresses=[dest])
    node.slot += 10
    node.pay(sig2, mint=MINT, payer=PAYER_SOL, owner=DEV_SOL, amount=6, addresses=[dest, ref])
    node.accounts.add(dest)
    node.token_balances[ref] = 7_000_000
    node.balances[PAYER_SOL] = 2_000_000
    node.priority_fees = [100, 5_000, 300]
    rpc, _ = solana(node)

    assert rpc.get_slot() == node.slot
    assert [s["signature"] for s in rpc.get_signatures_for_address(dest)] == [sig2, sig1]
    assert [s["signature"] for s in rpc.get_signatures_for_address(dest, limit=1)] == [sig2]
    assert rpc.get_signatures_for_address(dest, until=sig1) == [
        {"signature": sig2, "slot": node.slot, "err": None}]
    assert [s["signature"] for s in rpc.get_signatures_for_address(dest, before=sig2)] == [sig1]
    assert rpc.get_signatures_for_address("Nobody1111111111111111111111111111111111111") == []
    tx = rpc.get_transaction(sig1)
    assert tx["slot"] == node.slot - 10 and tx["transaction"]["signatures"] == [sig1]
    assert rpc.get_transaction(signature(9)) is None
    assert rpc.get_transaction_any(sig1, min_slot=0) == (tx, False)
    assert rpc.get_transaction_any(signature(9), min_slot=0) == (None, True)
    node.first_available = 5
    rpc, _ = solana(node)
    assert rpc.get_transaction_any(signature(9), min_slot=0) == (None, False)
    assert rpc.get_signature_statuses([sig1, signature(9)]) == [
        {"slot": node.slot - 10, "err": None, "confirmationStatus": "confirmed",
         "confirmations": 1}, None]
    node.finalize(sig1)
    assert rpc.get_signature_statuses([sig1])[0]["confirmationStatus"] == "finalized"
    assert rpc.account_exists(dest) is True and rpc.account_exists(PAYER_SOL) is False
    assert rpc.account_exists(ref) is True                     # holds a token balance
    assert rpc.get_token_balance(ref) == 7_000_000 and rpc.get_token_balance(PAYER_SOL) == 0
    assert rpc.get_balance(PAYER_SOL) == 2_000_000 and rpc.get_balance(DEV_SOL) == 0
    assert rpc.get_latest_blockhash() == node.blockhash
    assert rpc.get_priority_fee([dest]) == 300
    node.priority_fees = []
    assert rpc.get_priority_fee([dest]) == 0


def test_simnode_solana_history_and_tx_version_faults():
    node = simnode.SolanaNode([A, B])
    sig = signature(1)
    node.pay(sig, mint=MINT, payer=PAYER_SOL, owner=DEV_SOL, amount=5, slot=400)

    node.endpoint(A).first_available = 500     # A pruned slot 400; B keeps everything
    rpc, _ = solana(node, A)
    assert rpc.get_transaction(sig) is None
    assert rpc.get_transaction_any(sig, min_slot=400) == (None, False)
    rpc, _ = solana(node, A, B)
    assert rpc.get_transaction_any(sig, min_slot=400)[0]["slot"] == 400
    node.endpoint(A).first_available = None

    node.endpoint(A).tx_version_max = 0         # an older node: rejects version 1 (-32602)
    rpc, raw = solana(node, A)
    assert rpc.get_transaction(sig)["slot"] == 400 and raw.tx_version == 0
    node.endpoint(A).tx_version_max = None

    node.txs[sig]["version"] = 2                # a newer transaction: -32015 names its version
    rpc, raw = solana(node, A)
    assert rpc.get_transaction(sig)["version"] == 2 and raw.tx_version == 1
    node.endpoint(A).tx_version_max = 1         # the node itself cannot serve version 2
    rpc, _ = solana(node, A)
    with pytest.raises(RpcError):
        rpc.get_transaction(sig)

    node.endpoint(A).down = True
    rpc, _ = solana(node, A)
    with pytest.raises(RpcUnavailable):
        rpc.get_slot()
