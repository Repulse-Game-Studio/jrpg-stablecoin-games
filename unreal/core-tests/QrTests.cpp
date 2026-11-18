// Copyright 2026 Repulse
// No Python twin: tests of scfg::qrcodegen beyond the tier1/qr.json vectors: every version boundary of every mode and
// level, the level boost and the mask choice, forced masks, every version's function patterns (the alignment centres
// of ISO/IEC 18004 Table E.1, version 32's step 26 included), the segment builders and every error.
//
// Every expectation is a differential against scfg_core._qrcodegen, computed with the project's interpreter
// (`uv run python`, CPython 3.9.25) from the repository root. The hash is unity/core-tests/QrTests.cs's: over the
// rows, x = (x * 131 + module + 7) % 1000000007. The snippet:
//
//   import sys; sys.path.insert(0, "plugin/game/python-packages")
//   from scfg_core._qrcodegen import QrCode, QrSegment
//   E = [QrCode.Ecc.LOW, QrCode.Ecc.MEDIUM, QrCode.Ecc.QUARTILE, QrCode.Ecc.HIGH]
//   def h(qr):
//       x = 0
//       for y in range(qr.get_size()):
//           for xx in range(qr.get_size()):
//               x = (x * 131 + (1 if qr.get_module(xx, y) else 0) + 7) % 1000000007
//       return x
//   # kBoost: for each text, level and boostecl b
//   qr = QrCode.encode_segments(QrSegment.make_segments(text), E[e], boostecl=b)
//   print(qr.get_version(), qr.get_error_correction_level().ordinal, qr.get_mask(), h(qr))
//   # kForcedMasks: [h(QrCode.encode_segments(QrSegment.make_segments(EIP681), E[0], mask=k, boostecl=False))
//   #               for k in range(8)]
//   # kEveryVersion: [h(QrCode(v, E[v % 4], bytes((i * 7 + v) & 255 for i in
//   #                  range(QrCode._get_num_data_codewords(v, E[v % 4]))), v % 8)) for v in range(1, 41)]
//   # kCapacity: for each mode m ("x", "7", "Z": byte, numeric, alphanumeric), level e and version v, the largest n
//   #            with QrSegment.get_total_bits(QrSegment.make_segments(m * n), v)
//   #                 <= QrCode._get_num_data_codewords(v, E[e]) * 8
//
// kCapacity is also ISO/IEC 18004 Table 7 (40-L holds 2953 bytes, 7089 digits, 4296 alphanumerics; 1-M 14, 34, 20).
#include "support/Test.h"

#include "Scfg/Err.h"
#include "Scfg/Paylinks.h"
#include "Scfg/QrCodeGen.h"
#include "Scfg/Result.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using scfg::Result;
using scfg::qrcodegen::QrCode;
using scfg::qrcodegen::QrSegment;
using Ecc = QrCode::Ecc;
namespace qrcodegen = scfg::qrcodegen;

static constexpr std::array<Ecc, 4> kLevels = {Ecc::LOW, Ecc::MEDIUM, Ecc::QUARTILE, Ecc::HIGH};

static int64_t hash(const QrCode& qr) {
    int64_t x = 0;
    for (int y = 0; y < qr.get_size(); ++y) {
        for (int xx = 0; xx < qr.get_size(); ++xx) x = (x * 131 + (qr.get_module(xx, y) ? 1 : 0) + 7) % 1000000007;
    }
    return x;
}

static int64_t hash(const std::vector<std::vector<bool>>& m) {
    int64_t x = 0;
    for (const auto& row : m) {
        for (bool c : row) x = (x * 131 + (c ? 1 : 0) + 7) % 1000000007;
    }
    return x;
}

static std::string repeat(std::string_view s, size_t n) {
    std::string out;
    for (size_t i = 0; i < n; ++i) out += s;
    return out;
}

template <class T>
static std::string outcome(const Result<T>& r) {
    if (r.ok()) return "ok";
    return r.err().kind + ": " + r.err().message;
}

static std::string bits(const std::vector<bool>& b) {
    std::string s;
    for (bool x : b) s += x ? '1' : '0';
    return s;
}

// encode_segments(make_segments(text), ecl, 1, 40, mask, boostecl).
static Result<QrCode> encode(std::string_view text, Ecc ecl, int mask = -1, bool boostecl = true) {
    SCFG_TRY(std::vector<QrSegment> segs, QrSegment::make_segments(text));
    return QrCode::encode_segments(segs, ecl, 1, 40, mask, boostecl);
}

static const std::string kEip681 =
    "ethereum:0xA0b86991c6218b36c1d19D4a2e9Eb0cE3606eB48@1/transfer?address=0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed"
    "&uint256=4993742";

// ---- qr_matrix sizes and contents (the hashes of unity/core-tests/QrTests.cs, recomputed) ----

SCFG_TEST(QrTests, MatricesOfEveryMode) {
    struct Case {
        std::string text;
        size_t size;
        int64_t hash;
    };
    const std::vector<Case> cases = {
        {"", 21, 227443507},
        {"12345", 21, 688946133},
        {"0", 21, 544011565},
        {"HELLO WORLD", 21, 202162885},
        {repeat("A", 50), 29, 452248346},
        {repeat("1", 300), 49, 754031077},
        {repeat("AB-C.D/E:F$%*+ ", 40), 77, 730634772},
        {repeat("x", 1000), 121, 778860690},
        {repeat("\xC3\xA9", 500), 121, 374628804},  // é: 1000 bytes
        {repeat("a", 2000), 169, 653302781},
        {repeat("x", 2331), 177, 711778971},        // the most bytes at medium
    };
    for (const Case& c : cases) {
        Result<std::vector<std::vector<bool>>> m = scfg::paylinks::qr_matrix(c.text);
        if (!SCFG_CHECK_EQ(t, outcome(m), "ok")) continue;
        SCFG_CHECK_EQ(t, m.value().size(), c.size);
        for (const auto& row : m.value()) SCFG_CHECK_EQ(t, row.size(), c.size);
        t.check(hash(m.value()) == c.hash, "qr_matrix hash of a " + std::to_string(c.text.size()) + "-byte text");
    }
}

// ---- version boundaries ----

// kCapacity[mode][level][version - 1]: the most characters that fit (byte, numeric, alphanumeric).
static constexpr std::array<std::array<std::array<int, 40>, 4>, 3> kCapacity = {{
    {{
        {17, 32, 53, 78, 106, 134, 154, 192, 230, 271, 321, 367, 425, 458, 520, 586, 644, 718, 792, 858,
         929, 1003, 1091, 1171, 1273, 1367, 1465, 1528, 1628, 1732, 1840, 1952, 2068, 2188, 2303, 2431, 2563, 2699,
         2809, 2953},
        {14, 26, 42, 62, 84, 106, 122, 152, 180, 213, 251, 287, 331, 362, 412, 450, 504, 560, 624, 666,
         711, 779, 857, 911, 997, 1059, 1125, 1190, 1264, 1370, 1452, 1538, 1628, 1722, 1809, 1911, 1989, 2099,
         2213, 2331},
        {11, 20, 32, 46, 60, 74, 86, 108, 130, 151, 177, 203, 241, 258, 292, 322, 364, 394, 442, 482,
         509, 565, 611, 661, 715, 751, 805, 868, 908, 982, 1030, 1112, 1168, 1228, 1283, 1351, 1423, 1499, 1579,
         1663},
        {7, 14, 24, 34, 44, 58, 64, 84, 98, 119, 137, 155, 177, 194, 220, 250, 280, 310, 338, 382,
         403, 439, 461, 511, 535, 593, 625, 658, 698, 742, 790, 842, 898, 958, 983, 1051, 1093, 1139, 1219, 1273},
    }},
    {{
        {41, 77, 127, 187, 255, 322, 370, 461, 552, 652, 772, 883, 1022, 1101, 1250, 1408, 1548, 1725, 1903, 2061,
         2232, 2409, 2620, 2812, 3057, 3283, 3517, 3669, 3909, 4158, 4417, 4686, 4965, 5253, 5529, 5836, 6153, 6479,
         6743, 7089},
        {34, 63, 101, 149, 202, 255, 293, 365, 432, 513, 604, 691, 796, 871, 991, 1082, 1212, 1346, 1500, 1600,
         1708, 1872, 2059, 2188, 2395, 2544, 2701, 2857, 3035, 3289, 3486, 3693, 3909, 4134, 4343, 4588, 4775, 5039,
         5313, 5596},
        {27, 48, 77, 111, 144, 178, 207, 259, 312, 364, 427, 489, 580, 621, 703, 775, 876, 948, 1063, 1159,
         1224, 1358, 1468, 1588, 1718, 1804, 1933, 2085, 2181, 2358, 2473, 2670, 2805, 2949, 3081, 3244, 3417, 3599,
         3791, 3993},
        {17, 34, 58, 82, 106, 139, 154, 202, 235, 288, 331, 374, 427, 468, 530, 602, 674, 746, 813, 919,
         969, 1056, 1108, 1228, 1286, 1425, 1501, 1581, 1677, 1782, 1897, 2022, 2157, 2301, 2361, 2524, 2625, 2735,
         2927, 3057},
    }},
    {{
        {25, 47, 77, 114, 154, 195, 224, 279, 335, 395, 468, 535, 619, 667, 758, 854, 938, 1046, 1153, 1249,
         1352, 1460, 1588, 1704, 1853, 1990, 2132, 2223, 2369, 2520, 2677, 2840, 3009, 3183, 3351, 3537, 3729, 3927,
         4087, 4296},
        {20, 38, 61, 90, 122, 154, 178, 221, 262, 311, 366, 419, 483, 528, 600, 656, 734, 816, 909, 970,
         1035, 1134, 1248, 1326, 1451, 1542, 1637, 1732, 1839, 1994, 2113, 2238, 2369, 2506, 2632, 2780, 2894, 3054,
         3220, 3391},
        {16, 29, 47, 67, 87, 108, 125, 157, 189, 221, 259, 296, 352, 376, 426, 470, 531, 574, 644, 702,
         742, 823, 890, 963, 1041, 1094, 1172, 1263, 1322, 1429, 1499, 1618, 1700, 1787, 1867, 1966, 2071, 2181,
         2298, 2420},
        {10, 20, 35, 50, 64, 84, 93, 122, 143, 174, 200, 227, 259, 283, 321, 365, 408, 452, 493, 557,
         587, 640, 672, 744, 779, 864, 910, 958, 1016, 1080, 1150, 1226, 1307, 1394, 1431, 1530, 1591, 1658, 1774,
         1852},
    }},
}};

// Every version boundary of every mode at every level: capacity(v) characters give version v, one more gives
// version v + 1 (DataTooLongError after 40). The mask is forced, so no penalty is scored (the version does not
// depend on it), and the level is not boosted, so the result keeps it.
SCFG_TEST(QrTests, EveryVersionBoundary) {
    const std::array<std::string_view, 3> unit = {"x", "7", "Z"};
    const std::array<QrSegment::Mode, 3> modes = {QrSegment::Mode::BYTE, QrSegment::Mode::NUMERIC,
                                                  QrSegment::Mode::ALPHANUMERIC};
    for (size_t m = 0; m < unit.size(); ++m) {
        for (size_t e = 0; e < kLevels.size(); ++e) {
            for (int v = 1; v <= 40; ++v) {
                const auto n = static_cast<size_t>(kCapacity[m][e][static_cast<size_t>(v - 1)]);
                const std::string where = std::string(unit[m]) + " level " + std::to_string(e) + " version " +
                                          std::to_string(v);
                Result<std::vector<QrSegment>> segs = QrSegment::make_segments(repeat(unit[m], n));
                if (!t.check(segs.ok() && segs.value().size() == 1 && segs.value()[0].get_mode() == modes[m], where)) {
                    continue;
                }
                Result<QrCode> fits = encode(repeat(unit[m], n), kLevels[e], 0, false);
                t.check(fits.ok() && fits.value().get_version() == v && fits.value().get_size() == v * 4 + 17 &&
                            fits.value().get_error_correction_level() == kLevels[e] && fits.value().get_mask() == 0,
                        "fits " + where);
                Result<QrCode> over = encode(repeat(unit[m], n + 1), kLevels[e], 0, false);
                if (v < 40) {
                    t.check(over.ok() && over.value().get_version() == v + 1, "one more than " + where);
                } else {
                    t.check(!over.ok() && over.err().is_a(qrcodegen::DataTooLongError) && over.err().is_a("ValueError"),
                            "one more than " + where);
                }
            }
        }
    }
}

// ---- level boost and mask choice ----

SCFG_TEST(QrTests, LevelBoostAndMaskMatchPython) {
    const std::vector<std::string> texts = {"a", "HELLO WORLD", "12345678901234567890", kEip681,
                                            "Caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E \xF0\x9F\x8E\xAE",
                                            repeat("x", 300)};
    struct Case {
        int text, ecl;
        bool boost;
        int version, result_ecl, mask;
        int64_t hash;
    };
    // kBoost
    const std::vector<Case> cases = {
        {0, 0, true, 1, 3, 6, 114945259},   {0, 0, false, 1, 0, 0, 219521229},  {0, 1, true, 1, 3, 6, 114945259},
        {0, 1, false, 1, 1, 5, 20529507},   {0, 2, true, 1, 3, 6, 114945259},   {0, 2, false, 1, 2, 0, 159085410},
        {0, 3, true, 1, 3, 6, 114945259},   {0, 3, false, 1, 3, 6, 114945259},  {1, 0, true, 1, 2, 0, 202162885},
        {1, 0, false, 1, 0, 7, 359793355},  {1, 1, true, 1, 2, 0, 202162885},   {1, 1, false, 1, 1, 0, 867999678},
        {1, 2, true, 1, 2, 0, 202162885},   {1, 2, false, 1, 2, 0, 202162885},  {1, 3, true, 2, 3, 5, 617487247},
        {1, 3, false, 2, 3, 5, 617487247},  {2, 0, true, 1, 2, 4, 910613651},   {2, 0, false, 1, 0, 7, 932923471},
        {2, 1, true, 1, 2, 4, 910613651},   {2, 1, false, 1, 1, 3, 686074113},  {2, 2, true, 1, 2, 4, 910613651},
        {2, 2, false, 1, 2, 4, 910613651},  {2, 3, true, 2, 3, 3, 528133932},   {2, 3, false, 2, 3, 3, 528133932},
        {3, 0, true, 6, 0, 2, 993430334},   {3, 0, false, 6, 0, 2, 993430334},  {3, 1, true, 8, 1, 5, 224654475},
        {3, 1, false, 8, 1, 5, 224654475},  {3, 2, true, 9, 2, 2, 575823592},   {3, 2, false, 9, 2, 2, 575823592},
        {3, 3, true, 11, 3, 3, 688585901},  {3, 3, false, 11, 3, 3, 688585901}, {4, 0, true, 2, 2, 4, 918632917},
        {4, 0, false, 2, 0, 2, 278511343},  {4, 1, true, 2, 2, 4, 918632917},   {4, 1, false, 2, 1, 1, 984447343},
        {4, 2, true, 2, 2, 4, 918632917},   {4, 2, false, 2, 2, 4, 918632917},  {4, 3, true, 3, 3, 1, 539756801},
        {4, 3, false, 3, 3, 1, 539756801},  {5, 0, true, 11, 0, 0, 21745294},   {5, 0, false, 11, 0, 0, 21745294},
        {5, 1, true, 13, 1, 0, 122988421},  {5, 1, false, 13, 1, 0, 122988421}, {5, 2, true, 16, 2, 0, 260104666},
        {5, 2, false, 16, 2, 0, 260104666}, {5, 3, true, 18, 3, 0, 384028403},  {5, 3, false, 18, 3, 0, 384028403},
    };
    for (const Case& c : cases) {
        const std::string where = "text " + std::to_string(c.text) + " level " + std::to_string(c.ecl) +
                                  (c.boost ? " boosted" : " as asked");
        const std::string& text = texts[static_cast<size_t>(c.text)];
        Result<QrCode> qr = encode(text, kLevels[static_cast<size_t>(c.ecl)], -1, c.boost);
        if (!t.check(qr.ok(), where + ": " + outcome(qr))) continue;
        t.check(qr.value().get_version() == c.version, where + ": version");
        t.check(QrCode::ordinal(qr.value().get_error_correction_level()) == c.result_ecl, where + ": level");
        t.check(qr.value().get_mask() == c.mask, where + ": mask");
        t.check(hash(qr.value()) == c.hash, where + ": modules");
    }
    // encode_text is make_segments + encode_segments with the defaults.
    Result<QrCode> text = QrCode::encode_text("a", Ecc::LOW);
    SCFG_CHECK(t, text.ok() && hash(text.value()) == 114945259);
}

SCFG_TEST(QrTests, ForcedMasks) {
    // kForcedMasks: the EIP-681 link at LOW (version 6), each mask forced.
    const std::array<int64_t, 8> expected = {597515813, 608559086, 993430334, 649108117,
                                             836343469, 485018365, 809542535, 987775813};
    for (int k = 0; k < 8; ++k) {
        Result<QrCode> qr = encode(kEip681, Ecc::LOW, k, false);
        if (!SCFG_CHECK_EQ(t, outcome(qr), "ok")) continue;
        SCFG_CHECK_EQ(t, qr.value().get_mask(), k);
        SCFG_CHECK_EQ(t, qr.value().get_version(), 6);
        t.check(hash(qr.value()) == expected[static_cast<size_t>(k)], "mask " + std::to_string(k));
    }
}

// The mask encode_text chooses (the lowest penalty score) for 400 texts, which pins the penalty rules and weights:
//   "".join(str(QrCode.encode_text("scfg-%d:" % k * (k % 9 + 1), E[k % 4]).get_mask()) for k in range(400))
SCFG_TEST(QrTests, AutoMaskOf400Texts) {
    const std::string expected =
        "53322222266022222275232222222232227226762242226222462207266222725252222226522222265722422262262222226722222224"
        "72222222052222222322222227772222220046222227222772227552222222632222222642222227622222222722222222022222247402"
        "22272762222322722222222602222222262622252202622222622222222167222222262222232622222222722222222772222222042722"
        "2320622272226622222222222222227724222226632222222222222222662522222267";
    std::string got;
    for (int k = 0; k < 400; ++k) {
        std::string text;
        for (int i = 0; i < k % 9 + 1; ++i) text += "scfg-" + std::to_string(k) + ":";
        Result<QrCode> qr = QrCode::encode_text(text, kLevels[static_cast<size_t>(k % 4)]);
        got += qr.ok() ? static_cast<char>('0' + qr.value().get_mask()) : '?';
    }
    SCFG_CHECK_EQ(t, got, expected);
}

// ---- the low-level constructor at every version ----

SCFG_TEST(QrTests, EveryVersionMatchesPython) {
    // kEveryVersion: QrCode(v, E[v % 4], (i * 7 + v) & 255 for each data codeword i, v % 8).
    const std::array<int64_t, 40> expected = {
        664137975, 825951676, 910680273, 529034974, 753199438, 449069922, 358885154, 889847964, 682293702, 427324834,
        342158116, 124708757, 495862500, 950013104, 404302400, 907086272, 583705032, 739407849, 978026181, 743455202,
        458405584, 242357609, 235032101, 832749649, 383786505, 544480547, 796037071, 13009965,  11701851,  623854615,
        355283076, 340130730, 107811777, 840156132, 304841036, 207853914, 406064284, 202045004, 716185832, 303387824,
    };
    for (int v = 1; v <= 40; ++v) {
        const Ecc ecl = kLevels[static_cast<size_t>(v % 4)];
        // The data capacity: the one length make accepts.
        size_t n = 0;
        while (!QrCode::make(v, ecl, std::vector<uint8_t>(n), 0).ok() && n < 4000) ++n;
        std::vector<uint8_t> data(n);
        for (size_t i = 0; i < n; ++i) data[i] = static_cast<uint8_t>((i * 7 + static_cast<size_t>(v)) & 255);
        Result<QrCode> qr = QrCode::make(v, ecl, data, v % 8);
        if (!t.check(qr.ok(), "version " + std::to_string(v) + ": " + outcome(qr))) continue;
        SCFG_CHECK_EQ(t, qr.value().get_size(), v * 4 + 17);
        SCFG_CHECK_EQ(t, qr.value().get_mask(), v % 8);
        t.check(hash(qr.value()) == expected[static_cast<size_t>(v - 1)], "version " + std::to_string(v));
    }
}

// ISO/IEC 18004:2015 Annex E, Table E.1 (as tests/unit/test_paylinks.py): every alignment pattern is a dark centre,
// a light ring and a dark ring at the standard's centres, except under the three finder patterns. Version 32's
// centres are 26 apart (a naive step formula gives 28).
SCFG_TEST(QrTests, AlignmentPatternsSitWhereTheStandardPutsThem) {
    const std::vector<std::vector<int>> centres = {
        {}, {6, 18}, {6, 22}, {6, 26}, {6, 30}, {6, 34}, {6, 22, 38}, {6, 24, 42}, {6, 26, 46}, {6, 28, 50},
        {6, 30, 54}, {6, 32, 58}, {6, 34, 62}, {6, 26, 46, 66}, {6, 26, 48, 70}, {6, 26, 50, 74}, {6, 30, 54, 78},
        {6, 30, 56, 82}, {6, 30, 58, 86}, {6, 34, 62, 90}, {6, 28, 50, 72, 94}, {6, 26, 50, 74, 98},
        {6, 30, 54, 78, 102}, {6, 28, 54, 80, 106}, {6, 32, 58, 84, 110}, {6, 30, 58, 86, 114},
        {6, 34, 62, 90, 118}, {6, 26, 50, 74, 98, 122}, {6, 30, 54, 78, 102, 126}, {6, 26, 52, 78, 104, 130},
        {6, 30, 56, 82, 108, 134}, {6, 34, 60, 86, 112, 138}, {6, 30, 58, 86, 114, 142}, {6, 34, 62, 90, 118, 146},
        {6, 30, 54, 78, 102, 126, 150}, {6, 24, 50, 76, 102, 128, 154}, {6, 28, 54, 80, 106, 132, 158},
        {6, 32, 58, 84, 110, 136, 162}, {6, 26, 54, 82, 110, 138, 166}, {6, 30, 58, 86, 114, 142, 170},
    };
    SCFG_CHECK_EQ(t, centres.size(), 40);
    for (int v = 1; v <= 40; ++v) {
        size_t n = 0;
        while (!QrCode::make(v, Ecc::MEDIUM, std::vector<uint8_t>(n), 0).ok() && n < 4000) ++n;
        Result<QrCode> qr = QrCode::make(v, Ecc::MEDIUM, std::vector<uint8_t>(n), 0);
        if (!SCFG_CHECK_EQ(t, outcome(qr), "ok")) continue;
        const std::vector<int>& c = centres[static_cast<size_t>(v - 1)];
        const size_t last = c.empty() ? 0 : c.size() - 1;
        bool good = true;
        for (size_t i = 0; i < c.size(); ++i) {
            for (size_t j = 0; j < c.size(); ++j) {
                if ((i == 0 && j == 0) || (i == 0 && j == last) || (i == last && j == 0)) continue;
                for (int dy = -2; dy <= 2; ++dy) {
                    for (int dx = -2; dx <= 2; ++dx) {
                        const int ring = std::max(dx < 0 ? -dx : dx, dy < 0 ? -dy : dy);
                        good = good && qr.value().get_module(c[i] + dx, c[j] + dy) == (ring != 1);
                    }
                }
            }
        }
        t.check(good, "alignment patterns of version " + std::to_string(v));
    }
    // The version_32 vector's own geometry: 1453 bytes need version 32 at medium.
    Result<QrCode> v32 = QrCode::encode_text(repeat("a", 1453), Ecc::MEDIUM);
    SCFG_CHECK(t, v32.ok() && v32.value().get_version() == 32 && v32.value().get_size() == 145);
}

SCFG_TEST(QrTests, GetModuleOutOfBoundsIsLight) {
    Result<QrCode> qr = QrCode::encode_text("a", Ecc::LOW);
    if (!SCFG_CHECK_EQ(t, outcome(qr), "ok")) return;
    SCFG_CHECK(t, qr.value().get_module(0, 0));  // the finder's corner
    SCFG_CHECK(t, !qr.value().get_module(-1, 0));
    SCFG_CHECK(t, !qr.value().get_module(0, -1));
    SCFG_CHECK(t, !qr.value().get_module(21, 0));
    SCFG_CHECK(t, !qr.value().get_module(0, 21));
}

// ---- segments ----

SCFG_TEST(QrTests, SegmentBuilders) {
    struct Eci {
        int64_t value;
        const char* bits;
    };
    for (const Eci& e : std::vector<Eci>{{0, "00000000"},
                                         {127, "01111111"},
                                         {128, "1000000010000000"},
                                         {16383, "1011111111111111"},
                                         {16384, "110000000100000000000000"},
                                         {999999, "110011110100001000111111"}}) {
        Result<QrSegment> s = QrSegment::make_eci(e.value);
        if (!SCFG_CHECK_EQ(t, outcome(s), "ok")) continue;
        SCFG_CHECK_EQ(t, bits(s.value().get_data()), e.bits);
        SCFG_CHECK(t, s.value().get_mode() == QrSegment::Mode::ECI && s.value().get_num_chars() == 0);
    }
    Result<QrSegment> num = QrSegment::make_numeric("0123456");
    SCFG_CHECK(t, num.ok() && bits(num.value().get_data()) == "000000110001010110010110");
    SCFG_CHECK(t, num.ok() && num.value().get_num_chars() == 7);
    Result<QrSegment> alnum = QrSegment::make_alphanumeric("AC-42");
    SCFG_CHECK(t, alnum.ok() && bits(alnum.value().get_data()) == "0011100111011100111001000010");

    // A string's mode: ASCII digits only are numeric (not Arabic-Indic or fullwidth digits, as Python's [0-9]).
    SCFG_CHECK(t, QrSegment::is_numeric("") && QrSegment::is_numeric("0123"));
    SCFG_CHECK(t, !QrSegment::is_numeric("12a") && !QrSegment::is_numeric("\xD9\xA1") &&
                      !QrSegment::is_numeric("\xEF\xBC\x91"));
    SCFG_CHECK(t, QrSegment::is_alphanumeric("") && QrSegment::is_alphanumeric("AZ09 $%*+-./:"));
    SCFG_CHECK(t, !QrSegment::is_alphanumeric("a") && !QrSegment::is_alphanumeric("\xC3\x89") &&
                      !QrSegment::is_alphanumeric("A_B") && !QrSegment::is_alphanumeric("A,B"));
    Result<std::vector<QrSegment>> none = QrSegment::make_segments("");
    SCFG_CHECK(t, none.ok() && none.value().empty());
    // é and an astral character: one byte segment of 6 UTF-8 bytes.
    Result<std::vector<QrSegment>> bytes = QrSegment::make_segments("\xC3\xA9\xF0\x9F\x8E\xAE");
    SCFG_CHECK(t, bytes.ok() && bytes.value().size() == 1 && bytes.value()[0].get_mode() == QrSegment::Mode::BYTE &&
                      bytes.value()[0].get_num_chars() == 6);
    SCFG_CHECK(t, QrSegment::get_mode_bits(QrSegment::Mode::KANJI) == 8 &&
                      QrSegment::num_char_count_bits(QrSegment::Mode::KANJI, 27) == 12);
}

SCFG_TEST(QrTests, MixedSegmentsAndBinary) {
    // ECI 26 (UTF-8) + bytes("é") + numeric "0123" + alphanumeric "AB" at HIGH from version 3.
    const std::vector<uint8_t> e_acute = {0xC3, 0xA9};
    std::vector<QrSegment> segs;
    segs.push_back(QrSegment::make_eci(26).value());
    segs.push_back(QrSegment::make_bytes(e_acute));
    segs.push_back(QrSegment::make_numeric("0123").value());
    segs.push_back(QrSegment::make_alphanumeric("AB").value());
    Result<QrCode> qr = QrCode::encode_segments(segs, Ecc::HIGH, 3);
    if (SCFG_CHECK_EQ(t, outcome(qr), "ok")) {
        SCFG_CHECK(t, qr.value().get_version() == 3 && qr.value().get_error_correction_level() == Ecc::HIGH);
        SCFG_CHECK_EQ(t, qr.value().get_mask(), 3);
        SCFG_CHECK(t, hash(qr.value()) == 73542505);
    }
    std::vector<uint8_t> all(256);
    for (size_t i = 0; i < all.size(); ++i) all[i] = static_cast<uint8_t>(i);
    Result<QrCode> bin = QrCode::encode_binary(all, Ecc::MEDIUM);
    if (SCFG_CHECK_EQ(t, outcome(bin), "ok")) {
        SCFG_CHECK(t, bin.value().get_version() == 12 && bin.value().get_error_correction_level() == Ecc::MEDIUM);
        SCFG_CHECK_EQ(t, bin.value().get_mask(), 5);
        SCFG_CHECK(t, hash(bin.value()) == 257911706);
    }
}

// ---- errors (Python's exceptions, as Err values) ----

SCFG_TEST(QrTests, DataTooLong) {
    Result<QrCode> x2332 = QrCode::encode_text(repeat("x", 2332), Ecc::MEDIUM);
    SCFG_CHECK_EQ(t, outcome(x2332), "DataTooLongError: Data length = 18676 bits, Max capacity = 18672 bits");
    SCFG_CHECK(t, !x2332.ok() && x2332.err().is_a("ValueError") && x2332.err().is_a(qrcodegen::DataTooLongError));
    const std::vector<uint8_t> b2954(2954, 'x');
    SCFG_CHECK_EQ(t, outcome(QrCode::encode_binary(b2954, Ecc::LOW)),
                  "DataTooLongError: Data length = 23652 bits, Max capacity = 23648 bits");
    // A count too wide for the 16-bit length field of version 40.
    const std::vector<uint8_t> b65536(65536, 'x');
    SCFG_CHECK_EQ(t, outcome(QrCode::encode_binary(b65536, Ecc::LOW)), "DataTooLongError: Segment too long");
    SCFG_CHECK_EQ(t, outcome(QrCode::encode_text(repeat("7", 7090), Ecc::LOW)),
                  "DataTooLongError: Data length = 23652 bits, Max capacity = 23648 bits");
    Result<std::vector<QrSegment>> segs = QrSegment::make_segments(repeat("x", 15));
    SCFG_CHECK_EQ(t, outcome(QrCode::encode_segments(segs.value(), Ecc::MEDIUM, 1, 1)),
                  "DataTooLongError: Data length = 132 bits, Max capacity = 128 bits");
    // Through qr_matrix and qr_rects.
    SCFG_CHECK_EQ(t, outcome(scfg::paylinks::qr_matrix(repeat("x", 2332))),
                  "DataTooLongError: Data length = 18676 bits, Max capacity = 18672 bits");
    SCFG_CHECK_EQ(t, outcome(scfg::paylinks::qr_rects(repeat("x", 2332))),
                  "DataTooLongError: Data length = 18676 bits, Max capacity = 18672 bits");
}

SCFG_TEST(QrTests, BadArgumentsAreValueErrors) {
    const std::vector<QrSegment> none;
    for (auto r : {QrCode::encode_segments(none, Ecc::LOW, 0, 40), QrCode::encode_segments(none, Ecc::LOW, 5, 4),
                   QrCode::encode_segments(none, Ecc::LOW, 1, 41), QrCode::encode_segments(none, Ecc::LOW, 1, 40, 8),
                   QrCode::encode_segments(none, Ecc::LOW, 1, 40, -2)}) {
        SCFG_CHECK_EQ(t, outcome(r), "ValueError: Invalid value");
    }
    SCFG_CHECK_EQ(t, outcome(QrCode::make(0, Ecc::LOW, {}, 0)), "ValueError: Version value out of range");
    SCFG_CHECK_EQ(t, outcome(QrCode::make(41, Ecc::LOW, {}, 0)), "ValueError: Version value out of range");
    SCFG_CHECK_EQ(t, outcome(QrCode::make(1, Ecc::LOW, std::vector<uint8_t>(19), 8)),
                  "ValueError: Mask value out of range");
    // Python's assert on the codeword count: AssertionError with no message.
    SCFG_CHECK_EQ(t, outcome(QrCode::make(1, Ecc::LOW, std::vector<uint8_t>(18), 0)), "AssertionError: ");
    SCFG_CHECK_EQ(t, outcome(QrCode::make(1, Ecc::LOW, std::vector<uint8_t>(20), 0)), "AssertionError: ");
    SCFG_CHECK_EQ(t, outcome(QrCode::make(1, Ecc::LOW, std::vector<uint8_t>(19), -1)), "ok");
    SCFG_CHECK_EQ(t, outcome(QrSegment::make_numeric("12a")), "ValueError: String contains non-numeric characters");
    SCFG_CHECK_EQ(t, outcome(QrSegment::make_numeric("\xD9\xA1\xD9\xA2")),
                  "ValueError: String contains non-numeric characters");
    SCFG_CHECK_EQ(t, outcome(QrSegment::make_alphanumeric("abc")),
                  "ValueError: String contains unencodable characters in alphanumeric mode");
    SCFG_CHECK_EQ(t, outcome(QrSegment::make_eci(-1)), "ValueError: ECI assignment value out of range");
    SCFG_CHECK_EQ(t, outcome(QrSegment::make_eci(1000000)), "ValueError: ECI assignment value out of range");
    SCFG_CHECK_EQ(t, outcome(QrSegment::make(QrSegment::Mode::BYTE, -1, {})), "ValueError: ");
    SCFG_CHECK_EQ(t, outcome(QrSegment::make(QrSegment::Mode::BYTE, 0, {})), "ok");
    // A lone surrogate (WTF-8 from a JSON \ud800 escape): UnicodeEncodeError's message, as text.encode("UTF-8").
    Result<QrCode> lone = QrCode::encode_text("a\xED\xA0\x80", Ecc::MEDIUM);
    SCFG_CHECK_EQ(t, outcome(lone),
                  "ValueError: 'utf-8' codec can't encode character '\\ud800' in position 1: surrogates not allowed");
    SCFG_CHECK_EQ(t, outcome(scfg::paylinks::qr_matrix("a\xED\xA0\x80")), outcome(lone));
}
