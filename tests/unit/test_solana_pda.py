import random

import pytest
from solders.keypair import Keypair
from solders.pubkey import Pubkey

from cryptopay_core.solana.base58 import b58decode
from cryptopay_core.solana.ed25519 import is_on_curve
from cryptopay_core.solana.pda import (
    ASSOCIATED_TOKEN_PROGRAM_ID,
    TOKEN_PROGRAM_ID,
    associated_token_address,
    find_program_address,
)

USDC = "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v"


def test_on_curve_matches_solders_for_random_bytes():
    rng = random.Random(11)
    for _ in range(1000):
        raw = bytes(rng.randrange(256) for _ in range(32))
        assert is_on_curve(raw) == Pubkey(raw).is_on_curve(), raw.hex()


def test_wallet_keys_are_on_curve():
    for _ in range(50):
        assert is_on_curve(bytes(Keypair().pubkey()))


def test_wrong_length_is_not_on_curve():
    assert is_on_curve(b"\x01" * 31) is False


def test_find_program_address_matches_solders():
    program = Pubkey.from_string(ASSOCIATED_TOKEN_PROGRAM_ID)
    for seed in (b"", b"a", b"cryptopay", bytes(32)):
        expected, bump = Pubkey.find_program_address([seed], program)
        assert find_program_address([seed], bytes(program)) == (bytes(expected), bump)


def test_associated_token_address_matches_solders():
    mint = Pubkey.from_string(USDC)
    for _ in range(25):
        owner = Keypair().pubkey()
        expected, _ = Pubkey.find_program_address(
            [bytes(owner), bytes(Pubkey.from_string(TOKEN_PROGRAM_ID)), bytes(mint)],
            Pubkey.from_string(ASSOCIATED_TOKEN_PROGRAM_ID))
        assert associated_token_address(str(owner), USDC) == str(expected)


def test_token_accounts_are_off_curve():
    ata = associated_token_address(str(Keypair().pubkey()), USDC)
    assert is_on_curve(b58decode(ata)) is False


@pytest.mark.parametrize("seeds", [[bytes(33)], [b"x"] * 16])
def test_seed_limits(seeds):
    with pytest.raises(ValueError):
        find_program_address(seeds, bytes(32))
