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


# ISO/IEC 18004:2015 Annex E, Table E.1: row/column coordinates of the alignment pattern
# centres per version (version 1 has none).
ALIGNMENT_CENTRES = {
    2: [6, 18], 3: [6, 22], 4: [6, 26], 5: [6, 30], 6: [6, 34], 7: [6, 22, 38],
    8: [6, 24, 42], 9: [6, 26, 46], 10: [6, 28, 50], 11: [6, 30, 54], 12: [6, 32, 58],
    13: [6, 34, 62], 14: [6, 26, 46, 66], 15: [6, 26, 48, 70], 16: [6, 26, 50, 74],
    17: [6, 30, 54, 78], 18: [6, 30, 56, 82], 19: [6, 30, 58, 86], 20: [6, 34, 62, 90],
    21: [6, 28, 50, 72, 94], 22: [6, 26, 50, 74, 98], 23: [6, 30, 54, 78, 102],
    24: [6, 28, 54, 80, 106], 25: [6, 32, 58, 84, 110], 26: [6, 30, 58, 86, 114],
    27: [6, 34, 62, 90, 118], 28: [6, 26, 50, 74, 98, 122], 29: [6, 30, 54, 78, 102, 126],
    30: [6, 26, 52, 78, 104, 130], 31: [6, 30, 56, 82, 108, 134], 32: [6, 34, 60, 86, 112, 138],
    33: [6, 30, 58, 86, 114, 142], 34: [6, 34, 62, 90, 118, 146],
    35: [6, 30, 54, 78, 102, 126, 150], 36: [6, 24, 50, 76, 102, 128, 154],
    37: [6, 28, 54, 80, 106, 132, 158], 38: [6, 32, 58, 84, 110, 136, 162],
    39: [6, 26, 54, 82, 110, 138, 166], 40: [6, 30, 58, 86, 114, 142, 170],
}


def test_qr_alignment_patterns_sit_where_the_standard_puts_them():
    """Every version draws its alignment patterns at the standard's centres, version 32 (step
    26, where a naive formula gives 28) included: a dark centre, a light ring and a dark ring,
    except under the three finder patterns."""
    from scfg_core._qrcodegen import QrCode

    for version in range(1, 41):
        ecl = QrCode.Ecc.MEDIUM
        qr = QrCode(version, ecl, bytes(QrCode._get_num_data_codewords(version, ecl)), 0)
        assert qr._get_alignment_pattern_positions() == ALIGNMENT_CENTRES.get(version, [])
        centres = ALIGNMENT_CENTRES.get(version, [])
        last = len(centres) - 1
        for i, cx in enumerate(centres):
            for j, cy in enumerate(centres):
                if (i, j) in ((0, 0), (0, last), (last, 0)):
                    continue
                for dy in range(-2, 3):
                    for dx in range(-2, 3):
                        ring = max(abs(dx), abs(dy))
                        assert qr.get_module(cx + dx, cy + dy) == (ring != 1), (version, cx, cy)


def test_a_version_32_payment_link_matrix():
    """The longest links stay far below version 32, but the encoder handles it: 1453 bytes
    need version 32 at medium error correction (145 x 145 modules)."""
    m = qr_matrix("a" * 1453)
    assert len(m) == 17 + 4 * 32
