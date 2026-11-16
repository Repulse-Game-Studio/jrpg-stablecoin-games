// No Python twin: tests of scfg::paylinks and scfg::solana::paylinks beyond the tier1/paylinks.json and qr.json
// vectors: astral characters and lone surrogates in a Solana Pay label or message, the characters quote(v, safe='')
// encodes, negative and huge units, str()'s 4300-digit limit on an EIP-681 int (py::int_str), and qr_rects' runs.
// Every expectation is scfg_core's own output, computed with the project's interpreter (`uv run python`, CPython
// 3.9.25, from the repository root), e.g.
//   solana_pay_uri(recipient=W, units=4993742, mint=M, reference=R, label="\U0001d11e\U0001f3ae", message=...)
//   eip681_transfer_uri(token_address="T", chain_id=1, recipient="R", units=2**14285)   # ValueError
#include "support/Test.h"

#include "Scfg/BigInt.h"
#include "Scfg/Paylinks.h"
#include "Scfg/Py.h"
#include "Scfg/Result.h"
#include "Scfg/SolanaPaylinks.h"

#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

using scfg::BigInt;
using scfg::Result;
namespace paylinks = scfg::paylinks;
namespace py = scfg::py;

template <class T>
static std::string outcome(const Result<T>& r) {
    if (r.ok()) return "ok";
    return r.err().kind + ": " + r.err().message;
}

static const std::string W = "7xKXtg2CW87d97TXJSDpbD5jBkheTqA83TZRuJosgAsU";
static const std::string M = "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v";
static const std::string R = "82ZJ7nbGpixjeDCmEhUcmwXYfvurzAgGdtSMuHnUgyny";
static const std::string kHead = "solana:" + W + "?amount=4.993742&spl-token=" + M + "&reference=" + R;
static const std::string kLimit =
    "ValueError: Exceeds the limit (4300) for integer string conversion; use sys.set_int_max_str_digits() to "
    "increase the limit";

static Result<std::string> pay(std::string_view label, std::string_view message, BigInt units = BigInt(4993742),
                               std::string_view recipient = W) {
    return scfg::solana::paylinks::solana_pay_uri(recipient, units, M, R, label, message);
}

static std::string text_or_error(const Result<std::string>& r) { return r.ok() ? r.value() : outcome(r); }

SCFG_TEST(PaylinksTests, AstralCharactersInASolanaPayLabelAndMessage) {
    // U+1D11E MUSICAL SYMBOL G CLEF, U+1F3AE, U+10FFFF (the last code point), a ZWJ family sequence.
    SCFG_CHECK_EQ(t, text_or_error(pay("\xF0\x9D\x84\x9E\xF0\x9F\x8E\xAE",
                                       "\xF4\x8F\xBF\xBF end \xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D"
                                       "\xF0\x9F\x91\xA7")),
                  kHead + "&label=%F0%9D%84%9E%F0%9F%8E%AE&message=%F4%8F%BF%BF%20end%20%F0%9F%91%A8%E2%80%8D%F0%9F%91"
                          "%A9%E2%80%8D%F0%9F%91%A7");
    // A regional-indicator flag and a CJK Extension B character (U+20000) after a BMP one.
    SCFG_CHECK_EQ(t, text_or_error(pay("\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8 flag", "\xC3\xA9\xF0\xA0\x80\x80")),
                  kHead + "&label=%F0%9F%87%BA%F0%9F%87%B8%20flag&message=%C3%A9%F0%A0%80%80");
}

SCFG_TEST(PaylinksTests, LoneSurrogatesAreUnicodeEncodeErrors) {
    // quote() encodes with errors='strict'. Positions count code points.
    SCFG_CHECK_EQ(t, outcome(pay("a\xED\xA0\x80", "")),
                  "ValueError: 'utf-8' codec can't encode character '\\ud800' in position 1: surrogates not allowed");
    SCFG_CHECK_EQ(t, outcome(pay("", "x\xED\xBF\xBF")),
                  "ValueError: 'utf-8' codec can't encode character '\\udfff' in position 1: surrogates not allowed");
    // The recipient is not quoted: the surrogate stays in the str (WTF-8 here).
    SCFG_CHECK_EQ(t, text_or_error(pay("", "", BigInt(4993742), "r\xED\xA0\x80")),
                  "solana:r\xED\xA0\x80?amount=4.993742&spl-token=" + M + "&reference=" + R);
}

SCFG_TEST(PaylinksTests, SolanaPayQuotingAndEmptyValues) {
    // safe='': "/" is encoded too; only A-Za-z0-9_.-~ stay.
    SCFG_CHECK_EQ(t, text_or_error(pay("/?&=#+%:@ ~._-", "")),
                  kHead + "&label=%2F%3F%26%3D%23%2B%25%3A%40%20~._-");
    // An empty value is left out with its key, whichever it is.
    SCFG_CHECK_EQ(t, text_or_error(scfg::solana::paylinks::solana_pay_uri(W, BigInt(4993742), "", "", "", "")),
                  "solana:" + W + "?amount=4.993742");
    // format_units: negative and huge units, exactly.
    SCFG_CHECK_EQ(t, text_or_error(pay("", "", BigInt(-5))),
                  "solana:" + W + "?amount=-0.000005&spl-token=" + M + "&reference=" + R);
    SCFG_CHECK_EQ(t, text_or_error(pay("", "", BigInt(10).pow(30))),
                  "solana:" + W + "?amount=1000000000000000000000000.000000&spl-token=" + M + "&reference=" + R);
}

SCFG_TEST(PaylinksTests, Eip681FormatsIntsExactly) {
    SCFG_CHECK_EQ(t, text_or_error(paylinks::eip681_transfer_uri("T", BigInt(2).pow(70), "R", BigInt(-7))),
                  "ethereum:T@1180591620717411303424/transfer?address=R&uint256=-7");
    // str()'s limit: 4300 digits (the sign not counted) are fine, 4301 are a ValueError.
    const BigInt big = BigInt(10).pow(4300);
    Result<std::string> most = paylinks::eip681_transfer_uri("T", BigInt(1), "R", -(big - BigInt(1)));
    SCFG_CHECK(t, most.ok() && most.value().size() == 4341);
    SCFG_CHECK_EQ(t, outcome(paylinks::eip681_transfer_uri("T", big, "R", BigInt(1))), kLimit);
    SCFG_CHECK_EQ(t, outcome(paylinks::eip681_transfer_uri("T", BigInt(1), "R", BigInt(2).pow(14285))), kLimit);
    Result<std::string> bits14284 = paylinks::eip681_transfer_uri("T", BigInt(1), "R", BigInt(2).pow(14284));
    SCFG_CHECK(t, bits14284.ok() && bits14284.value().size() == 4340);
}

SCFG_TEST(PaylinksTests, IntStr) {
    const BigInt big = BigInt(10).pow(4300);
    SCFG_CHECK_EQ(t, text_or_error(py::int_str(BigInt(0))), "0");
    SCFG_CHECK_EQ(t, text_or_error(py::int_str(BigInt(-12))), "-12");
    Result<std::string> nines = py::int_str(big - BigInt(1));
    SCFG_CHECK(t, nines.ok() && nines.value().size() == 4300);
    SCFG_CHECK_EQ(t, outcome(py::int_str(big)), kLimit);
    SCFG_CHECK_EQ(t, outcome(py::int_str(-big)), kLimit);
    Result<std::string> neg = py::int_str(-(big - BigInt(1)));
    SCFG_CHECK(t, neg.ok() && neg.value().size() == 4301);
    // Refused from the bit length alone, however large.
    SCFG_CHECK_EQ(t, outcome(py::int_str(BigInt(1) << 1000000)), kLimit);
}

SCFG_TEST(PaylinksTests, QrRectsCoverExactlyTheDarkModules) {
    const std::string text = "ethereum:0xaf88d065e77c8cC2239327C5EDb3A432268e5831@42161/transfer?uint256=1";
    Result<std::vector<std::vector<bool>>> m = paylinks::qr_matrix(text);
    Result<paylinks::QrRects> rects = paylinks::qr_rects(text);
    if (!SCFG_CHECK(t, m.ok() && rects.ok())) return;
    const auto n = static_cast<int64_t>(m.value().size());
    auto [size, runs] = rects.value();
    SCFG_CHECK_EQ(t, size, n + 4);
    std::set<std::pair<int64_t, int64_t>> painted, dark;
    bool merged = true;
    for (auto [x, y, w] : runs) {
        for (int64_t dx = 0; dx < w; ++dx) painted.insert({x + dx - 2, y - 2});
        // Runs are maximal: the modules just before and after are light.
        const auto& row = m.value()[static_cast<size_t>(y - 2)];
        if (x - 2 > 0 && row[static_cast<size_t>(x - 3)]) merged = false;
        if (x - 2 + w < n && row[static_cast<size_t>(x - 2 + w)]) merged = false;
    }
    for (int64_t y = 0; y < n; ++y) {
        for (int64_t x = 0; x < n; ++x) {
            if (m.value()[static_cast<size_t>(y)][static_cast<size_t>(x)]) dark.insert({x, y});
        }
    }
    SCFG_CHECK(t, painted == dark);
    SCFG_CHECK(t, merged);
    SCFG_CHECK(t, runs.size() < dark.size());
}

SCFG_TEST(PaylinksTests, QrRectsBorder) {
    // qr_rects("hello", border=0) and border=-1, from Python.
    Result<paylinks::QrRects> zero = paylinks::qr_rects("hello", 0);
    if (SCFG_CHECK(t, zero.ok())) {
        SCFG_CHECK_EQ(t, zero.value().size, 21);
        SCFG_CHECK_EQ(t, zero.value().runs.size(), 110);
        const std::vector<paylinks::QrRun> first = {{0, 0, 7}, {12, 0, 1}, {14, 0, 7}, {0, 1, 1}, {6, 1, 1}};
        SCFG_CHECK(t, std::vector<paylinks::QrRun>(zero.value().runs.begin(), zero.value().runs.begin() + 5) == first);
    }
    Result<paylinks::QrRects> neg = paylinks::qr_rects("hello", -1);
    if (SCFG_CHECK(t, neg.ok())) {
        SCFG_CHECK_EQ(t, neg.value().size, 19);
        const std::vector<paylinks::QrRun> first = {{-1, -1, 7}, {11, -1, 1}, {13, -1, 7}};
        SCFG_CHECK(t, std::vector<paylinks::QrRun>(neg.value().runs.begin(), neg.value().runs.begin() + 3) == first);
    }
}
