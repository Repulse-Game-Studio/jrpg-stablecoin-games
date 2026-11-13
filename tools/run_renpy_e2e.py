"""Run the Ren'Py end-to-end test: real screens, real worker thread, local chain.

--family evm (default) pays USDC on a local Anvil chain: needs `anvil` (Foundry) on PATH.
--family arc pays native USDC on Anvil dressed as Arc (USDC interface and system emitter
emulated at their real addresses), the way a wallet's "Send" button pays: needs `anvil`.
--family solana pays USDC on a local Solana validator: needs `solana-test-validator`
(Agave) on PATH and `solders` (a dev dependency, so run it with `uv run`).

Both need a Ren'Py SDK. Usage:

    uv run python tools/run_renpy_e2e.py --sdk /path/to/renpy-8.3.x-sdk [--family arc|solana]
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GAME = ROOT / "tests" / "renpy_e2e"
sys.path.insert(0, str(ROOT / "plugin" / "game" / "python-packages"))
sys.path.insert(0, str(ROOT / "tests" / "integration"))

from scfg_core.chains import ARC_SYSTEM_EMITTER, ARC_USDC  # noqa: E402
from scfg_core.coio import Sync  # noqa: E402
from scfg_core.rpc import RpcClient, UrllibTransport  # noqa: E402

from mock_token import (  # noqa: E402
    ARC_EMITTER_RUNTIME,
    ARC_USDC_RUNTIME,
    BYTECODE,
    MINT_SELECTOR,
)


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait_receipt(rpc, tx):
    for _ in range(250):
        r = rpc.call("eth_getTransactionReceipt", [tx])
        if r:
            return r
        time.sleep(0.02)
    raise RuntimeError("transaction not mined")


def start_anvil(anvil: str):
    """Start Anvil; returns (url, rpc, accounts, cleanup)."""
    port = free_port()
    url = f"http://127.0.0.1:{port}"
    proc = subprocess.Popen([anvil, "--port", str(port), "--silent"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def cleanup():
        proc.terminate()
        proc.wait(5)

    try:
        rpc = Sync(RpcClient([url]), UrllibTransport())
        for _ in range(100):
            try:
                rpc = Sync(RpcClient([url]), UrllibTransport())
                rpc.block_number()
                break
            except Exception:
                time.sleep(0.1)
        accounts = rpc.call("eth_accounts", [])[:3]
    except BaseException:
        cleanup()
        raise
    return url, rpc, accounts, cleanup


def mint(rpc, deployer, token, to):
    wait_receipt(rpc, rpc.call("eth_sendTransaction", [{
        "from": deployer, "to": token,
        "data": "0x" + MINT_SELECTOR + "0" * 24 + to[2:].lower() + format(10**9, "064x")}]))


def setup_evm(anvil: str, workdir: Path):
    """Start Anvil, deploy a mock USDC and fund the payer. Returns (env_updates, cleanup)."""
    url, rpc, (deployer, dev, payer), cleanup = start_anvil(anvil)
    try:
        token = wait_receipt(rpc, rpc.call("eth_sendTransaction", [
            {"from": deployer, "data": BYTECODE, "gas": hex(3_000_000)}]))["contractAddress"]
        mint(rpc, deployer, token, payer)
    except BaseException:
        cleanup()
        raise
    return {"SCFG_E2E_FAMILY": "evm", "SCFG_E2E_RPC": url, "SCFG_E2E_USDC": token,
            "SCFG_E2E_DEV": dev, "SCFG_E2E_PAYER": payer}, cleanup


def setup_arc(anvil: str, workdir: Path):
    """Start Anvil dressed as Arc: the USDC interface and the system emitter at their real
    addresses. Returns (env_updates, cleanup)."""
    url, rpc, (deployer, dev, payer), cleanup = start_anvil(anvil)
    try:
        rpc.call("anvil_setCode", [ARC_SYSTEM_EMITTER, ARC_EMITTER_RUNTIME])
        rpc.call("anvil_setCode", [ARC_USDC, ARC_USDC_RUNTIME])
        mint(rpc, deployer, ARC_USDC, payer)
    except BaseException:
        cleanup()
        raise
    return {"SCFG_E2E_FAMILY": "arc", "SCFG_E2E_RPC": url, "SCFG_E2E_USDC": ARC_USDC,
            "SCFG_E2E_DEV": dev, "SCFG_E2E_PAYER": payer}, cleanup


def solana_world(validator: str):
    """Start a local Solana validator with a mock 6-decimal USDC, a developer and a player
    holding 1000 of it. Returns (cluster, mint, dev, player); close the cluster when done."""
    from solders.keypair import Keypair

    from solana_local import LocalSolana  # tests/integration is on sys.path (top of file)

    cluster = LocalSolana(validator)
    try:
        mint = cluster.create_mint()
        dev, player = Keypair(), Keypair()
        for owner in (dev, player):
            cluster.create_ata(str(owner.pubkey()), mint)
        cluster.mint_to(mint, str(player.pubkey()), 1_000 * 10**6)
    except BaseException:
        cluster.close()
        raise
    return cluster, mint, dev, player


def setup_solana(validator: str, workdir: Path):
    """Start a local Solana validator with a mock USDC and a funded player, plus a thread
    that pays the game's payment requests (Ren'Py has no solders, so the game cannot sign).
    Returns (env_updates, cleanup)."""
    cluster, mint, dev, player = solana_world(validator)
    stop = threading.Event()
    payreq = workdir / "payreq.json"
    thread = None

    def cleanup():
        stop.set()
        if thread is not None:
            thread.join(10)
        cluster.close()

    try:
        def pay_requests():
            # The game writes payreq.json; pay it from the player and write the signature back.
            while not stop.wait(0.1):
                if not payreq.exists():
                    continue
                try:
                    req = json.loads(payreq.read_text())
                    sig = cluster.transfer(player, mint, str(dev.pubkey()), int(req["units"]),
                                           reference=req["reference"])
                except Exception as e:
                    print(f"Solana payment request failed: {e!r}", flush=True)
                    payreq.unlink(missing_ok=True)
                    continue
                tmp = payreq.with_name(payreq.name + ".sig.tmp")
                tmp.write_text(sig)
                tmp.replace(payreq.with_name(payreq.name + ".sig"))
                payreq.unlink(missing_ok=True)

        thread = threading.Thread(target=pay_requests, name="e2e-solana-payer", daemon=True)
        thread.start()
    except BaseException:
        cleanup()
        raise
    return {"SCFG_E2E_FAMILY": "solana", "SCFG_E2E_RPC": cluster.url,
            "SCFG_E2E_USDC": mint, "SCFG_E2E_DEV": str(dev.pubkey()),
            "SCFG_E2E_PAYREQ": str(payreq)}, cleanup


FAMILIES = {
    "evm": ("anvil", setup_evm, "anvil not found on PATH (install Foundry: https://getfoundry.sh)"),
    "arc": ("anvil", setup_arc, "anvil not found on PATH (install Foundry: https://getfoundry.sh)"),
    "solana": ("solana-test-validator", setup_solana,
               "solana-test-validator not found on PATH (install an Agave release: https://github.com/anza-xyz/agave/releases)"),
}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--sdk", required=True, help="Ren'Py SDK directory")
    ap.add_argument("--family", choices=sorted(FAMILIES), default="evm",
                    help="chain family to pay on (default: evm)")
    ap.add_argument("--timeout", type=int, default=180, help="seconds before giving up")
    args = ap.parse_args()

    tool, setup, missing = FAMILIES[args.family]
    exe = shutil.which(tool)
    if not exe:
        print(missing)
        return 2

    for name in ("scfg", "python-packages"):
        target = GAME / "game" / name
        if target.exists():
            shutil.rmtree(target)
        shutil.copytree(ROOT / "plugin" / "game" / name, target,
                        ignore=shutil.ignore_patterns("__pycache__", "*.rpyc"))
    # Ren'Py 8.4 replaced the test language; pick the matching testcase dialect.
    modern = (Path(args.sdk) / "renpy" / "test" / "testsettings.py").exists()
    dialect = "modern" if modern else "legacy"
    shutil.copy(GAME / "testcases" / f"{dialect}.rpy", GAME / "game" / "e2e_testcase.rpy")
    print(f"Ren'Py test dialect: {dialect}, chain family: {args.family}")

    savedir = tempfile.mkdtemp(prefix="scfg-e2e-saves-")
    out = Path(tempfile.mkdtemp(prefix="scfg-e2e-")) / "result.json"
    cleanup = None
    try:
        env_updates, cleanup = setup(exe, out.parent)
        env = dict(os.environ)
        env.update(env_updates)
        env.update({
            "SCFG_E2E_OUT": str(out),
            "SDL_AUDIODRIVER": "dummy",
            # Render offscreen: never open a window on the developer's desktop.
            "SDL_VIDEODRIVER": os.environ.get("SCFG_E2E_VIDEODRIVER", "offscreen"),
        })
        renpy_sh = Path(args.sdk) / "renpy.sh"
        shutil.rmtree(GAME / "game" / "saves", ignore_errors=True)  # fresh persistent data
        debug = out.with_name("debug.txt")
        env["SCFG_E2E_DEBUG"] = str(debug)
        try:
            result = subprocess.run([str(renpy_sh), "--savedir", savedir, str(GAME), "test"],
                                    env=env, timeout=args.timeout, capture_output=True, text=True)
        except subprocess.TimeoutExpired as e:
            print("Ren'Py test timed out. stdout tail:")
            tail = e.stdout or b""
            if isinstance(tail, bytes):
                tail = tail.decode("utf-8", "replace")
            print(tail[-3000:])
            if debug.exists():
                print("--- plugin state (last lines) ---")
                print("\n".join(debug.read_text().splitlines()[-8:]))
            return 1
        if result.returncode != 0 or not out.exists():
            print(result.stdout[-4000:])
            print(result.stderr[-4000:])
            for log in ("traceback.txt", "errors.txt", "log.txt"):
                p = GAME / log
                if p.exists():
                    print(f"--- {log} ---")
                    print(p.read_text()[-4000:])
            print("Ren'Py e2e FAILED")
            return 1
        report = json.loads(out.read_text())
        print("Ren'Py e2e report:", report)
        ok = (report["status"] == "paid" and report["owned"] and report["receipts"] == 1
              and report["purchases_listed"] == 1 and report["restore_state"] == "done"
              and report.get("cleared", True))
        print("Ren'Py e2e PASSED" if ok else "Ren'Py e2e FAILED")
        return 0 if ok else 1
    finally:
        if cleanup is not None:
            cleanup()
        shutil.rmtree(savedir, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
