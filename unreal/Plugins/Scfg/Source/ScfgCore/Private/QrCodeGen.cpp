// Copyright 2026 Repulse
// Twin of scfg_core/_qrcodegen.py (Project Nayuki's QR Code generator, ported to C++; see QrCodeGen.h).
//
// QR Code generator library (Python)
//
// Copyright (c) Project Nayuki. (MIT License)
// https://www.nayuki.io/page/qr-code-generator-library
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of
// this software and associated documentation files (the "Software"), to deal in
// the Software without restriction, including without limitation the rights to
// use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
// the Software, and to permit persons to whom the Software is furnished to do so,
// subject to the following conditions:
// - The above copyright notice and this permission notice shall be included in
//   all copies or substantial portions of the Software.
// - The Software is provided "as is", without warranty of any kind, express or
//   implied, including but not limited to the warranties of merchantability,
//   fitness for a particular purpose and noninfringement. In no event shall the
//   authors or copyright holders be liable for any claim, damages or other
//   liability, whether in an action of contract, tort or otherwise, arising from,
//   out of or in connection with the Software or the use or other dealings in the
//   Software.
#include "Scfg/QrCodeGen.h"

#include "Scfg/Err.h"
#include "Scfg/Py.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <utility>

namespace scfg::qrcodegen::detail {

// ---- Constants and tables ----

// For use in get_penalty_score(), when evaluating which mask is best.
inline constexpr int64_t PENALTY_N1 = 3;
inline constexpr int64_t PENALTY_N2 = 3;
inline constexpr int64_t PENALTY_N3 = 40;
inline constexpr int64_t PENALTY_N4 = 10;

// Indexed [ordinal][version]; index 0 is padding, set to an illegal value.
inline constexpr std::array<std::array<int8_t, 41>, 4> ECC_CODEWORDS_PER_BLOCK = {{
    // 0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27,
    // 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40    Error correction level
    {-1, 7, 10, 15, 20, 26, 18, 20, 24, 30, 18, 20, 24, 26, 30, 22, 24, 28, 30, 28, 28, 28, 28, 30, 30, 26, 28, 30,
     30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},  // Low
    {-1, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26, 30, 22, 22, 24, 24, 28, 28, 26, 26, 26, 26, 28, 28, 28, 28, 28, 28,
     28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28},  // Medium
    {-1, 13, 22, 18, 26, 18, 24, 18, 22, 20, 24, 28, 26, 24, 20, 30, 24, 28, 28, 26, 30, 28, 30, 30, 30, 30, 28, 30,
     30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},  // Quartile
    {-1, 17, 28, 22, 16, 22, 28, 26, 26, 24, 28, 24, 28, 22, 24, 24, 30, 28, 28, 26, 28, 30, 24, 30, 30, 30, 30, 30,
     30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},  // High
}};

inline constexpr std::array<std::array<int8_t, 41>, 4> NUM_ERROR_CORRECTION_BLOCKS = {{
    // 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29,
    // 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40    Error correction level
    {-1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 4, 4, 4, 4, 4, 6, 6, 6, 6, 7, 8, 8, 9, 9, 10, 12, 12, 12, 13, 14,
     15, 16, 17, 18, 19, 19, 20, 21, 22, 24, 25},  // Low
    {-1, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5, 5, 8, 9, 9, 10, 10, 11, 13, 14, 16, 17, 17, 18, 20, 21, 23, 25, 26, 28,
     29, 31, 33, 35, 37, 38, 40, 43, 45, 47, 49},  // Medium
    {-1, 1, 1, 2, 2, 4, 4, 6, 6, 8, 8, 8, 10, 12, 16, 12, 17, 16, 18, 21, 20, 23, 23, 25, 27, 29, 34, 34, 35, 38,
     40, 43, 45, 48, 51, 53, 56, 59, 62, 65, 68},  // Quartile
    {-1, 1, 1, 2, 4, 4, 4, 5, 6, 8, 8, 11, 11, 16, 16, 18, 16, 19, 21, 25, 25, 25, 34, 30, 32, 35, 37, 40, 42, 45,
     48, 51, 54, 57, 60, 63, 66, 70, 74, 77, 81},  // High
}};

// "0"->0, "A"->10, "$"->37, etc.
inline constexpr std::string_view ALPHANUMERIC_CHARSET = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:";

// ---- _BitBuffer ----

// Appends the n low-order bits of val, high bit first. Every caller passes 0 <= val < 2^n (Python's ValueError
// "Value out of range" is unreachable).
void append_bits(std::vector<bool>& bb, uint32_t val, int n) {
    for (int i = n - 1; i >= 0; --i) bb.push_back(((val >> i) & 1) != 0);
}

// True iff the i'th bit of x is set to 1.
bool get_bit(int64_t x, int i) { return ((x >> i) & 1) != 0; }

// The number of data bits that can be stored in a QR Code of the given version (1..40), after all function modules
// are excluded, remainder bits included: 208..29648.
int get_num_raw_data_modules(int ver) {
    int result = (16 * ver + 128) * ver + 64;
    if (ver >= 2) {
        int numalign = ver / 7 + 2;
        result -= (25 * numalign - 10) * numalign - 55;
        if (ver >= 7) result -= 36;
    }
    return result;
}

// The number of 8-bit data (not error correction) codewords of the given version and level.
int get_num_data_codewords(int ver, QrCode::Ecc ecl) {
    const auto o = static_cast<size_t>(QrCode::ordinal(ecl));
    const auto v = static_cast<size_t>(ver);
    return get_num_raw_data_modules(ver) / 8 - ECC_CODEWORDS_PER_BLOCK[o][v] * NUM_ERROR_CORRECTION_BLOCKS[o][v];
}

// The product of two field elements modulo GF(2^8/0x11D) (Russian peasant multiplication).
uint8_t reed_solomon_multiply(uint8_t x, uint8_t y) {
    int z = 0;
    for (int i = 7; i >= 0; --i) {
        z = (z << 1) ^ ((z >> 7) * 0x11D);
        z ^= ((y >> i) & 1) * x;
    }
    return static_cast<uint8_t>(z);
}

// The Reed-Solomon generator polynomial of the given degree (1..255; the tables give 7..30), coefficients from the
// highest to the lowest power, the leading 1 dropped.
std::vector<uint8_t> reed_solomon_compute_divisor(int degree) {
    std::vector<uint8_t> result(static_cast<size_t>(degree), 0);
    result.back() = 1;  // Start off with the monomial x^0
    // Multiply by (x - r^0) * (x - r^1) * ... * (x - r^{degree-1}), r = 0x02, a generator of GF(2^8/0x11D).
    uint8_t root = 1;
    for (int i = 0; i < degree; ++i) {
        for (size_t j = 0; j < result.size(); ++j) {
            result[j] = reed_solomon_multiply(result[j], root);
            if (j + 1 < result.size()) result[j] ^= result[j + 1];
        }
        root = reed_solomon_multiply(root, 0x02);
    }
    return result;
}

// The Reed-Solomon error correction codewords for the given data and divisor polynomials.
std::vector<uint8_t> reed_solomon_compute_remainder(std::span<const uint8_t> data,
                                                    const std::vector<uint8_t>& divisor) {
    std::vector<uint8_t> result(divisor.size(), 0);
    for (uint8_t b : data) {  // Polynomial division
        const uint8_t factor = static_cast<uint8_t>(b ^ result.front());
        result.erase(result.begin());
        result.push_back(0);
        for (size_t i = 0; i < divisor.size(); ++i) result[i] ^= reed_solomon_multiply(divisor[i], factor);
    }
    return result;
}

bool mask_pattern(int mask, int x, int y) {
    switch (mask) {
        case 0: return (x + y) % 2 == 0;
        case 1: return y % 2 == 0;
        case 2: return x % 3 == 0;
        case 3: return (x + y) % 3 == 0;
        case 4: return (x / 3 + y / 2) % 2 == 0;
        case 5: return x * y % 2 + x * y % 3 == 0;
        case 6: return (x * y % 2 + x * y % 3) % 2 == 0;
        default: return ((x + y) % 2 + x * y % 3) % 2 == 0;
    }
}

// The run history of get_penalty_score(): collections.deque([0] * 7, 7), newest first.
using RunHistory = std::array<int64_t, 7>;

void finder_penalty_add_history(int64_t currentrunlength, RunHistory& runhistory, int size) {
    if (runhistory[0] == 0) currentrunlength += size;  // Add light border to initial run
    // appendleft on a full deque of maxlen 7: the oldest (rightmost) falls off.
    for (size_t i = runhistory.size() - 1; i > 0; --i) runhistory[i] = runhistory[i - 1];
    runhistory[0] = currentrunlength;
}

// Only right after a light run is added: 0, 1 or 2.
int64_t finder_penalty_count_patterns(const RunHistory& runhistory) {
    const int64_t n = runhistory[1];
    const bool core = n > 0 && runhistory[2] == n && runhistory[4] == n && runhistory[5] == n && runhistory[3] == n * 3;
    return ((core && runhistory[0] >= n * 4 && runhistory[6] >= n) ? 1 : 0) +
           ((core && runhistory[6] >= n * 4 && runhistory[0] >= n) ? 1 : 0);
}

// At the end of a line (row or column) of modules.
int64_t finder_penalty_terminate_and_count(bool currentruncolor, int64_t currentrunlength, RunHistory& runhistory,
                                           int size) {
    if (currentruncolor) {  // Terminate dark run
        finder_penalty_add_history(currentrunlength, runhistory, size);
        currentrunlength = 0;
    }
    currentrunlength += size;  // Add light border to final run
    finder_penalty_add_history(currentrunlength, runhistory, size);
    return finder_penalty_count_patterns(runhistory);
}

}  // namespace scfg::qrcodegen::detail

namespace scfg::qrcodegen {

// ---- QrSegment ----

int QrSegment::get_mode_bits(Mode mode) {
    switch (mode) {
        case Mode::NUMERIC: return 0x1;
        case Mode::ALPHANUMERIC: return 0x2;
        case Mode::BYTE: return 0x4;
        case Mode::KANJI: return 0x8;
        case Mode::ECI: return 0x7;
    }
    return 0;
}

int QrSegment::num_char_count_bits(Mode mode, int ver) {
    static constexpr std::array<std::array<int, 3>, 5> charcounts = {{
        {10, 12, 14},  // NUMERIC
        {9, 11, 13},   // ALPHANUMERIC
        {8, 16, 16},   // BYTE
        {8, 10, 12},   // KANJI
        {0, 0, 0},     // ECI
    }};
    return charcounts[static_cast<size_t>(mode)][static_cast<size_t>((ver + 7) / 17)];
}

QrSegment::QrSegment(Mode mode, int64_t numch, std::vector<bool> bitdata)
    : mode_(mode), numchars_(numch), bitdata_(std::move(bitdata)) {}

Result<QrSegment> QrSegment::make(Mode mode, int64_t numch, std::vector<bool> bitdata) {
    if (numch < 0) return Err::value_error("");
    return QrSegment(mode, numch, std::move(bitdata));
}

QrSegment QrSegment::make_bytes(std::span<const uint8_t> data) {
    std::vector<bool> bb;
    bb.reserve(data.size() * 8);
    for (uint8_t b : data) detail::append_bits(bb, b, 8);
    return QrSegment(Mode::BYTE, static_cast<int64_t>(data.size()), std::move(bb));
}

Result<QrSegment> QrSegment::make_numeric(std::string_view digits) {
    if (!is_numeric(digits)) return Err::value_error("String contains non-numeric characters");
    std::vector<bool> bb;
    size_t i = 0;
    while (i < digits.size()) {  // Consume up to 3 digits per iteration
        const size_t n = std::min<size_t>(digits.size() - i, 3);
        uint32_t value = 0;
        for (size_t k = i; k < i + n; ++k) value = value * 10 + static_cast<uint32_t>(digits[k] - '0');
        detail::append_bits(bb, value, static_cast<int>(n * 3 + 1));
        i += n;
    }
    return QrSegment(Mode::NUMERIC, static_cast<int64_t>(digits.size()), std::move(bb));
}

Result<QrSegment> QrSegment::make_alphanumeric(std::string_view text) {
    if (!is_alphanumeric(text)) return Err::value_error("String contains unencodable characters in alphanumeric mode");
    auto code = [&text](size_t i) { return static_cast<uint32_t>(detail::ALPHANUMERIC_CHARSET.find(text[i])); };
    std::vector<bool> bb;
    for (size_t i = 0; i + 1 < text.size(); i += 2) {  // Process groups of 2
        detail::append_bits(bb, code(i) * 45 + code(i + 1), 11);
    }
    if (text.size() % 2 > 0) detail::append_bits(bb, code(text.size() - 1), 6);  // 1 character remaining
    return QrSegment(Mode::ALPHANUMERIC, static_cast<int64_t>(text.size()), std::move(bb));
}

Result<std::vector<QrSegment>> QrSegment::make_segments(std::string_view text) {
    // Select the most efficient segment encoding automatically
    std::vector<QrSegment> segs;
    if (text.empty()) return segs;
    if (is_numeric(text)) {
        SCFG_TRY(QrSegment seg, make_numeric(text));
        segs.push_back(std::move(seg));
    } else if (is_alphanumeric(text)) {
        SCFG_TRY(QrSegment seg, make_alphanumeric(text));
        segs.push_back(std::move(seg));
    } else {
        SCFG_TRY(std::vector<uint8_t> raw, py::utf8_bytes(text));
        segs.push_back(make_bytes(raw));
    }
    return segs;
}

Result<QrSegment> QrSegment::make_eci(int64_t assignval) {
    std::vector<bool> bb;
    if (assignval < 0 || assignval >= 1000000) return Err::value_error("ECI assignment value out of range");
    const auto v = static_cast<uint32_t>(assignval);
    if (assignval < (1 << 7)) {
        detail::append_bits(bb, v, 8);
    } else if (assignval < (1 << 14)) {
        detail::append_bits(bb, 0b10, 2);
        detail::append_bits(bb, v, 14);
    } else {
        detail::append_bits(bb, 0b110, 3);
        detail::append_bits(bb, v, 21);
    }
    return QrSegment(Mode::ECI, 0, std::move(bb));
}

bool QrSegment::is_numeric(std::string_view text) {
    return std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

bool QrSegment::is_alphanumeric(std::string_view text) {
    // A non-ASCII byte is never in the set, so a multi-byte character fails as in the Python regex.
    return std::all_of(text.begin(), text.end(),
                       [](char c) { return detail::ALPHANUMERIC_CHARSET.find(c) != std::string_view::npos; });
}

std::optional<int64_t> QrSegment::get_total_bits(const std::vector<QrSegment>& segs, int version) {
    int64_t result = 0;
    for (const QrSegment& seg : segs) {
        const int ccbits = num_char_count_bits(seg.mode_, version);
        if (seg.numchars_ >= (int64_t{1} << ccbits)) return std::nullopt;  // the count does not fit its field
        result += 4 + ccbits + static_cast<int64_t>(seg.bitdata_.size());
    }
    return result;
}

// ---- QrCode: factories ----

int QrCode::formatbits(Ecc ecl) {
    switch (ecl) {
        case Ecc::LOW: return 1;
        case Ecc::MEDIUM: return 0;
        case Ecc::QUARTILE: return 3;
        case Ecc::HIGH: return 2;
    }
    return 0;
}

Result<QrCode> QrCode::encode_text(std::string_view text, Ecc ecl) {
    SCFG_TRY(std::vector<QrSegment> segs, QrSegment::make_segments(text));
    return encode_segments(segs, ecl);
}

Result<QrCode> QrCode::encode_binary(std::span<const uint8_t> data, Ecc ecl) {
    return encode_segments({QrSegment::make_bytes(data)}, ecl);
}

Result<QrCode> QrCode::encode_segments(const std::vector<QrSegment>& segs, Ecc ecl, int minversion, int maxversion,
                                       int mask, bool boostecl) {
    if (!(MIN_VERSION <= minversion && minversion <= maxversion && maxversion <= MAX_VERSION) ||
        !(-1 <= mask && mask <= 7)) {
        return Err::value_error("Invalid value");
    }

    // Find the minimal version number to use
    int version = minversion;
    int64_t datausedbits = 0;
    for (;; ++version) {
        const int64_t datacapacitybits = int64_t{detail::get_num_data_codewords(version, ecl)} * 8;
        const std::optional<int64_t> used = QrSegment::get_total_bits(segs, version);
        if (used && *used <= datacapacitybits) {
            datausedbits = *used;
            break;  // This version number is found to be suitable
        }
        if (version >= maxversion) {  // All versions in the range could not fit the given data
            std::string msg = "Segment too long";
            if (used) msg = "Data length = " + py::dec(*used) + " bits, Max capacity = " +
                            py::dec(datacapacitybits) + " bits";
            return Err::make(std::string(DataTooLongError), std::move(msg));
        }
    }

    // Increase the error correction level while the data still fits in the current version number
    for (Ecc newecl : {Ecc::MEDIUM, Ecc::QUARTILE, Ecc::HIGH}) {  // From low to high
        if (boostecl && datausedbits <= int64_t{detail::get_num_data_codewords(version, newecl)} * 8) ecl = newecl;
    }

    // Concatenate all segments to create the data bit string
    std::vector<bool> bb;
    for (const QrSegment& seg : segs) {
        detail::append_bits(bb, static_cast<uint32_t>(QrSegment::get_mode_bits(seg.mode_)), 4);
        detail::append_bits(bb, static_cast<uint32_t>(seg.numchars_),
                            QrSegment::num_char_count_bits(seg.mode_, version));
        bb.insert(bb.end(), seg.bitdata_.begin(), seg.bitdata_.end());
    }

    // Add terminator and pad up to a byte if applicable
    const auto datacapacitybits = static_cast<size_t>(detail::get_num_data_codewords(version, ecl)) * 8;
    detail::append_bits(bb, 0, static_cast<int>(std::min<size_t>(4, datacapacitybits - bb.size())));
    detail::append_bits(bb, 0, static_cast<int>((8 - bb.size() % 8) % 8));

    // Pad with alternating bytes until data capacity is reached
    for (uint32_t padbyte = 0xEC; bb.size() < datacapacitybits; padbyte ^= 0xEC ^ 0x11) {
        detail::append_bits(bb, padbyte, 8);
    }

    // Pack bits into bytes in big endian
    std::vector<uint8_t> datacodewords(bb.size() / 8, 0);
    for (size_t i = 0; i < bb.size(); ++i) {
        if (bb[i]) datacodewords[i >> 3] = static_cast<uint8_t>(datacodewords[i >> 3] | (1u << (7 - (i & 7))));
    }

    // Create the QR Code object
    return make(version, ecl, datacodewords, mask);
}

// ---- QrCode: constructor ----

QrCode::QrCode(int version, Ecc errcorlvl)
    : version_(version),
      size_(version * 4 + 17),
      errcorlvl_(errcorlvl),
      // Initially all light, and no function module
      modules_(static_cast<size_t>(size_) * static_cast<size_t>(size_), 0),
      isfunction_(modules_.size(), 0) {}

Result<QrCode> QrCode::make(int version, Ecc errcorlvl, std::span<const uint8_t> datacodewords, int msk) {
    // Check scalar arguments and set fields
    if (!(MIN_VERSION <= version && version <= MAX_VERSION)) return Err::value_error("Version value out of range");
    if (!(-1 <= msk && msk <= 7)) return Err::value_error("Mask value out of range");
    // Python's `assert len(data) == QrCode._get_num_data_codewords(...)` in _add_ecc_and_interleave.
    if (datacodewords.size() != static_cast<size_t>(detail::get_num_data_codewords(version, errcorlvl))) {
        return Err::make("AssertionError", "");
    }

    QrCode qr(version, errcorlvl);
    // Compute ECC, draw modules
    qr.draw_function_patterns();
    const std::vector<uint8_t> allcodewords = qr.add_ecc_and_interleave(datacodewords);
    qr.draw_codewords(allcodewords);

    // Do masking
    if (msk == -1) {  // Automatically choose best mask
        int64_t minpenalty = int64_t{1} << 32;
        for (int i = 0; i < 8; ++i) {
            qr.apply_mask(i);
            qr.draw_format_bits(i);
            const int64_t penalty = qr.get_penalty_score();
            if (penalty < minpenalty) {
                msk = i;
                minpenalty = penalty;
            }
            qr.apply_mask(i);  // Undoes the mask due to XOR
        }
    }
    qr.mask_ = msk;
    qr.apply_mask(msk);        // Apply the final choice of mask
    qr.draw_format_bits(msk);  // Overwrite old format bits

    qr.isfunction_.clear();
    qr.isfunction_.shrink_to_fit();
    return qr;
}

bool QrCode::get_module(int x, int y) const {
    return 0 <= x && x < size_ && 0 <= y && y < size_ && modules_[at(x, y)] != 0;
}

// ---- QrCode: drawing function modules ----

void QrCode::draw_function_patterns() {
    // Draw horizontal and vertical timing patterns
    for (int i = 0; i < size_; ++i) {
        set_function_module(6, i, i % 2 == 0);
        set_function_module(i, 6, i % 2 == 0);
    }

    // Draw 3 finder patterns (all corners except bottom right; overwrites some timing modules)
    draw_finder_pattern(3, 3);
    draw_finder_pattern(size_ - 4, 3);
    draw_finder_pattern(3, size_ - 4);

    // Draw numerous alignment patterns
    const std::vector<int> alignpatpos = get_alignment_pattern_positions();
    const size_t numalign = alignpatpos.size();
    for (size_t i = 0; i < numalign; ++i) {
        for (size_t j = 0; j < numalign; ++j) {
            // Don't draw on the three finder corners
            const bool skip = (i == 0 && j == 0) || (i == 0 && j == numalign - 1) || (i == numalign - 1 && j == 0);
            if (!skip) draw_alignment_pattern(alignpatpos[i], alignpatpos[j]);
        }
    }

    // Draw configuration data
    draw_format_bits(0);  // Dummy mask value; overwritten later in the constructor
    draw_version();
}

void QrCode::draw_format_bits(int mask) {
    // Calculate error correction code and pack bits
    const int data = formatbits(errcorlvl_) << 3 | mask;  // errCorrLvl is uint2, mask is uint3
    int rem = data;
    for (int i = 0; i < 10; ++i) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    const int bits = (data << 10 | rem) ^ 0x5412;  // uint15

    // Draw first copy
    for (int i = 0; i < 6; ++i) set_function_module(8, i, detail::get_bit(bits, i));
    set_function_module(8, 7, detail::get_bit(bits, 6));
    set_function_module(8, 8, detail::get_bit(bits, 7));
    set_function_module(7, 8, detail::get_bit(bits, 8));
    for (int i = 9; i < 15; ++i) set_function_module(14 - i, 8, detail::get_bit(bits, i));

    // Draw second copy
    for (int i = 0; i < 8; ++i) set_function_module(size_ - 1 - i, 8, detail::get_bit(bits, i));
    for (int i = 8; i < 15; ++i) set_function_module(8, size_ - 15 + i, detail::get_bit(bits, i));
    set_function_module(8, size_ - 8, true);  // Always dark
}

void QrCode::draw_version() {
    if (version_ < 7) return;

    // Calculate error correction code and pack bits
    int rem = version_;  // version is uint6, in the range [7, 40]
    for (int i = 0; i < 12; ++i) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
    const int64_t bits = int64_t{version_} << 12 | rem;  // uint18

    // Draw two copies
    for (int i = 0; i < 18; ++i) {
        const bool bit = detail::get_bit(bits, i);
        const int a = size_ - 11 + i % 3;
        const int b = i / 3;
        set_function_module(a, b, bit);
        set_function_module(b, a, bit);
    }
}

void QrCode::draw_finder_pattern(int x, int y) {
    // A 9*9 finder pattern including the border separator, centred at (x, y); modules can be out of bounds.
    for (int dy = -4; dy <= 4; ++dy) {
        for (int dx = -4; dx <= 4; ++dx) {
            const int xx = x + dx, yy = y + dy;
            if (0 <= xx && xx < size_ && 0 <= yy && yy < size_) {
                const int dist = std::max(dx < 0 ? -dx : dx, dy < 0 ? -dy : dy);  // Chebyshev/infinity norm
                set_function_module(xx, yy, dist != 2 && dist != 4);
            }
        }
    }
}

void QrCode::draw_alignment_pattern(int x, int y) {
    // A 5*5 alignment pattern centred at (x, y); all modules in bounds.
    for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
            set_function_module(x + dx, y + dy, std::max(dx < 0 ? -dx : dx, dy < 0 ? -dy : dy) != 1);
        }
    }
}

void QrCode::set_function_module(int x, int y, bool isdark) {
    modules_[at(x, y)] = isdark ? 1 : 0;
    isfunction_[at(x, y)] = 1;
}

// ---- QrCode: codewords and masking ----

std::vector<uint8_t> QrCode::add_ecc_and_interleave(std::span<const uint8_t> data) const {
    // Calculate parameter numbers
    const auto o = static_cast<size_t>(ordinal(errcorlvl_));
    const auto v = static_cast<size_t>(version_);
    const auto numblocks = static_cast<size_t>(detail::NUM_ERROR_CORRECTION_BLOCKS[o][v]);
    const auto blockecclen = static_cast<size_t>(detail::ECC_CODEWORDS_PER_BLOCK[o][v]);
    const auto rawcodewords = static_cast<size_t>(detail::get_num_raw_data_modules(version_) / 8);
    const size_t numshortblocks = numblocks - rawcodewords % numblocks;
    const size_t shortblocklen = rawcodewords / numblocks;

    // Split data into blocks and append ECC to each block
    std::vector<std::vector<uint8_t>> blocks;
    const std::vector<uint8_t> rsdiv = detail::reed_solomon_compute_divisor(static_cast<int>(blockecclen));
    size_t k = 0;
    for (size_t i = 0; i < numblocks; ++i) {
        const size_t datlen = shortblocklen - blockecclen + (i < numshortblocks ? 0 : 1);
        std::vector<uint8_t> dat(data.begin() + static_cast<std::ptrdiff_t>(k),
                                 data.begin() + static_cast<std::ptrdiff_t>(k + datlen));
        k += datlen;
        const std::vector<uint8_t> ecc = detail::reed_solomon_compute_remainder(dat, rsdiv);
        if (i < numshortblocks) dat.push_back(0);
        dat.insert(dat.end(), ecc.begin(), ecc.end());
        blocks.push_back(std::move(dat));
    }

    // Interleave (not concatenate) the bytes from every block into a single sequence
    std::vector<uint8_t> result;
    result.reserve(rawcodewords);
    for (size_t i = 0; i < blocks[0].size(); ++i) {
        for (size_t j = 0; j < blocks.size(); ++j) {
            // Skip the padding byte in short blocks
            if (i != shortblocklen - blockecclen || j >= numshortblocks) result.push_back(blocks[j][i]);
        }
    }
    return result;
}

void QrCode::draw_codewords(std::span<const uint8_t> data) {
    size_t i = 0;  // Bit index into the data
    const size_t nbits = data.size() * 8;
    // Do the funny zigzag scan. Python's `right -= 1` changes only this pass, not the range it iterates.
    for (int column = size_ - 1; column >= 1; column -= 2) {  // Index of right column in each column pair
        const int right = column <= 6 ? column - 1 : column;
        for (int vert = 0; vert < size_; ++vert) {  // Vertical counter
            for (int j = 0; j < 2; ++j) {
                const int x = right - j;  // Actual x coordinate
                const bool upward = ((right + 1) & 2) == 0;
                const int y = upward ? size_ - 1 - vert : vert;  // Actual y coordinate
                if (isfunction_[at(x, y)] == 0 && i < nbits) {
                    modules_[at(x, y)] = detail::get_bit(data[i >> 3], static_cast<int>(7 - (i & 7))) ? 1 : 0;
                    ++i;
                }
                // If this QR Code has any remainder bits (0 to 7), they were assigned as
                // 0/false/light by the constructor and are left unchanged by this method
            }
        }
    }
}

void QrCode::apply_mask(int mask) {
    // XOR, so applying the same mask twice undoes it.
    for (int y = 0; y < size_; ++y) {
        for (int x = 0; x < size_; ++x) {
            if (detail::mask_pattern(mask, x, y) && isfunction_[at(x, y)] == 0) modules_[at(x, y)] ^= 1;
        }
    }
}

int64_t QrCode::get_penalty_score() const {
    int64_t result = 0;
    const int size = size_;
    auto m = [this](int x, int y) { return modules_[at(x, y)] != 0; };

    // Adjacent modules in row having same color, and finder-like patterns
    for (int y = 0; y < size; ++y) {
        bool runcolor = false;
        int64_t runx = 0;
        detail::RunHistory runhistory{};
        for (int x = 0; x < size; ++x) {
            if (m(x, y) == runcolor) {
                ++runx;
                if (runx == 5) {
                    result += detail::PENALTY_N1;
                } else if (runx > 5) {
                    result += 1;
                }
            } else {
                detail::finder_penalty_add_history(runx, runhistory, size);
                if (!runcolor) result += detail::finder_penalty_count_patterns(runhistory) * detail::PENALTY_N3;
                runcolor = m(x, y);
                runx = 1;
            }
        }
        result += detail::finder_penalty_terminate_and_count(runcolor, runx, runhistory, size) * detail::PENALTY_N3;
    }
    // Adjacent modules in column having same color, and finder-like patterns
    for (int x = 0; x < size; ++x) {
        bool runcolor = false;
        int64_t runy = 0;
        detail::RunHistory runhistory{};
        for (int y = 0; y < size; ++y) {
            if (m(x, y) == runcolor) {
                ++runy;
                if (runy == 5) {
                    result += detail::PENALTY_N1;
                } else if (runy > 5) {
                    result += 1;
                }
            } else {
                detail::finder_penalty_add_history(runy, runhistory, size);
                if (!runcolor) result += detail::finder_penalty_count_patterns(runhistory) * detail::PENALTY_N3;
                runcolor = m(x, y);
                runy = 1;
            }
        }
        result += detail::finder_penalty_terminate_and_count(runcolor, runy, runhistory, size) * detail::PENALTY_N3;
    }

    // 2*2 blocks of modules having same color
    for (int y = 0; y < size - 1; ++y) {
        for (int x = 0; x < size - 1; ++x) {
            const bool c = m(x, y);
            if (c == m(x + 1, y) && c == m(x, y + 1) && c == m(x + 1, y + 1)) result += detail::PENALTY_N2;
        }
    }

    // Balance of dark and light modules
    const auto dark = static_cast<int64_t>(std::count(modules_.begin(), modules_.end(), uint8_t{1}));
    const int64_t total = int64_t{size} * size;  // Note that size is odd, so dark/total != 1/2
    // Compute the smallest integer k >= 0 such that (45-5k)% <= dark/total <= (55+5k)%
    const int64_t diff = dark * 20 - total * 10;
    const int64_t k = ((diff < 0 ? -diff : diff) + total - 1) / total - 1;
    result += k * detail::PENALTY_N4;
    return result;
}

// ---- QrCode: helpers ----

std::vector<int> QrCode::get_alignment_pattern_positions() const {
    // The ascending positions of the alignment patterns for this version, used on both axes.
    if (version_ == 1) return {};
    const int numalign = version_ / 7 + 2;
    const int step = (version_ * 8 + numalign * 3 + 5) / (numalign * 4 - 4) * 2;
    std::vector<int> result;
    for (int i = 0; i < numalign - 1; ++i) result.push_back(size_ - 7 - i * step);
    result.push_back(6);
    std::reverse(result.begin(), result.end());
    return result;
}

}  // namespace scfg::qrcodegen
