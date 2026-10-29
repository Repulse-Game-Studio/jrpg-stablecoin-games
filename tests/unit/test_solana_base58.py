import random

import pytest
from solders.pubkey import Pubkey
from solders.signature import Signature

from scfg_core.solana.base58 import (
    b58decode,
    b58encode,
    decode_fixed,
    normalize_pubkey,
    normalize_signature,
)

TOKEN = "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA"


@pytest.mark.parametrize("raw, text", [
    (b"", ""), (b"\x00", "1"), (b"\x00\x00\x01", "112"), (bytes(32), "1" * 32),
    (b"hello world", "StV1DL6CwTryKyV"),
])
def test_known_vectors(raw, text):
    assert b58encode(raw) == text
    if text:
        assert b58decode(text) == raw


def test_matches_solders_for_random_keys_and_signatures():
    rng = random.Random(7)
    for _ in range(200):
        key = bytes(rng.randrange(256) for _ in range(32))
        sig = bytes(rng.randrange(256) for _ in range(64))
        assert b58encode(key) == str(Pubkey(key))
        assert b58encode(sig) == str(Signature.from_bytes(sig))
        assert b58decode(str(Pubkey(key))) == key


@pytest.mark.parametrize("bad", ["", "0", "O", "I", "l", "abc def", None, 5])
def test_decode_rejects_bad_input(bad):
    with pytest.raises(ValueError):
        b58decode(bad)


def test_fixed_lengths():
    assert len(decode_fixed(TOKEN, 32)) == 32
    with pytest.raises(ValueError):
        decode_fixed(TOKEN, 64)
    assert normalize_pubkey("  " + TOKEN + "\n") == TOKEN
    with pytest.raises(ValueError):
        normalize_pubkey("112")
    sig = b58encode(bytes(range(64)))
    assert normalize_signature(" " + sig) == sig
    with pytest.raises(ValueError):
        normalize_signature(TOKEN)
