from scfg_core.paylinks import eip681_transfer_uri, qr_matrix, qr_rects


def test_eip681_uri():
    uri = eip681_transfer_uri(token_address="0xaf88d065e77c8cC2239327C5EDb3A432268e5831",
                              chain_id=42161, recipient="0x52908400098527886E0F7030069857D2E4169EE7",
                              units=4_993_742)
    assert uri == ("ethereum:0xaf88d065e77c8cC2239327C5EDb3A432268e5831@42161/transfer"
                   "?address=0x52908400098527886E0F7030069857D2E4169EE7&uint256=4993742")


def test_qr_matrix_is_square_with_finder_patterns():
    m = qr_matrix("hello")
    n = len(m)
    assert all(len(row) == n for row in m)
    # every QR code has a solid 7x7 finder outline in the top-left corner
    assert all(m[0][x] for x in range(7)) and all(m[y][0] for y in range(7))


def test_qr_rects_cover_exactly_the_dark_modules():
    text = "ethereum:0xaf88d065e77c8cC2239327C5EDb3A432268e5831@42161/transfer?uint256=1"
    m = qr_matrix(text)
    size, rects = qr_rects(text, border=2)
    assert size == len(m) + 4
    painted = {(x + dx - 2, y - 2) for x, y, w in rects for dx in range(w)}
    dark = {(x, y) for y, row in enumerate(m) for x, v in enumerate(row) if v}
    assert painted == dark
    assert len(rects) < len(dark)   # runs are merged
