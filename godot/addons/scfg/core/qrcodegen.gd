## QR Code generator: a GDScript port of the vendored Python copy of the Project Nayuki
## library, scfg_core/_qrcodegen.py (not of upstream v1.8.0, which special-cases the version 32
## alignment step; both give the standard's positions). Line-for-line where it can be, and
## bit-identical; the GDScript differences are listed below.
##
## Differences from the Python module, none of which changes a result:
## - Python's exceptions are ScfgErr values (ValueError, DataTooLongError, a ValueError) from the
##   public factories; QrCode's constructor is the static `QrCode.create(...)`, which can return
##   an error, and QrSegment's is `QrSegment.create(...)`.
## - The error correction levels are the ordinals in `Ecc` (LOW 0, MEDIUM 1, QUARTILE 2,
##   HIGH 3); `get_error_correction_level()` returns that ordinal.
## - Modules are flat PackedByteArrays, bit buffers PackedByteArrays of 0/1, segment modes
##   small Mode objects, and the mask patterns a `match` instead of lambdas (speed).
## - The private helpers (_BitBuffer.append_bits, _reed_solomon_*, _get_num_raw_data_modules,
##   _apply_mask) assume what their only callers guarantee instead of raising.
##
## The original notice (MIT License) follows.
##
## QR Code generator library (Python)
##
## Copyright (c) Project Nayuki. (MIT License)
## https://www.nayuki.io/page/qr-code-generator-library
##
## Permission is hereby granted, free of charge, to any person obtaining a copy of
## this software and associated documentation files (the "Software"), to deal in
## the Software without restriction, including without limitation the rights to
## use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
## the Software, and to permit persons to whom the Software is furnished to do so,
## subject to the following conditions:
## - The above copyright notice and this permission notice shall be included in
##   all copies or substantial portions of the Software.
## - The Software is provided "as is", without warranty of any kind, express or
##   implied, including but not limited to the warranties of merchantability,
##   fitness for a particular purpose and noninfringement. In no event shall the
##   authors or copyright holders be liable for any claim, damages or other
##   liability, whether in an action of contract, tort or otherwise, arising from,
##   out of or in connection with the Software or the use or other dealings in the
##   Software.
class_name ScfgQrcodegen
extends RefCounted


## The error correction level in a QR Code symbol (the ordinal; FORMAT_BITS[ordinal] are the
## format bits).
class Ecc:
	const LOW := 0  # The QR Code can tolerate about  7% erroneous codewords
	const MEDIUM := 1  # The QR Code can tolerate about 15% erroneous codewords
	const QUARTILE := 2  # The QR Code can tolerate about 25% erroneous codewords
	const HIGH := 3  # The QR Code can tolerate about 30% erroneous codewords
	const FORMAT_BITS: Array[int] = [1, 0, 3, 2]


## A QR Code symbol, which is a type of two-dimension barcode.
## Invented by Denso Wave and described in the ISO/IEC 18004 standard.
## Instances of this class represent an immutable square grid of dark and light cells.
## The class provides static factory functions to create a QR Code from text or binary data.
## The class covers the QR Code Model 2 specification, supporting all versions (sizes)
## from 1 to 40, all 4 error correction levels, and 4 character encoding modes.
class QrCode:
	extends RefCounted

	const MIN_VERSION := 1  # The minimum version number supported in the QR Code Model 2 standard
	const MAX_VERSION := 40  # The maximum version number supported in the QR Code Model 2 standard

	# For use in _get_penalty_score(), when evaluating which mask is best.
	const _PENALTY_N1 := 3
	const _PENALTY_N2 := 3
	const _PENALTY_N3 := 40
	const _PENALTY_N4 := 10

	const _ECC_CODEWORDS_PER_BLOCK: Array = [
		# Version: (note that index 0 is for padding, and is set to an illegal value)
		# 0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40    Error correction level
		[-1,  7, 10, 15, 20, 26, 18, 20, 24, 30, 18, 20, 24, 26, 30, 22, 24, 28, 30, 28, 28, 28, 28, 30, 30, 26, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30],  # Low
		[-1, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26, 30, 22, 22, 24, 24, 28, 28, 26, 26, 26, 26, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28],  # Medium
		[-1, 13, 22, 18, 26, 18, 24, 18, 22, 20, 24, 28, 26, 24, 20, 30, 24, 28, 28, 26, 30, 28, 30, 30, 30, 30, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30],  # Quartile
		[-1, 17, 28, 22, 16, 22, 28, 26, 26, 24, 28, 24, 28, 22, 24, 24, 30, 28, 28, 26, 28, 30, 24, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30]]  # High

	const _NUM_ERROR_CORRECTION_BLOCKS: Array = [
		# Version: (note that index 0 is for padding, and is set to an illegal value)
		# 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40    Error correction level
		[-1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 4,  4,  4,  4,  4,  6,  6,  6,  6,  7,  8,  8,  9,  9, 10, 12, 12, 12, 13, 14, 15, 16, 17, 18, 19, 19, 20, 21, 22, 24, 25],  # Low
		[-1, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5,  5,  8,  9,  9, 10, 10, 11, 13, 14, 16, 17, 17, 18, 20, 21, 23, 25, 26, 28, 29, 31, 33, 35, 37, 38, 40, 43, 45, 47, 49],  # Medium
		[-1, 1, 1, 2, 2, 4, 4, 6, 6, 8, 8,  8, 10, 12, 16, 12, 17, 16, 18, 21, 20, 23, 23, 25, 27, 29, 34, 34, 35, 38, 40, 43, 45, 48, 51, 53, 56, 59, 62, 65, 68],  # Quartile
		[-1, 1, 1, 2, 4, 4, 4, 5, 6, 8, 8, 11, 11, 16, 16, 18, 16, 19, 21, 25, 25, 25, 34, 30, 32, 35, 37, 40, 42, 45, 48, 51, 54, 57, 60, 63, 66, 70, 74, 77, 81]]  # High

	# ---- Instance fields ----

	# The version number of this QR Code, which is between 1 and 40 (inclusive).
	# This determines the size of this barcode.
	var _version: int
	# The width and height of this QR Code, measured in modules, between
	# 21 and 177 (inclusive). This is equal to version * 4 + 17.
	var _size: int
	# The error correction level used in this QR Code (an Ecc ordinal).
	var _errcorlvl: int
	# The index of the mask pattern used in this QR Code, which is between 0 and 7 (inclusive).
	# Even if a QR Code is created with automatic masking requested (mask = -1),
	# the resulting object still has a mask value between 0 and 7.
	var _mask: int
	# The modules of this QR Code (0 = light, 1 = dark), row-major: [y * size + x].
	var _modules: PackedByteArray
	# Indicates function modules that are not subjected to masking. Discarded when constructor finishes.
	var _isfunction: PackedByteArray

	# ---- Static factory functions (high level) ----

	## Returns a QR Code representing the given Unicode text string at the given error correction level.
	## As a conservative upper bound, this function is guaranteed to succeed for strings that have 738 or fewer
	## Unicode code points (not UTF-16 code units) if the low error correction level is used. The smallest possible
	## QR Code version is automatically chosen for the output. The ECC level of the result may be higher than the
	## ecl argument if it can be done without increasing the version.
	## @fallible
	static func encode_text(text: String, ecl: int) -> Variant:
		var segs: Array = QrSegment.make_segments(text)
		return encode_segments(segs, ecl)

	## Returns a QR Code representing the given binary data at the given error correction level.
	## This function always encodes using the binary segment mode, not any text mode. The maximum number of
	## bytes allowed is 2953. The smallest possible QR Code version is automatically chosen for the output.
	## The ECC level of the result may be higher than the ecl argument if it can be done without increasing the version.
	## @fallible
	static func encode_binary(data: PackedByteArray, ecl: int) -> Variant:
		return encode_segments([QrSegment.make_bytes(data)], ecl)

	# ---- Static factory functions (mid level) ----

	## Returns a QR Code representing the given segments with the given encoding parameters.
	## The smallest possible QR Code version within the given range is automatically
	## chosen for the output. Iff boostecl is true, then the ECC level of the result
	## may be higher than the ecl argument if it can be done without increasing the
	## version. The mask number is either between 0 to 7 (inclusive) to force that
	## mask, or -1 to automatically choose an appropriate mask (which may be slow).
	## This function allows the user to create a custom sequence of segments that switches
	## between modes (such as alphanumeric and byte) to encode text in less space.
	## This is a mid-level API; the high-level API is encode_text() and encode_binary().
	## @fallible
	static func encode_segments(segs: Array, ecl: int, minversion: int = 1, maxversion: int = 40, mask: int = -1,
			boostecl: bool = true) -> Variant:
		if not (MIN_VERSION <= minversion and minversion <= maxversion and maxversion <= MAX_VERSION) or not (-1 <= mask and mask <= 7):
			return ScfgErr.make("ValueError", "Invalid value")
		if ecl < Ecc.LOW or ecl > Ecc.HIGH:
			return ScfgErr.make("ValueError", "Invalid value")
		for seg: Variant in segs:
			if not seg is QrSegment:
				return ScfgErr.make("TypeError", "expected a QrSegment, got '%s'" % ScfgPy.py_type(seg))

		# Find the minimal version number to use
		var version := minversion
		var datausedbits: Variant = null
		while true:
			var datacapacitybits: int = _get_num_data_codewords(version, ecl) * 8  # Number of data bits available
			datausedbits = QrSegment.get_total_bits(segs, version)
			if datausedbits != null and datausedbits <= datacapacitybits:
				break  # This version number is found to be suitable
			if version >= maxversion:  # All versions in the range could not fit the given data
				var msg := "Segment too long"
				if datausedbits != null:
					msg = "Data length = %d bits, Max capacity = %d bits" % [datausedbits, datacapacitybits]
				return ScfgErr.make("DataTooLongError", msg)
			version += 1
		var usedbits: int = datausedbits

		# Increase the error correction level while the data still fits in the current version number
		for newecl: int in [Ecc.MEDIUM, Ecc.QUARTILE, Ecc.HIGH]:  # From low to high
			if boostecl and usedbits <= _get_num_data_codewords(version, newecl) * 8:
				ecl = newecl

		# Concatenate all segments to create the data bit string
		var bb := PackedByteArray()
		for seg: QrSegment in segs:
			ScfgQrcodegen._append_bits(bb, seg.get_mode().get_mode_bits(), 4)
			ScfgQrcodegen._append_bits(bb, seg.get_num_chars(), seg.get_mode().num_char_count_bits(version))
			bb.append_array(seg._bitdata)

		# Add terminator and pad up to a byte if applicable
		var capacitybits: int = _get_num_data_codewords(version, ecl) * 8
		ScfgQrcodegen._append_bits(bb, 0, mini(4, capacitybits - bb.size()))
		ScfgQrcodegen._append_bits(bb, 0, posmod(-bb.size(), 8))  # Python's modulo on negative numbers

		# Pad with alternating bytes until data capacity is reached
		var padbyte := 0xEC
		while bb.size() < capacitybits:
			ScfgQrcodegen._append_bits(bb, padbyte, 8)
			padbyte = 0x11 if padbyte == 0xEC else 0xEC

		# Pack bits into bytes in big endian
		var datacodewords := PackedByteArray()
		datacodewords.resize(bb.size() >> 3)
		datacodewords.fill(0)
		for i in bb.size():
			datacodewords[i >> 3] |= bb[i] << (7 - (i & 7))

		# Create the QR Code object
		return create(version, ecl, datacodewords, mask)

	# ---- Constructor (low level) ----

	## Creates a new QR Code with the given version number,
	## error correction level, data codeword bytes, and mask number.
	## This is a low-level API that most users should not use directly.
	## A mid-level API is the encode_segments() function.
	## (Python's constructor; a version or mask out of range, or a wrong number of data
	## codewords, is a ValueError.)
	## @fallible
	static func create(version: int, errcorlvl: int, datacodewords: PackedByteArray, msk: int) -> Variant:
		# Check scalar arguments
		if not (MIN_VERSION <= version and version <= MAX_VERSION):
			return ScfgErr.make("ValueError", "Version value out of range")
		if not (-1 <= msk and msk <= 7):
			return ScfgErr.make("ValueError", "Mask value out of range")
		if errcorlvl < Ecc.LOW or errcorlvl > Ecc.HIGH or datacodewords.size() != _get_num_data_codewords(version, errcorlvl):
			return ScfgErr.make("ValueError", "Invalid value")
		var qr := QrCode.new()
		qr._build(version, errcorlvl, datacodewords, msk)
		return qr

	func _build(version: int, errcorlvl: int, datacodewords: PackedByteArray, msk: int) -> void:
		_version = version
		_size = version * 4 + 17
		_errcorlvl = errcorlvl

		# Initialize both grids to be size*size arrays of Boolean false
		_modules = PackedByteArray()
		_modules.resize(_size * _size)
		_modules.fill(0)  # Initially all light
		_isfunction = PackedByteArray()
		_isfunction.resize(_size * _size)
		_isfunction.fill(0)

		# Compute ECC, draw modules
		_draw_function_patterns()
		var allcodewords := _add_ecc_and_interleave(datacodewords)
		_draw_codewords(allcodewords)

		# Do masking
		if msk == -1:  # Automatically choose best mask
			var minpenalty: int = 1 << 32
			for i in 8:
				_apply_mask(i)
				_draw_format_bits(i)
				var penalty := _get_penalty_score()
				if penalty < minpenalty:
					msk = i
					minpenalty = penalty
				_apply_mask(i)  # Undoes the mask due to XOR
		_mask = msk
		_apply_mask(msk)  # Apply the final choice of mask
		_draw_format_bits(msk)  # Overwrite old format bits

		_isfunction = PackedByteArray()

	# ---- Accessor methods ----

	## Returns this QR Code's version number, in the range [1, 40].
	func get_version() -> int:
		return _version

	## Returns this QR Code's size, in the range [21, 177].
	func get_size() -> int:
		return _size

	## Returns this QR Code's error correction level (an Ecc ordinal).
	func get_error_correction_level() -> int:
		return _errcorlvl

	## Returns this QR Code's mask, in the range [0, 7].
	func get_mask() -> int:
		return _mask

	## Returns the color of the module (pixel) at the given coordinates, which is False
	## for light or True for dark. The top left corner has the coordinates (x=0, y=0).
	## If the given coordinates are out of bounds, then False (light) is returned.
	func get_module(x: int, y: int) -> bool:
		return (0 <= x and x < _size) and (0 <= y and y < _size) and _modules[y * _size + x] != 0

	# ---- Private helper methods for constructor: Drawing function modules ----

	# Reads this object's version field, and draws and marks all function modules.
	func _draw_function_patterns() -> void:
		# Draw horizontal and vertical timing patterns
		for i in _size:
			_set_function_module(6, i, i % 2 == 0)
			_set_function_module(i, 6, i % 2 == 0)

		# Draw 3 finder patterns (all corners except bottom right; overwrites some timing modules)
		_draw_finder_pattern(3, 3)
		_draw_finder_pattern(_size - 4, 3)
		_draw_finder_pattern(3, _size - 4)

		# Draw numerous alignment patterns
		var alignpatpos: Array = _get_alignment_pattern_positions()
		var numalign: int = alignpatpos.size()
		var skips := [Vector2i(0, 0), Vector2i(0, numalign - 1), Vector2i(numalign - 1, 0)]
		for i in numalign:
			for j in numalign:
				if not Vector2i(i, j) in skips:  # Don't draw on the three finder corners
					_draw_alignment_pattern(alignpatpos[i], alignpatpos[j])

		# Draw configuration data
		_draw_format_bits(0)  # Dummy mask value; overwritten later in the constructor
		_draw_version()

	# Draws two copies of the format bits (with its own error correction code)
	# based on the given mask and this object's error correction level field.
	func _draw_format_bits(mask: int) -> void:
		# Calculate error correction code and pack bits
		var data: int = Ecc.FORMAT_BITS[_errcorlvl] << 3 | mask  # errCorrLvl is uint2, mask is uint3
		var rem: int = data
		for _i in 10:
			rem = (rem << 1) ^ ((rem >> 9) * 0x537)
		var bits: int = (data << 10 | rem) ^ 0x5412  # uint15

		# Draw first copy
		for i in range(0, 6):
			_set_function_module(8, i, ScfgQrcodegen._get_bit(bits, i))
		_set_function_module(8, 7, ScfgQrcodegen._get_bit(bits, 6))
		_set_function_module(8, 8, ScfgQrcodegen._get_bit(bits, 7))
		_set_function_module(7, 8, ScfgQrcodegen._get_bit(bits, 8))
		for i in range(9, 15):
			_set_function_module(14 - i, 8, ScfgQrcodegen._get_bit(bits, i))

		# Draw second copy
		for i in range(0, 8):
			_set_function_module(_size - 1 - i, 8, ScfgQrcodegen._get_bit(bits, i))
		for i in range(8, 15):
			_set_function_module(8, _size - 15 + i, ScfgQrcodegen._get_bit(bits, i))
		_set_function_module(8, _size - 8, true)  # Always dark

	# Draws two copies of the version bits (with its own error correction code),
	# based on this object's version field, iff 7 <= version <= 40.
	func _draw_version() -> void:
		if _version < 7:
			return

		# Calculate error correction code and pack bits
		var rem: int = _version  # version is uint6, in the range [7, 40]
		for _i in 12:
			rem = (rem << 1) ^ ((rem >> 11) * 0x1F25)
		var bits: int = _version << 12 | rem  # uint18

		# Draw two copies
		for i in 18:
			var bit: bool = ScfgQrcodegen._get_bit(bits, i)
			var a: int = _size - 11 + i % 3
			var b: int = i / 3
			_set_function_module(a, b, bit)
			_set_function_module(b, a, bit)

	# Draws a 9*9 finder pattern including the border separator,
	# with the center module at (x, y). Modules can be out of bounds.
	func _draw_finder_pattern(x: int, y: int) -> void:
		for dy in range(-4, 5):
			for dx in range(-4, 5):
				var xx := x + dx
				var yy := y + dy
				if (0 <= xx and xx < _size) and (0 <= yy and yy < _size):
					var dist := maxi(absi(dx), absi(dy))  # Chebyshev/infinity norm
					_set_function_module(xx, yy, dist != 2 and dist != 4)

	# Draws a 5*5 alignment pattern, with the center module
	# at (x, y). All modules must be in bounds.
	func _draw_alignment_pattern(x: int, y: int) -> void:
		for dy in range(-2, 3):
			for dx in range(-2, 3):
				_set_function_module(x + dx, y + dy, maxi(absi(dx), absi(dy)) != 1)

	# Sets the color of a module and marks it as a function module.
	# Only used by the constructor. Coordinates must be in bounds.
	func _set_function_module(x: int, y: int, isdark: bool) -> void:
		_modules[y * _size + x] = 1 if isdark else 0
		_isfunction[y * _size + x] = 1

	# ---- Private helper methods for constructor: Codewords and masking ----

	# Returns a new byte string representing the given data with the appropriate error correction
	# codewords appended to it, based on this object's version and error correction level.
	func _add_ecc_and_interleave(data: PackedByteArray) -> PackedByteArray:
		var version: int = _version

		# Calculate parameter numbers
		var numblocks: int = _NUM_ERROR_CORRECTION_BLOCKS[_errcorlvl][version]
		var blockecclen: int = _ECC_CODEWORDS_PER_BLOCK[_errcorlvl][version]
		var rawcodewords: int = _get_num_raw_data_modules(version) / 8
		var numshortblocks: int = numblocks - rawcodewords % numblocks
		var shortblocklen: int = rawcodewords / numblocks

		# Split data into blocks and append ECC to each block
		var blocks: Array[PackedByteArray] = []
		var rsdiv := _reed_solomon_compute_divisor(blockecclen)
		var k := 0
		for i in numblocks:
			var dat := data.slice(k, k + shortblocklen - blockecclen + (0 if i < numshortblocks else 1))
			k += dat.size()
			var ecc := _reed_solomon_compute_remainder(dat, rsdiv)
			if i < numshortblocks:
				dat.append(0)
			dat.append_array(ecc)
			blocks.append(dat)

		# Interleave (not concatenate) the bytes from every block into a single sequence
		var result := PackedByteArray()
		for i in blocks[0].size():
			for j in blocks.size():
				# Skip the padding byte in short blocks
				if (i != shortblocklen - blockecclen) or (j >= numshortblocks):
					result.append(blocks[j][i])
		return result

	# Draws the given sequence of 8-bit codewords (data and error correction) onto the entire
	# data area of this QR Code. Function modules need to be marked off before this is called.
	func _draw_codewords(data: PackedByteArray) -> void:
		var total := data.size() * 8
		var i := 0  # Bit index into the data
		# Do the funny zigzag scan
		for column_pair in range(_size - 1, 0, -2):  # Index of right column in each column pair
			var right: int = column_pair
			if right <= 6:
				right -= 1
			var upward: bool = ((right + 1) & 2) == 0
			for vert in _size:  # Vertical counter
				for j in 2:
					var x: int = right - j  # Actual x coordinate
					var y: int = (_size - 1 - vert) if upward else vert  # Actual y coordinate
					if _isfunction[y * _size + x] == 0 and i < total:
						_modules[y * _size + x] = (data[i >> 3] >> (7 - (i & 7))) & 1
						i += 1
					# If this QR Code has any remainder bits (0 to 7), they were assigned as
					# 0/false/light by the constructor and are left unchanged by this method

	# XORs the codeword modules in this QR Code with the given mask pattern.
	# The function modules must be marked and the codeword bits must be drawn
	# before masking. Due to the arithmetic of XOR, calling _apply_mask() with
	# the same mask value a second time will undo the mask. A final well-formed
	# QR Code needs exactly one (not zero, two, etc.) mask applied.
	# (The callers pass a mask in 0..7, which the Python checks.)
	func _apply_mask(mask: int) -> void:
		for y in _size:
			for x in _size:
				var p := y * _size + x
				if _isfunction[p] == 0 and _mask_pattern(mask, x, y) == 0:
					_modules[p] ^= 1

	static func _mask_pattern(mask: int, x: int, y: int) -> int:
		match mask:
			0:
				return (x + y) % 2
			1:
				return y % 2
			2:
				return x % 3
			3:
				return (x + y) % 3
			4:
				return (x / 3 + y / 2) % 2
			5:
				return x * y % 2 + x * y % 3
			6:
				return (x * y % 2 + x * y % 3) % 2
			_:
				return ((x + y) % 2 + x * y % 3) % 2

	# Calculates and returns the penalty score based on state of this QR Code's current modules.
	# This is used by the automatic mask choice algorithm to find the mask pattern that yields the lowest score.
	func _get_penalty_score() -> int:
		var result := 0
		var size := _size
		var modules := _modules

		# Adjacent modules in row having same color, and finder-like patterns
		for y in size:
			var runcolor := 0
			var runx := 0
			var runhistory := PackedInt32Array([0, 0, 0, 0, 0, 0, 0])
			for x in size:
				var color := modules[y * size + x]
				if color == runcolor:
					runx += 1
					if runx == 5:
						result += _PENALTY_N1
					elif runx > 5:
						result += 1
				else:
					_finder_penalty_add_history(runx, runhistory)
					if runcolor == 0:
						result += _finder_penalty_count_patterns(runhistory) * _PENALTY_N3
					runcolor = color
					runx = 1
			result += _finder_penalty_terminate_and_count(runcolor, runx, runhistory) * _PENALTY_N3

		# Adjacent modules in column having same color, and finder-like patterns
		for x in size:
			var runcolor := 0
			var runy := 0
			var runhistory := PackedInt32Array([0, 0, 0, 0, 0, 0, 0])
			for y in size:
				var color := modules[y * size + x]
				if color == runcolor:
					runy += 1
					if runy == 5:
						result += _PENALTY_N1
					elif runy > 5:
						result += 1
				else:
					_finder_penalty_add_history(runy, runhistory)
					if runcolor == 0:
						result += _finder_penalty_count_patterns(runhistory) * _PENALTY_N3
					runcolor = color
					runy = 1
			result += _finder_penalty_terminate_and_count(runcolor, runy, runhistory) * _PENALTY_N3

		# 2*2 blocks of modules having same color
		for y in size - 1:
			for x in size - 1:
				var p := y * size + x
				var c := modules[p]
				if c == modules[p + 1] and c == modules[p + size] and c == modules[p + size + 1]:
					result += _PENALTY_N2

		# Balance of dark and light modules
		var dark := 0
		for cell in modules:
			dark += cell
		var total: int = size * size  # Note that size is odd, so dark/total != 1/2
		# Compute the smallest integer k >= 0 such that (45-5k)% <= dark/total <= (55+5k)%
		var k: int = (absi(dark * 20 - total * 10) + total - 1) / total - 1
		result += k * _PENALTY_N4
		return result

	# ---- Private helper functions ----

	# Returns an ascending list of positions of alignment patterns for this version number.
	# Each position is in the range [0,177), and are used on both the x and y axes.
	# This could be implemented as lookup table of 40 variable-length lists of integers.
	# (The vendored Python's step formula: version 32 gets step 26 without a special case.)
	func _get_alignment_pattern_positions() -> Array:
		if _version == 1:
			return []
		var numalign: int = _version / 7 + 2
		var step: int = (_version * 8 + numalign * 3 + 5) / (numalign * 4 - 4) * 2
		var result: Array = []
		for i in numalign - 1:
			result.append(_size - 7 - i * step)
		result.append(6)
		result.reverse()
		return result

	# Returns the number of data bits that can be stored in a QR Code of the given version number, after
	# all function modules are excluded. This includes remainder bits, so it might not be a multiple of 8.
	# The result is in the range [208, 29648]. This could be implemented as a 40-entry lookup table.
	# (Callers pass a version in 1..40, which the Python checks.)
	static func _get_num_raw_data_modules(ver: int) -> int:
		var result: int = (16 * ver + 128) * ver + 64
		if ver >= 2:
			var numalign: int = ver / 7 + 2
			result -= (25 * numalign - 10) * numalign - 55
			if ver >= 7:
				result -= 36
		return result

	# Returns the number of 8-bit data (i.e. not error correction) codewords contained in any
	# QR Code of the given version number and error correction level, with remainder bits discarded.
	# This stateless pure function could be implemented as a (40*4)-cell lookup table.
	static func _get_num_data_codewords(ver: int, ecl: int) -> int:
		return (_get_num_raw_data_modules(ver) / 8
			- _ECC_CODEWORDS_PER_BLOCK[ecl][ver]
			* _NUM_ERROR_CORRECTION_BLOCKS[ecl][ver])

	# Returns a Reed-Solomon ECC generator polynomial for the given degree. This could be
	# implemented as a lookup table over all possible parameter values, instead of as an algorithm.
	# (Callers pass a degree in 1..255, which the Python checks.)
	static func _reed_solomon_compute_divisor(degree: int) -> PackedByteArray:
		# Polynomial coefficients are stored from highest to lowest power, excluding the leading term which is always 1.
		# For example the polynomial x^3 + 255x^2 + 8x + 93 is stored as the uint8 array [255, 8, 93].
		var result := PackedByteArray()
		result.resize(degree)
		result.fill(0)
		result[degree - 1] = 1  # Start off with the monomial x^0

		# Compute the product polynomial (x - r^0) * (x - r^1) * (x - r^2) * ... * (x - r^{degree-1}),
		# and drop the highest monomial term which is always 1x^degree.
		# Note that r = 0x02, which is a generator element of this field GF(2^8/0x11D).
		var root := 1
		for _i in degree:  # Unused variable i
			# Multiply the current product by (x - r^i)
			for j in degree:
				result[j] = _reed_solomon_multiply(result[j], root)
				if j + 1 < degree:
					result[j] ^= result[j + 1]
			root = _reed_solomon_multiply(root, 0x02)
		return result

	# Returns the Reed-Solomon error correction codeword for the given data and divisor polynomials.
	static func _reed_solomon_compute_remainder(data: PackedByteArray, divisor: PackedByteArray) -> PackedByteArray:
		var n := divisor.size()
		var result := PackedByteArray()
		result.resize(n)
		result.fill(0)
		for b in data:  # Polynomial division
			var factor: int = b ^ result[0]
			# result.pop(0); result.append(0)
			for i in n - 1:
				result[i] = result[i + 1]
			result[n - 1] = 0
			for i in n:
				result[i] ^= _reed_solomon_multiply(divisor[i], factor)
		return result

	# Returns the product of the two given field elements modulo GF(2^8/0x11D). The arguments and result
	# are unsigned 8-bit integers. This could be implemented as a lookup table of 256*256 entries of uint8.
	# (Callers pass bytes, which the Python checks.)
	static func _reed_solomon_multiply(x: int, y: int) -> int:
		# Russian peasant multiplication
		var z := 0
		for i in range(7, -1, -1):
			z = (z << 1) ^ ((z >> 7) * 0x11D)
			z ^= ((y >> i) & 1) * x
		return z

	# Can only be called immediately after a light run is added, and
	# returns either 0, 1, or 2. A helper function for _get_penalty_score().
	func _finder_penalty_count_patterns(runhistory: PackedInt32Array) -> int:
		var n: int = runhistory[1]
		var core: bool = (n > 0 and runhistory[2] == n and runhistory[4] == n and runhistory[5] == n
			and runhistory[3] == n * 3)
		return ((1 if (core and runhistory[0] >= n * 4 and runhistory[6] >= n) else 0)
			+ (1 if (core and runhistory[6] >= n * 4 and runhistory[0] >= n) else 0))

	# Must be called at the end of a line (row or column) of modules. A helper function for _get_penalty_score().
	func _finder_penalty_terminate_and_count(currentruncolor: int, currentrunlength: int,
			runhistory: PackedInt32Array) -> int:
		if currentruncolor != 0:  # Terminate dark run
			_finder_penalty_add_history(currentrunlength, runhistory)
			currentrunlength = 0
		currentrunlength += _size  # Add light border to final run
		_finder_penalty_add_history(currentrunlength, runhistory)
		return _finder_penalty_count_patterns(runhistory)

	# Pushes the given value to the front and drops the last value. A helper function for _get_penalty_score().
	# (Python's deque(maxlen=7).appendleft.)
	func _finder_penalty_add_history(currentrunlength: int, runhistory: PackedInt32Array) -> void:
		if runhistory[0] == 0:
			currentrunlength += _size  # Add light border to initial run
		for i in range(6, 0, -1):
			runhistory[i] = runhistory[i - 1]
		runhistory[0] = currentrunlength


## Describes how a segment's data bits are interpreted. Immutable.
class Mode:
	extends RefCounted

	# The mode indicator bits, which is a uint4 value (range 0 to 15)
	var _modebits: int
	# Number of character count bits for three different version ranges
	var _charcounts: Array[int]

	func _init(modebits: int, charcounts: Array[int]) -> void:
		_modebits = modebits
		_charcounts = charcounts

	## Returns an unsigned 4-bit integer value (range 0 to 15) representing the mode indicator bits for this mode object.
	func get_mode_bits() -> int:
		return _modebits

	## Returns the bit width of the character count field for a segment in this mode
	## in a QR Code at the given version number. The result is in the range [0, 16].
	func num_char_count_bits(ver: int) -> int:
		return _charcounts[(ver + 7) / 17]


## A segment of character/binary/control data in a QR Code symbol.
## Instances of this class are immutable.
## The mid-level way to create a segment is to take the payload data
## and call a static factory function such as QrSegment.make_numeric().
## The low-level way to create a segment is to custom-make the bit buffer
## and call the QrSegment.create() constructor with appropriate values.
## This segment class imposes no length restrictions, but QR Codes have restrictions.
## Even in the most favorable conditions, a QR Code can only hold 7089 characters of data.
## Any segment longer than this is meaningless for the purpose of generating QR Codes.
class QrSegment:
	extends RefCounted

	# ---- Public helper enumeration ----

	static var NUMERIC := Mode.new(0x1, [10, 12, 14])
	static var ALPHANUMERIC := Mode.new(0x2, [9, 11, 13])
	static var BYTE := Mode.new(0x4, [8, 16, 16])
	static var KANJI := Mode.new(0x8, [8, 10, 12])
	static var ECI := Mode.new(0x7, [0, 0, 0])

	# The set of all legal characters in alphanumeric mode, where
	# each character value maps to the index in the string.
	const _ALPHANUMERIC_CHARSET := "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:"

	# ---- Instance fields ----

	# The mode indicator of this segment.
	var _mode: Mode
	# The length of this segment's unencoded data. Measured in characters for
	# numeric/alphanumeric/kanji mode, bytes for byte mode, and 0 for ECI mode.
	# Always zero or positive. Not the same as the data's bit length.
	var _numchars: int
	# The data bits of this segment (one 0/1 per byte).
	var _bitdata: PackedByteArray

	# ---- Static factory functions (mid level) ----

	## Returns a segment representing the given binary data encoded in byte mode.
	## All input byte lists are acceptable. Any text string can be converted to
	## UTF-8 bytes (s.to_utf8_buffer()) and encoded as a byte mode segment.
	static func make_bytes(data: PackedByteArray) -> QrSegment:
		var bb := PackedByteArray()
		for b in data:
			ScfgQrcodegen._append_bits(bb, b, 8)
		return _new(BYTE, data.size(), bb)

	## Returns a segment representing the given string of decimal digits encoded in numeric mode.
	## @fallible
	static func make_numeric(digits: String) -> Variant:
		if not is_numeric(digits):
			return ScfgErr.make("ValueError", "String contains non-numeric characters")
		var bb := PackedByteArray()
		var i := 0
		while i < digits.length():  # Consume up to 3 digits per iteration
			var n: int = mini(digits.length() - i, 3)
			ScfgQrcodegen._append_bits(bb, digits.substr(i, n).to_int(), n * 3 + 1)
			i += n
		return _new(NUMERIC, digits.length(), bb)

	## Returns a segment representing the given text string encoded in alphanumeric mode.
	## The characters allowed are: 0 to 9, A to Z (uppercase only), space,
	## dollar, percent, asterisk, plus, hyphen, period, slash, colon.
	## @fallible
	static func make_alphanumeric(text: String) -> Variant:
		if not is_alphanumeric(text):
			return ScfgErr.make("ValueError", "String contains unencodable characters in alphanumeric mode")
		var bb := PackedByteArray()
		for i in range(0, text.length() - 1, 2):  # Process groups of 2
			var temp: int = _ALPHANUMERIC_CHARSET.find(text[i]) * 45
			temp += _ALPHANUMERIC_CHARSET.find(text[i + 1])
			ScfgQrcodegen._append_bits(bb, temp, 11)
		if text.length() % 2 > 0:  # 1 character remaining
			ScfgQrcodegen._append_bits(bb, _ALPHANUMERIC_CHARSET.find(text[text.length() - 1]), 6)
		return _new(ALPHANUMERIC, text.length(), bb)

	## Returns a new mutable list of zero or more segments to represent the given Unicode text string.
	## The result may use various segment modes and switch modes to optimize the length of the bit stream.
	static func make_segments(text: String) -> Array:
		# Select the most efficient segment encoding automatically
		if text == "":
			return []
		elif is_numeric(text):
			return [make_numeric(text)]  # err-ok: is_numeric() holds
		elif is_alphanumeric(text):
			return [make_alphanumeric(text)]  # err-ok: is_alphanumeric() holds
		else:
			return [make_bytes(text.to_utf8_buffer())]

	## Returns a segment representing an Extended Channel Interpretation
	## (ECI) designator with the given assignment value.
	## @fallible
	static func make_eci(assignval: int) -> Variant:
		var bb := PackedByteArray()
		if assignval < 0:
			return ScfgErr.make("ValueError", "ECI assignment value out of range")
		elif assignval < (1 << 7):
			ScfgQrcodegen._append_bits(bb, assignval, 8)
		elif assignval < (1 << 14):
			ScfgQrcodegen._append_bits(bb, 0b10, 2)
			ScfgQrcodegen._append_bits(bb, assignval, 14)
		elif assignval < 1000000:
			ScfgQrcodegen._append_bits(bb, 0b110, 3)
			ScfgQrcodegen._append_bits(bb, assignval, 21)
		else:
			return ScfgErr.make("ValueError", "ECI assignment value out of range")
		return _new(ECI, 0, bb)

	# Tests whether the given string can be encoded as a segment in numeric mode.
	# A string is encodable iff each character is in the range 0 to 9.
	static func is_numeric(text: String) -> bool:
		for i in text.length():
			var c := text.unicode_at(i)
			if c < 0x30 or c > 0x39:
				return false
		return true

	# Tests whether the given string can be encoded as a segment in alphanumeric mode.
	# A string is encodable iff each character is in the following set: 0 to 9, A to Z
	# (uppercase only), space, dollar, percent, asterisk, plus, hyphen, period, slash, colon.
	static func is_alphanumeric(text: String) -> bool:
		for i in text.length():
			var c := text.unicode_at(i)
			if c >= 0x80 or not _ALPHANUMERIC_CHARSET.contains(String.chr(c)):
				return false
		return true

	# ---- Constructor (low level) ----

	## Creates a new QR Code segment with the given attributes and data.
	## The character count (numch) must agree with the mode and the bit buffer length,
	## but the constraint isn't checked. The given bit buffer is cloned and stored.
	## (Python's constructor: a negative numch is a ValueError.)
	## @fallible
	static func create(mode: Mode, numch: int, bitdata: PackedByteArray) -> Variant:
		if numch < 0:
			return ScfgErr.make("ValueError", "")
		return _new(mode, numch, bitdata)

	static func _new(mode: Mode, numch: int, bitdata: PackedByteArray) -> QrSegment:
		var seg := QrSegment.new()
		seg._mode = mode
		seg._numchars = numch
		seg._bitdata = bitdata.duplicate()  # Make defensive copy
		return seg

	# ---- Accessor methods ----

	## Returns the mode field of this segment.
	func get_mode() -> Mode:
		return _mode

	## Returns the character count field of this segment.
	func get_num_chars() -> int:
		return _numchars

	## Returns a new copy of the data bits of this segment.
	func get_data() -> PackedByteArray:
		return _bitdata.duplicate()  # Make defensive copy

	## Calculates the number of bits needed to encode the given segments at
	## the given version. Returns a non-negative number if successful. Otherwise
	## returns null (Python's None) if a segment has too many characters to fit its length field.
	static func get_total_bits(segs: Array, version: int) -> Variant:
		var result := 0
		for seg: QrSegment in segs:
			var ccbits: int = seg.get_mode().num_char_count_bits(version)
			if seg.get_num_chars() >= (1 << ccbits):
				return null  # The segment's length doesn't fit the field's bit width
			result += 4 + ccbits + seg._bitdata.size()
		return result


# ---- Private helper functions ----

# Python's _BitBuffer.append_bits: appends the given number of low-order bits of the given value
# to this buffer. Requires n >= 0 and 0 <= val < 2^n (every caller guarantees it).
static func _append_bits(bb: PackedByteArray, val: int, n: int) -> void:
	for i in range(n - 1, -1, -1):
		bb.append((val >> i) & 1)


# Returns true iff the i'th bit of x is set to 1.
static func _get_bit(x: int, i: int) -> bool:
	return (x >> i) & 1 != 0
