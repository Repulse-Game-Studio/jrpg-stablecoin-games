// Copyright 2026 Repulse
// Twin of scfg_core/_qrcodegen.py (Project Nayuki's QR Code generator, vendored; and of unity/.../Core/QrCodeGen.cs).
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
//
// Ported to C++ for the SCFG core from the Python version above, not from Nayuki's own C++ version (which throws).
// The whole public API is here (unlike the C# twin, which keeps only what qr_matrix reaches): encode_text,
// encode_binary, encode_segments with all its parameters, the low-level QrCode constructor (QrCode::make), the
// accessors, and every QrSegment factory. Namespace scfg::qrcodegen (the module's leading underscore dropped).
//
// Errors are values. Every exception the Python raises from a public entry point is an Err with its message:
// DataTooLongError (a ValueError) when the data fits no version in the range, ValueError for a bad argument, and
// UnicodeEncodeError's ValueError for text with a lone surrogate. A wrong number of data codewords passed to
// QrCode::make is the AssertionError Python's `assert` raises there (empty message). The other checks of the Python
// helpers (_BitBuffer.append_bits, _reed_solomon_*, _get_num_raw_data_modules, _apply_mask) cannot fail from any
// public entry point, and the C++ types (uint8_t bytes, bool bits, enum levels and modes) cannot hold the values
// they refuse, so those helpers do not return a Result. The other `assert`s are invariants and are not repeated.
#pragma once

#include "Scfg/Export.h"
#include "Scfg/Result.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace scfg::qrcodegen {

// The Err kind raised when the supplied data does not fit any QR Code version (a ValueError): e.is_a(DataTooLongError).
inline constexpr std::string_view DataTooLongError = "DataTooLongError";

// ---- Data segment class ----

// A segment of character/binary/control data in a QR Code symbol. Immutable.
class SCFGCORE_API QrSegment {
public:
    // Describes how a segment's data bits are interpreted.
    enum class Mode { NUMERIC, ALPHANUMERIC, BYTE, KANJI, ECI };

    // Mode.get_mode_bits(): the 4-bit mode indicator (1, 2, 4, 8, 7).
    static int get_mode_bits(Mode mode);
    // Mode.num_char_count_bits(ver): the bit width of the character count field at version ver (1..40).
    static int num_char_count_bits(Mode mode, int ver);

    // ---- Static factory functions (mid level) ----

    // The given binary data in byte mode.
    static QrSegment make_bytes(std::span<const uint8_t> data);
    // A string of decimal digits in numeric mode; ValueError("String contains non-numeric characters") otherwise.
    static Result<QrSegment> make_numeric(std::string_view digits);
    // Text in alphanumeric mode (0-9, A-Z, space, $ % * + - . / :); ValueError("String contains unencodable
    // characters in alphanumeric mode") otherwise.
    static Result<QrSegment> make_alphanumeric(std::string_view text);
    // Zero or one segment for the text: none for "", numeric, alphanumeric, or else its UTF-8 bytes (a lone
    // surrogate is UnicodeEncodeError's ValueError).
    static Result<std::vector<QrSegment>> make_segments(std::string_view text);
    // An Extended Channel Interpretation designator; ValueError("ECI assignment value out of range") outside
    // 0 <= assignval < 1000000.
    static Result<QrSegment> make_eci(int64_t assignval);

    // Every character is 0-9 (ASCII only, as the Python regex [0-9]*).
    static bool is_numeric(std::string_view text);
    // Every character is one of the 45 alphanumeric-mode characters.
    static bool is_alphanumeric(std::string_view text);

    // ---- Constructor (low level) ----

    // QrSegment(mode, numch, bitdata): ValueError (empty message) for a negative numch. The count is not checked
    // against the data, as in Python. Bits are bools here (Python accepts any ints and fails later on other values).
    static Result<QrSegment> make(Mode mode, int64_t numch, std::vector<bool> bitdata);

    // ---- Accessor methods ----

    Mode get_mode() const { return mode_; }
    int64_t get_num_chars() const { return numchars_; }
    std::vector<bool> get_data() const { return bitdata_; }  // a copy

    // The number of bits needed to encode the segments at the given version, or nullopt when a segment has too many
    // characters for its length field.
    static std::optional<int64_t> get_total_bits(const std::vector<QrSegment>& segs, int version);

private:
    QrSegment(Mode mode, int64_t numch, std::vector<bool> bitdata);

    Mode mode_;
    int64_t numchars_;
    std::vector<bool> bitdata_;

    friend class QrCode;  // reads bitdata_, as encode_segments reads seg._bitdata
};

// ---- QR Code symbol class ----

// A QR Code symbol (QR Code Model 2, ISO/IEC 18004): an immutable square grid of dark and light modules, versions
// 1 to 40, four error correction levels, numeric/alphanumeric/byte/ECI segments.
class SCFGCORE_API QrCode {
public:
    // The error correction level; the enumerator's value is Python's Ecc.ordinal.
    enum class Ecc {
        LOW = 0,       // tolerates about  7% erroneous codewords
        MEDIUM = 1,    // about 15%
        QUARTILE = 2,  // about 25%
        HIGH = 3,      // about 30%
    };
    static int ordinal(Ecc ecl) { return static_cast<int>(ecl); }
    // Ecc.formatbits: LOW 1, MEDIUM 0, QUARTILE 3, HIGH 2.
    static int formatbits(Ecc ecl);

    static constexpr int MIN_VERSION = 1;
    static constexpr int MAX_VERSION = 40;

    // ---- Static factory functions (high level) ----

    // The text at the given level (boosted when the version does not grow), with the smallest version that fits and
    // the best mask. DataTooLongError when it fits no version; a lone surrogate is UnicodeEncodeError's ValueError.
    static Result<QrCode> encode_text(std::string_view text, Ecc ecl);
    // The binary data in byte mode (at most 2953 bytes).
    static Result<QrCode> encode_binary(std::span<const uint8_t> data, Ecc ecl);

    // ---- Static factory function (mid level) ----

    // The segments at the smallest version in [minversion, maxversion] that fits; the level is boosted iff boostecl;
    // mask 0..7 forces that mask, -1 chooses the one with the lowest penalty. ValueError("Invalid value") unless
    // 1 <= minversion <= maxversion <= 40 and -1 <= mask <= 7; DataTooLongError("Data length = <n> bits, Max
    // capacity = <c> bits"), or "Segment too long" when a segment's count overflows its field at maxversion.
    static Result<QrCode> encode_segments(const std::vector<QrSegment>& segs, Ecc ecl, int minversion = 1,
                                          int maxversion = 40, int mask = -1, bool boostecl = true);

    // ---- Constructor (low level) ----

    // QrCode(version, errcorlvl, datacodewords, msk): ValueError("Version value out of range") /
    // ValueError("Mask value out of range"); AssertionError when datacodewords is not exactly the data capacity of
    // that version and level.
    static Result<QrCode> make(int version, Ecc errcorlvl, std::span<const uint8_t> datacodewords, int msk);

    // ---- Accessor methods ----

    int get_version() const { return version_; }                 // 1..40
    int get_size() const { return size_; }                       // 21..177, version * 4 + 17
    Ecc get_error_correction_level() const { return errcorlvl_; }
    int get_mask() const { return mask_; }                       // 0..7
    // The module at (x, y), true = dark, (0, 0) the top left; false (light) out of bounds.
    bool get_module(int x, int y) const;

private:
    QrCode(int version, Ecc errcorlvl);

    // ---- Drawing function modules ----
    void draw_function_patterns();
    void draw_format_bits(int mask);
    void draw_version();
    void draw_finder_pattern(int x, int y);
    void draw_alignment_pattern(int x, int y);
    void set_function_module(int x, int y, bool isdark);

    // ---- Codewords and masking ----
    std::vector<uint8_t> add_ecc_and_interleave(std::span<const uint8_t> data) const;
    void draw_codewords(std::span<const uint8_t> data);
    void apply_mask(int mask);
    int64_t get_penalty_score() const;

    // ---- Helpers ----
    std::vector<int> get_alignment_pattern_positions() const;
    // The index of (x, y) in modules_ and isfunction_.
    size_t at(int x, int y) const {
        return static_cast<size_t>(y) * static_cast<size_t>(size_) + static_cast<size_t>(x);
    }

    int version_;
    int size_;
    Ecc errcorlvl_;
    int mask_ = 0;
    std::vector<uint8_t> modules_;     // size * size, row-major, 1 = dark
    std::vector<uint8_t> isfunction_;  // function modules (not masked); emptied when make finishes
};

}  // namespace scfg::qrcodegen
