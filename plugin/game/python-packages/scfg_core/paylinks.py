"""Payment links (EIP-681) and QR code geometry.

The QR matrix comes from the vendored Project Nayuki generator (_qrcodegen.py).
Rendering is left to Ren'Py: ``qr_rects`` merges each row's dark modules into
horizontal runs, so the screen draws a few hundred rectangles instead of thousands
of single squares.
"""

from __future__ import annotations

import re

from ._qrcodegen import QrCode

# The "Pay with card" page (configure(onramp=...)): a template whose {placeholders} are filled
# in for the network the player picked and the payment's session. The default is thirdweb's
# hosted Checkout page, which needs no account or key: the player pays by card and thirdweb
# sends the token to `seller`, the developer's wallet, on that network. {amount} is the
# session's exact amount with all 6 decimals, so the game finds the payment as it finds any
# other (amounts.encode). paymentMethods=card offers card payment only (thirdweb's iframe docs).
# NOT YET CHECKED with a real card: that thirdweb delivers the amount exactly, to 6 decimals.
ONRAMP_URL = ("https://thirdweb.com/bridge/checkout-widget?chain={chain_id}"
              "&tokenAddress={token_address}&amount={amount}&seller={recipient}"
              "&paymentMethods=card")
ONRAMP_FIELDS = ("amount", "chain", "chain_id", "recipient", "token", "token_address")
ONRAMP_PLACEHOLDER = re.compile(r"\{([^{}]*)\}")


def eip681_transfer_uri(*, token_address: str, chain_id: int, recipient: str, units: int) -> str:
    """ERC-20 transfer request that mobile wallets understand when scanned or opened."""
    return f"ethereum:{token_address}@{chain_id}/transfer?address={recipient}&uint256={units}"


def onramp_url(template: str, *, chain: str, chain_id: int, token: str, token_address: str,
               amount: str, recipient: str) -> str:
    """Fill a validated onramp template (config._onramp_url). Every value is made of URL-safe
    characters (a chain key, digits, a 0x address, a token symbol, an amount), so none is
    escaped."""
    values = {"amount": amount, "chain": chain, "chain_id": str(chain_id), "token": token,
              "token_address": token_address, "recipient": recipient}
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
