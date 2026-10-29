"""A throwaway local Solana cluster (Agave's solana-test-validator) for integration tests,
with helpers to create a 6-decimal mock USDC, token accounts, and transfers.

Transactions are signed with solders (test-only dependency). The plugin itself never
signs anything: it only reads the chain and builds unsigned transactions.
"""
from __future__ import annotations

import base64
import shutil
import socket
import subprocess
import tempfile
import time

from solders.hash import Hash
from solders.instruction import AccountMeta, Instruction
from solders.keypair import Keypair
from solders.pubkey import Pubkey
from solders.system_program import ID as SYSTEM_PROGRAM
from solders.system_program import CreateAccountParams, create_account
from solders.transaction import Transaction

from scfg_core.coio import Sync
from scfg_core.rpc import RpcClient, UrllibTransport
from scfg_core.solana.pda import (
    ASSOCIATED_TOKEN_PROGRAM_ID,
    TOKEN_PROGRAM_ID,
    associated_token_address,
)
from scfg_core.solana.tx import transfer_checked

TOKEN = Pubkey.from_string(TOKEN_PROGRAM_ID)
ATA = Pubkey.from_string(ASSOCIATED_TOKEN_PROGRAM_ID)
MINT_SIZE = 82


def _free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def to_solders(ix):
    return Instruction(Pubkey.from_string(ix.program_id), ix.data,
                       [AccountMeta(Pubkey.from_string(a.pubkey), a.is_signer, a.is_writable)
                        for a in ix.accounts])


class LocalSolana:
    def __init__(self, exe: str):
        self.ledger = tempfile.mkdtemp(prefix="scfg-solana-")
        port = _free_port()
        self.proc = subprocess.Popen(
            [exe, "--ledger", self.ledger, "--reset", "--quiet", "--rpc-port", str(port),
             "--faucet-port", str(_free_port()), "--gossip-port", str(_free_port()),
             "--dynamic-port-range", f"{port + 2}-{port + 40}"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.url = f"http://127.0.0.1:{port}"
        self.client = Sync(RpcClient([self.url], timeout=10.0), UrllibTransport())
        deadline = time.time() + 90
        while True:
            try:
                if self.client.call("getHealth", []) == "ok":
                    break
            except Exception:
                pass
            if time.time() > deadline or self.proc.poll() is not None:
                self.close()
                raise RuntimeError("solana-test-validator did not start")
            time.sleep(0.5)
        self.payer = Keypair()
        self.airdrop(self.payer.pubkey(), 50 * 10**9)

    def call(self, method, params):
        return self.client.call(method, params)

    def wait(self, sig: str, level: str = "confirmed", timeout: float = 60) -> None:
        wanted = ("confirmed", "finalized") if level == "confirmed" else ("finalized",)
        deadline = time.time() + timeout
        while time.time() < deadline:
            status = self.call("getSignatureStatuses", [[sig]])["value"][0]
            if status and status.get("err"):
                raise RuntimeError(f"transaction failed: {status['err']}")
            if status and status.get("confirmationStatus") in wanted:
                return
            time.sleep(0.3)
        raise TimeoutError(f"{sig} not {level} after {timeout}s")

    def airdrop(self, pubkey, lamports: int) -> None:
        self.wait(self.call("requestAirdrop", [str(pubkey), lamports]))

    def _send_bytes(self, raw: bytes) -> str:
        sig = self.call("sendTransaction", [base64.b64encode(raw).decode(),
                                            {"encoding": "base64",
                                             "preflightCommitment": "confirmed"}])
        self.wait(sig)
        return sig

    def send(self, ixs, signers=()) -> str:
        blockhash = Hash.from_string(
            self.call("getLatestBlockhash", [{"commitment": "confirmed"}])["value"]["blockhash"])
        tx = Transaction.new_signed_with_payer(ixs, self.payer.pubkey(),
                                               [self.payer, *signers], blockhash)
        return self._send_bytes(bytes(tx))

    def create_mint(self, decimals: int = 6) -> str:
        mint = Keypair()
        rent = self.call("getMinimumBalanceForRentExemption", [MINT_SIZE])
        init = Instruction(TOKEN, bytes([20, decimals]) + bytes(self.payer.pubkey()) + bytes([0]),
                           [AccountMeta(mint.pubkey(), False, True)])  # InitializeMint2
        self.send([create_account(CreateAccountParams(
            from_pubkey=self.payer.pubkey(), to_pubkey=mint.pubkey(), lamports=rent,
            space=MINT_SIZE, owner=TOKEN)), init], [mint])
        return str(mint.pubkey())

    def create_ata(self, owner: str, mint: str) -> str:
        ata = associated_token_address(owner, mint)
        ix = Instruction(ATA, bytes([1]), [  # CreateIdempotent
            AccountMeta(self.payer.pubkey(), True, True),
            AccountMeta(Pubkey.from_string(ata), False, True),
            AccountMeta(Pubkey.from_string(owner), False, False),
            AccountMeta(Pubkey.from_string(mint), False, False),
            AccountMeta(SYSTEM_PROGRAM, False, False),
            AccountMeta(TOKEN, False, False)])
        self.send([ix])
        return ata

    def mint_to(self, mint: str, owner: str, amount: int) -> None:
        ix = Instruction(TOKEN, bytes([7]) + amount.to_bytes(8, "little"), [  # MintTo
            AccountMeta(Pubkey.from_string(mint), False, True),
            AccountMeta(Pubkey.from_string(associated_token_address(owner, mint)), False, True),
            AccountMeta(self.payer.pubkey(), True, False)])
        self.send([ix])

    def transfer(self, sender: Keypair, mint: str, to_owner: str, amount: int,
                 reference: str | None = None) -> str:
        owner = str(sender.pubkey())
        ix = transfer_checked(source=associated_token_address(owner, mint), mint=mint,
                              destination=associated_token_address(to_owner, mint), owner=owner,
                              amount=amount, decimals=6, extra=(reference,) if reference else ())
        return self.send([to_solders(ix)], [sender])

    def sign_and_send(self, unsigned: bytes, signer: Keypair) -> str:
        tx = Transaction.from_bytes(unsigned)
        tx.sign([signer], tx.message.recent_blockhash)
        return self._send_bytes(bytes(tx))

    def close(self) -> None:
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        shutil.rmtree(self.ledger, ignore_errors=True)
