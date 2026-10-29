import pytest

from cryptopay_core.keccak import keccak256


@pytest.mark.parametrize(
    "data, expected",
    [
        (b"", "c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470"),
        (b"abc", "4e03657aea45a94fc7d47ba826c8d667c0d1e6e33a64a036ec44f58fa12d6c45"),
        (
            b"Transfer(address,address,uint256)",
            "ddf252ad1be2c89b69c2b068fc378daa952ba7f163c4a11628f55a4df523b3ef",
        ),
        # 200 bytes spans two 136-byte blocks: exercises multi-block absorption
        (b"a" * 200, "96ea54061def936c4be90b518992fdc6f12f535068a256229aca54267b4d084d"),
    ],
)
def test_keccak256_vectors(data, expected):
    assert keccak256(data).hex() == expected


def test_keccak256_returns_32_bytes():
    assert len(keccak256(b"x")) == 32
