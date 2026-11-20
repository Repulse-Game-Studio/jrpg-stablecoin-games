"""Payment links (EIP-681) and QR code geometry.

The QR matrix comes from the vendored Project Nayuki generator (_qrcodegen.py).
Rendering is left to Ren'Py: ``qr_rects`` merges each row's dark modules into
horizontal runs, so the screen draws a few hundred rectangles instead of thousands
of single squares.
"""

from __future__ import annotations

import re

from ._qrcodegen import QrCode

# The buy page offered to players who have no stablecoin yet (configure(onramp=...)): a
# template whose {placeholders} are filled in for the network the player picked. The default
# is thirdweb's hosted Buy page, which needs no account or key; the player connects their own
# wallet there and buys with a card, then pays as usual. tokenEditable=false keeps the page on
# the network and token the game asked for, and paymentMethods=card offers card payment only
# (thirdweb's iframe docs). tokenAddress is not in those docs, but the hosted page honours it:
# checked in Chromium on 2026-11-18 for USDC on Base, Arbitrum and Arc and USDT on Ethereum and
# Polygon (without it the page offers the network's native coin).
ONRAMP_URL = ("https://thirdweb.com/bridge/buy-widget?chain={chain_id}"
              "&tokenAddress={token_address}&amount={amount}&tokenEditable=false"
              "&paymentMethods=card")
ONRAMP_FIELDS = ("amount", "chain", "chain_id", "token", "token_address")
ONRAMP_PLACEHOLDER = re.compile(r"\{([^{}]*)\}")
# 6-decimal units in a cent: the buy page's amount is rounded up to whole cents.
UNITS_PER_CENT = 10_000
# On a chain whose gas coin is the token (Arc), the payment's fee is paid in it too: a cent.
ONRAMP_GAS_UNITS = UNITS_PER_CENT
# Before a session exists its exact amount is not known: it is the price plus the item code and
# nonce in the last four decimals (amounts.encode), at most 0.009999. The buy link then adds that
# most, so it never suggests less than the payment will need.
ONRAMP_EXACT_UNITS = 9_999


def eip681_transfer_uri(*, token_address: str, chain_id: int, recipient: str, units: int) -> str:
    """ERC-20 transfer request that mobile wallets understand when scanned or opened."""
    return f"ethereum:{token_address}@{chain_id}/transfer?address={recipient}&uint256={units}"


def onramp_amount(units: int, gas_is_token: bool) -> str:
    """The amount suggested on the buy page: the payment (6-decimal units), plus a cent for
    the fee where the token is the gas coin, rounded up to whole cents: 4993742 -> '5.00'."""
    total = units + (ONRAMP_GAS_UNITS if gas_is_token else 0)
    cents = -(-total // UNITS_PER_CENT)
    return f"{cents // 100}.{cents % 100:02d}"


def onramp_url(template: str, *, chain: str, chain_id: int, token: str, token_address: str,
               amount: str) -> str:
    """Fill a validated onramp template (config._onramp_url). Every value is made of URL-safe
    characters (a chain key, digits, a 0x address, a token symbol, an amount), so none is
    escaped."""
    values = {"amount": amount, "chain": chain, "chain_id": str(chain_id), "token": token,
              "token_address": token_address}
    return ONRAMP_PLACEHOLDER.sub(lambda m: values[m.group(1)], template)


def qr_matrix(text: str) -> list[list[bool]]:
    """Square matrix of modules, True = dark. Medium error correction."""
    qr = QrCode.encode_text(text, QrCode.Ecc.MEDIUM)
    n = qr.get_size()
    return [[qr.get_module(x, y) for x in range(n)] for y in range(n)]


def qr_rects(text: str, border: int = 2) -> tuple[int, list[tuple[int, int, int]]]:
    """(size in modules including the quiet-zone border, [(x, y, width), ...] dark runs)."""
    matrix = qr_matrix(text)
    n = len(matrix)
    runs = []
    for y, row in enumerate(matrix):
        x = 0
        while x < n:
            if row[x]:
                start = x
                while x < n and row[x]:
                    x += 1
                runs.append((start + border, y + border, x - start))
            else:
                x += 1
    return n + 2 * border, runs
