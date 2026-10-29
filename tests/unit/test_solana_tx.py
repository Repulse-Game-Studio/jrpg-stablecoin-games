import pytest
from solders.hash import Hash
from solders.instruction import AccountMeta as SMeta
from solders.instruction import Instruction as SInstruction
from solders.keypair import Keypair
from solders.message import Message
from solders.pubkey import Pubkey
from solders.transaction import Transaction

from cryptopay_core.solana.tx import (
    COMPUTE_BUDGET_PROGRAM_ID,
    AccountMeta,
    Instruction,
    compact_u16,
    compile_message,
    set_compute_unit_limit,
    set_compute_unit_price,
    transfer_checked,
    unsigned_transaction,
)

BLOCKHASH = str(Hash.new_unique())


@pytest.mark.parametrize("n, raw", [(0, b"\x00"), (127, b"\x7f"), (128, b"\x80\x01"),
                                     (16383, b"\xff\x7f"), (16384, b"\x80\x80\x01")])
def test_compact_u16(n, raw):
    assert compact_u16(n) == raw


def test_instruction_data():
    assert set_compute_unit_limit(30_000).data == bytes([2]) + (30_000).to_bytes(4, "little")
    assert set_compute_unit_price(5_000).data == bytes([3]) + (5_000).to_bytes(8, "little")
    k = [str(Keypair().pubkey()) for _ in range(5)]
    ix = transfer_checked(source=k[0], mint=k[1], destination=k[2], owner=k[3],
                          amount=4_993_742, decimals=6, extra=(k[4],))
    assert ix.data == bytes([12]) + (4_993_742).to_bytes(8, "little") + bytes([6])
    assert [(a.pubkey, a.is_signer, a.is_writable) for a in ix.accounts] == [
        (k[0], False, True), (k[1], False, False), (k[2], False, True), (k[3], True, False),
        (k[4], False, False)]


def _solders(ix):
    return SInstruction(Pubkey.from_string(ix.program_id), ix.data,
                        [SMeta(Pubkey.from_string(a.pubkey), a.is_signer, a.is_writable)
                         for a in ix.accounts])


def test_payment_message_is_byte_identical_to_solders():
    for _ in range(20):  # random keys exercise the account ordering
        payer = str(Keypair().pubkey())
        k = [str(Keypair().pubkey()) for _ in range(4)]
        ixs = [set_compute_unit_limit(30_000), set_compute_unit_price(1_000),
               transfer_checked(source=k[0], mint=k[1], destination=k[2], owner=payer,
                                amount=1_000_000, decimals=6, extra=(k[3],))]
        expected = Message.new_with_blockhash([_solders(i) for i in ixs],
                                              Pubkey.from_string(payer),
                                              Hash.from_string(BLOCKHASH))
        assert compile_message(payer, ixs, BLOCKHASH) == bytes(expected)


def test_unsigned_transaction_matches_solders():
    payer = str(Keypair().pubkey())
    ixs = [set_compute_unit_limit(30_000)]
    message = compile_message(payer, ixs, BLOCKHASH)
    expected = Transaction.new_unsigned(Message.from_bytes(message))
    assert unsigned_transaction(message) == bytes(expected)


def test_compute_budget_program_id():
    assert COMPUTE_BUDGET_PROGRAM_ID == "ComputeBudget111111111111111111111111111111"


def test_multi_signer_message_is_byte_identical_to_solders():
    # The brief's payment message has only the fee payer as a signer; these co-signers
    # exercise the writable-signer and readonly-signer groups and their ordering.
    payer = str(Keypair().pubkey())
    for _ in range(20):
        writable_signer = str(Keypair().pubkey())
        readonly_signer = str(Keypair().pubkey())
        k = [str(Keypair().pubkey()) for _ in range(3)]
        ixs = [Instruction(k[2], (AccountMeta(writable_signer, True, True),
                                  AccountMeta(readonly_signer, True, False),
                                  AccountMeta(k[0], False, True),
                                  AccountMeta(k[1], False, False),
                                  AccountMeta(payer, True, False)), bytes([1]))]
        expected = Message.new_with_blockhash([_solders(i) for i in ixs],
                                              Pubkey.from_string(payer),
                                              Hash.from_string(BLOCKHASH))
        assert compile_message(payer, ixs, BLOCKHASH) == bytes(expected)
