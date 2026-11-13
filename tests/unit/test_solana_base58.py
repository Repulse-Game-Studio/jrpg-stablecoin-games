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


@pytest.mark.parametrize("call", [
    lambda t: decode_fixed(t, 64), lambda t: decode_fixed(t, 32), normalize_signature,
    normalize_pubkey,
], ids=["decode_fixed64", "decode_fixed32", "normalize_signature", "normalize_pubkey"])
@pytest.mark.parametrize("text", ["2" * 100_000, "1" * 100_000, "0" + "2" * 99_999, "z" * 89,
                                  " " + "2" * 89],
                         ids=["2x100000", "1x100000", "bad-first-char", "zx89", "padded2x89"])
def test_fixed_length_decoding_refuses_long_text_before_decoding(call, text, monkeypatch):
    """Base58 decoding is quadratic in the text's length: a fixed-length value is refused at
    once when its text is longer than the longest encoding of that many bytes (44 characters
    for 32 bytes, 88 for 64), before any decoding (a crafted stored id must not freeze launch).
    Checked structurally, not by the clock: the decoder is never called, and the refusal is the
    length's (a text whose first character is not base58 would otherwise fail on that)."""
    import scfg_core.solana.base58 as base58

    def no_decoding(t):
        raise AssertionError(f"decoded {len(t)} characters")
    monkeypatch.setattr(base58, "b58decode", no_decoding)
    with pytest.raises(ValueError, match="too long for (32|64) bytes"):
        call(text)


def test_the_length_check_spy_sees_the_decoding_of_valid_lengths(monkeypatch):
    """The spy above is wired: a text within the bound does reach the decoder."""
    import scfg_core.solana.base58 as base58
    seen = []
    real = base58.b58decode
    monkeypatch.setattr(base58, "b58decode", lambda t: seen.append(len(t)) or real(t))
    assert decode_fixed(TOKEN, 32) and seen == [len(TOKEN)]


@pytest.mark.parametrize("length", ["32", None, [32], float("nan"), float("inf"), 10**400, True,
                                    32.0, 32.5],
                         ids=["str", "None", "list", "nan", "inf", "huge", "bool", "float", "fraction"])
def test_a_length_that_is_not_a_byte_count_is_a_value_error(length):
    with pytest.raises(ValueError):
        decode_fixed(TOKEN, length)
    with pytest.raises(ValueError):
        decode_fixed("2" * 100_000, length)


def test_the_longest_encodings_still_decode():
    longest_sig, longest_key = b58encode(b"\xff" * 64), b58encode(b"\xff" * 32)
    assert (len(longest_sig), len(longest_key)) == (88, 44)
    assert normalize_signature(longest_sig) == longest_sig
    assert normalize_pubkey(longest_key) == longest_key
    assert normalize_signature("1" * 64) == "1" * 64       # 64 zero bytes
    with pytest.raises(ValueError):
        decode_fixed("1" * 89, 64)
