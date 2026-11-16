// Twin of scfg_core/_qrcodegen.py (itself Project Nayuki's QR Code generator library).
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
// Ported to C# for the SCFG core. Only what Paylinks.QrMatrix reaches is ported: EncodeText (numeric,
// alphanumeric and byte segments, chosen as Python's make_segments does), EncodeSegments, the QrCode
// constructor and everything below it. Left out: encode_binary, make_eci, the kanji mode, the public
// make_* segment builders, the version/mask/ecc accessors and the public low-level constructor.
using System;
using System.Collections.Generic;
using System.Globalization;

namespace Scfg.Core
{
    /// <summary>Raised when the data fits no QR Code version (Python's DataTooLongError, a ValueError).</summary>
    public sealed class DataTooLongError : ValueError
    {
        public DataTooLongError(string message) : base(message) { }
    }

    /// <summary>The error correction level in a QR Code symbol.</summary>
    public sealed class QrEcc
    {
        internal int Ordinal { get; }
        internal int FormatBits { get; }

        private QrEcc(int ordinal, int formatBits)
        {
            Ordinal = ordinal;
            FormatBits = formatBits;
        }

        public static readonly QrEcc Low = new QrEcc(0, 1);
        public static readonly QrEcc Medium = new QrEcc(1, 0);
        public static readonly QrEcc Quartile = new QrEcc(2, 3);
        public static readonly QrEcc High = new QrEcc(3, 2);
    }

    /// <summary>A QR Code symbol: an immutable square grid of dark and light modules.</summary>
    public sealed class QrCode
    {
        private const int MinVersion = 1;
        private const int MaxVersion = 40;
        private const int PenaltyN1 = 3;
        private const int PenaltyN2 = 3;
        private const int PenaltyN3 = 40;
        private const int PenaltyN4 = 10;

        private static readonly int[][] EccCodewordsPerBlock =
        {
            new[] { -1, 7, 10, 15, 20, 26, 18, 20, 24, 30, 18, 20, 24, 26, 30, 22, 24, 28, 30, 28, 28, 28, 28, 30, 30, 26, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30 },
            new[] { -1, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26, 30, 22, 22, 24, 24, 28, 28, 26, 26, 26, 26, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28 },
            new[] { -1, 13, 22, 18, 26, 18, 24, 18, 22, 20, 24, 28, 26, 24, 20, 30, 24, 28, 28, 26, 30, 28, 30, 30, 30, 30, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30 },
            new[] { -1, 17, 28, 22, 16, 22, 28, 26, 26, 24, 28, 24, 28, 22, 24, 24, 30, 28, 28, 26, 28, 30, 24, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30 },
        };

        private static readonly int[][] NumErrorCorrectionBlocks =
        {
            new[] { -1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 4, 4, 4, 4, 4, 6, 6, 6, 6, 7, 8, 8, 9, 9, 10, 12, 12, 12, 13, 14, 15, 16, 17, 18, 19, 19, 20, 21, 22, 24, 25 },
            new[] { -1, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5, 5, 8, 9, 9, 10, 10, 11, 13, 14, 16, 17, 17, 18, 20, 21, 23, 25, 26, 28, 29, 31, 33, 35, 37, 38, 40, 43, 45, 47, 49 },
            new[] { -1, 1, 1, 2, 2, 4, 4, 6, 6, 8, 8, 8, 10, 12, 16, 12, 17, 16, 18, 21, 20, 23, 23, 25, 27, 29, 34, 34, 35, 38, 40, 43, 45, 48, 51, 53, 56, 59, 62, 65, 68 },
            new[] { -1, 1, 1, 2, 4, 4, 4, 5, 6, 8, 8, 11, 11, 16, 16, 18, 16, 19, 21, 25, 25, 25, 34, 30, 32, 35, 37, 40, 42, 45, 48, 51, 54, 57, 60, 63, 66, 70, 74, 77, 81 },
        };

        private readonly int _version;
        private readonly int _size;
        private readonly QrEcc _errcorlvl;
        private readonly bool[][] _modules;
        private bool[][]? _isFunction;

        // ---- Static factory functions ----

        /// <summary>
        /// A QR Code for the given Unicode text at the given error correction level (boosted to a higher level
        /// if the version does not grow), with the smallest version that fits and the best mask.
        /// </summary>
        public static QrCode EncodeText(string text, QrEcc ecl) => EncodeSegments(Segment.MakeSegments(text), ecl);

        private static QrCode EncodeSegments(List<Segment> segs, QrEcc ecl)
        {
            int version = MinVersion;
            int dataUsedBits;
            for (; ; version++)
            {
                int dataCapacityBits = GetNumDataCodewords(version, ecl) * 8;
                int? used = Segment.GetTotalBits(segs, version);
                if (used.HasValue && used.Value <= dataCapacityBits)
                {
                    dataUsedBits = used.Value;
                    break;
                }
                if (version >= MaxVersion)
                {
                    string msg = "Segment too long";
                    if (used.HasValue)
                    {
                        msg = "Data length = " + used.Value.ToString(CultureInfo.InvariantCulture)
                              + " bits, Max capacity = " + dataCapacityBits.ToString(CultureInfo.InvariantCulture)
                              + " bits";
                    }
                    throw new DataTooLongError(msg);
                }
            }

            foreach (var newEcl in new[] { QrEcc.Medium, QrEcc.Quartile, QrEcc.High })
            {
                if (dataUsedBits <= GetNumDataCodewords(version, newEcl) * 8) ecl = newEcl;
            }

            var bb = new List<int>();
            foreach (var seg in segs)
            {
                AppendBits(bb, seg.ModeBits, 4);
                AppendBits(bb, seg.NumChars, seg.NumCharCountBits(version));
                bb.AddRange(seg.BitData);
            }

            int capacity = GetNumDataCodewords(version, ecl) * 8;
            AppendBits(bb, 0, Math.Min(4, capacity - bb.Count));
            AppendBits(bb, 0, (8 - bb.Count % 8) % 8);
            for (int padByte = 0xEC; bb.Count < capacity; padByte ^= 0xEC ^ 0x11)
                AppendBits(bb, padByte, 8);

            var dataCodewords = new byte[bb.Count / 8];
            for (int i = 0; i < bb.Count; i++)
                dataCodewords[i >> 3] |= (byte)(bb[i] << (7 - (i & 7)));

            return new QrCode(version, ecl, dataCodewords);
        }

        internal static void AppendBits(List<int> bb, int val, int n)
        {
            if (n < 0 || (val >> n) != 0) throw new ValueError("Value out of range");
            for (int i = n - 1; i >= 0; i--) bb.Add((val >> i) & 1);
        }

        // ---- Constructor ----

        private QrCode(int version, QrEcc errcorlvl, byte[] dataCodewords)
        {
            _version = version;
            _size = version * 4 + 17;
            _errcorlvl = errcorlvl;
            _modules = NewGrid(_size);
            _isFunction = NewGrid(_size);

            DrawFunctionPatterns();
            byte[] allCodewords = AddEccAndInterleave(dataCodewords);
            DrawCodewords(allCodewords);

            int msk = -1;
            long minPenalty = 1L << 32;
            for (int i = 0; i < 8; i++)
            {
                ApplyMask(i);
                DrawFormatBits(i);
                int penalty = GetPenaltyScore();
                if (penalty < minPenalty)
                {
                    msk = i;
                    minPenalty = penalty;
                }
                ApplyMask(i);
            }
            ApplyMask(msk);
            DrawFormatBits(msk);
            _isFunction = null;
        }

        private static bool[][] NewGrid(int size)
        {
            var g = new bool[size][];
            for (int i = 0; i < size; i++) g[i] = new bool[size];
            return g;
        }

        // ---- Accessors ----

        public int Size => _size;

        /// <summary>The module at (x, y): false = light, true = dark. Out of bounds is light.</summary>
        public bool GetModule(int x, int y) => 0 <= x && x < _size && 0 <= y && y < _size && _modules[y][x];

        // ---- Drawing function modules ----

        private void DrawFunctionPatterns()
        {
            for (int i = 0; i < _size; i++)
            {
                SetFunctionModule(6, i, i % 2 == 0);
                SetFunctionModule(i, 6, i % 2 == 0);
            }
            DrawFinderPattern(3, 3);
            DrawFinderPattern(_size - 4, 3);
            DrawFinderPattern(3, _size - 4);

            int[] alignPatPos = GetAlignmentPatternPositions();
            int numAlign = alignPatPos.Length;
            for (int i = 0; i < numAlign; i++)
            {
                for (int j = 0; j < numAlign; j++)
                {
                    bool skip = (i == 0 && j == 0) || (i == 0 && j == numAlign - 1) || (i == numAlign - 1 && j == 0);
                    if (!skip) DrawAlignmentPattern(alignPatPos[i], alignPatPos[j]);
                }
            }
            DrawFormatBits(0);
            DrawVersion();
        }

        private void DrawFormatBits(int mask)
        {
            int data = (_errcorlvl.FormatBits << 3) | mask;
            int rem = data;
            for (int i = 0; i < 10; i++) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
            int bits = ((data << 10) | rem) ^ 0x5412;

            for (int i = 0; i < 6; i++) SetFunctionModule(8, i, GetBit(bits, i));
            SetFunctionModule(8, 7, GetBit(bits, 6));
            SetFunctionModule(8, 8, GetBit(bits, 7));
            SetFunctionModule(7, 8, GetBit(bits, 8));
            for (int i = 9; i < 15; i++) SetFunctionModule(14 - i, 8, GetBit(bits, i));

            for (int i = 0; i < 8; i++) SetFunctionModule(_size - 1 - i, 8, GetBit(bits, i));
            for (int i = 8; i < 15; i++) SetFunctionModule(8, _size - 15 + i, GetBit(bits, i));
            SetFunctionModule(8, _size - 8, true);
        }

        private void DrawVersion()
        {
            if (_version < 7) return;
            int rem = _version;
            for (int i = 0; i < 12; i++) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
            int bits = (_version << 12) | rem;
            for (int i = 0; i < 18; i++)
            {
                bool bit = GetBit(bits, i);
                int a = _size - 11 + i % 3;
                int b = i / 3;
                SetFunctionModule(a, b, bit);
                SetFunctionModule(b, a, bit);
            }
        }

        private void DrawFinderPattern(int x, int y)
        {
            for (int dy = -4; dy <= 4; dy++)
            {
                for (int dx = -4; dx <= 4; dx++)
                {
                    int xx = x + dx, yy = y + dy;
                    if (0 <= xx && xx < _size && 0 <= yy && yy < _size)
                    {
                        int dist = Math.Max(Math.Abs(dx), Math.Abs(dy));
                        SetFunctionModule(xx, yy, dist != 2 && dist != 4);
                    }
                }
            }
        }

        private void DrawAlignmentPattern(int x, int y)
        {
            for (int dy = -2; dy <= 2; dy++)
                for (int dx = -2; dx <= 2; dx++)
                    SetFunctionModule(x + dx, y + dy, Math.Max(Math.Abs(dx), Math.Abs(dy)) != 1);
        }

        private void SetFunctionModule(int x, int y, bool isDark)
        {
            _modules[y][x] = isDark;
            _isFunction![y][x] = true;
        }

        // ---- Codewords and masking ----

        private byte[] AddEccAndInterleave(byte[] data)
        {
            int numBlocks = NumErrorCorrectionBlocks[_errcorlvl.Ordinal][_version];
            int blockEccLen = EccCodewordsPerBlock[_errcorlvl.Ordinal][_version];
            int rawCodewords = GetNumRawDataModules(_version) / 8;
            int numShortBlocks = numBlocks - rawCodewords % numBlocks;
            int shortBlockLen = rawCodewords / numBlocks;

            var blocks = new List<byte[]>();
            byte[] rsDiv = ReedSolomonComputeDivisor(blockEccLen);
            int k = 0;
            for (int i = 0; i < numBlocks; i++)
            {
                int datLen = shortBlockLen - blockEccLen + (i < numShortBlocks ? 0 : 1);
                var dat = new byte[datLen];
                Array.Copy(data, k, dat, 0, datLen);
                k += datLen;
                byte[] ecc = ReedSolomonComputeRemainder(dat, rsDiv);
                // dat (+ a zero pad byte in a short block) + ecc: every block ends up shortBlockLen + 1 long
                var blk = new byte[shortBlockLen + 1];
                Array.Copy(dat, 0, blk, 0, datLen);
                Array.Copy(ecc, 0, blk, i < numShortBlocks ? datLen + 1 : datLen, ecc.Length);
                blocks.Add(blk);
            }

            var result = new List<byte>(rawCodewords);
            for (int i = 0; i < blocks[0].Length; i++)
            {
                for (int j = 0; j < blocks.Count; j++)
                {
                    // skip the padding byte in short blocks
                    if (i != shortBlockLen - blockEccLen || j >= numShortBlocks) result.Add(blocks[j][i]);
                }
            }
            return result.ToArray();
        }

        private void DrawCodewords(byte[] data)
        {
            int i = 0;
            for (int col = _size - 1; col > 0; col -= 2)
            {
                int right = col <= 6 ? col - 1 : col; // the vertical timing column is skipped
                for (int vert = 0; vert < _size; vert++)
                {
                    for (int j = 0; j < 2; j++)
                    {
                        int x = right - j;
                        bool upward = ((right + 1) & 2) == 0;
                        int y = upward ? _size - 1 - vert : vert;
                        if (!_isFunction![y][x] && i < data.Length * 8)
                        {
                            _modules[y][x] = GetBit(data[i >> 3], 7 - (i & 7));
                            i++;
                        }
                    }
                }
            }
        }

        private void ApplyMask(int mask)
        {
            for (int y = 0; y < _size; y++)
            {
                for (int x = 0; x < _size; x++)
                {
                    bool invert;
                    switch (mask)
                    {
                        case 0: invert = (x + y) % 2 == 0; break;
                        case 1: invert = y % 2 == 0; break;
                        case 2: invert = x % 3 == 0; break;
                        case 3: invert = (x + y) % 3 == 0; break;
                        case 4: invert = (x / 3 + y / 2) % 2 == 0; break;
                        case 5: invert = x * y % 2 + x * y % 3 == 0; break;
                        case 6: invert = (x * y % 2 + x * y % 3) % 2 == 0; break;
                        case 7: invert = ((x + y) % 2 + x * y % 3) % 2 == 0; break;
                        default: throw new ValueError("Mask value out of range");
                    }
                    _modules[y][x] ^= invert && !_isFunction![y][x];
                }
            }
        }

        private int GetPenaltyScore()
        {
            int result = 0;
            int size = _size;
            bool[][] modules = _modules;
            var runHistory = new int[7];

            for (int y = 0; y < size; y++)
            {
                bool runColor = false;
                int runX = 0;
                Array.Clear(runHistory, 0, 7);
                for (int x = 0; x < size; x++)
                {
                    if (modules[y][x] == runColor)
                    {
                        runX++;
                        if (runX == 5) result += PenaltyN1;
                        else if (runX > 5) result++;
                    }
                    else
                    {
                        FinderPenaltyAddHistory(runX, runHistory);
                        if (!runColor) result += FinderPenaltyCountPatterns(runHistory) * PenaltyN3;
                        runColor = modules[y][x];
                        runX = 1;
                    }
                }
                result += FinderPenaltyTerminateAndCount(runColor, runX, runHistory) * PenaltyN3;
            }

            for (int x = 0; x < size; x++)
            {
                bool runColor = false;
                int runY = 0;
                Array.Clear(runHistory, 0, 7);
                for (int y = 0; y < size; y++)
                {
                    if (modules[y][x] == runColor)
                    {
                        runY++;
                        if (runY == 5) result += PenaltyN1;
                        else if (runY > 5) result++;
                    }
                    else
                    {
                        FinderPenaltyAddHistory(runY, runHistory);
                        if (!runColor) result += FinderPenaltyCountPatterns(runHistory) * PenaltyN3;
                        runColor = modules[y][x];
                        runY = 1;
                    }
                }
                result += FinderPenaltyTerminateAndCount(runColor, runY, runHistory) * PenaltyN3;
            }

            for (int y = 0; y < size - 1; y++)
            {
                for (int x = 0; x < size - 1; x++)
                {
                    bool c = modules[y][x];
                    if (c == modules[y][x + 1] && c == modules[y + 1][x] && c == modules[y + 1][x + 1])
                        result += PenaltyN2;
                }
            }

            int dark = 0;
            foreach (var row in modules)
                foreach (bool cell in row)
                    if (cell) dark++;
            int total = size * size;
            int kk = (Math.Abs(dark * 20 - total * 10) + total - 1) / total - 1;
            result += kk * PenaltyN4;
            return result;
        }

        // ---- Private helper functions ----

        private int[] GetAlignmentPatternPositions()
        {
            if (_version == 1) return new int[0];
            int numAlign = _version / 7 + 2;
            int step = (_version * 8 + numAlign * 3 + 5) / (numAlign * 4 - 4) * 2;
            var result = new int[numAlign];
            result[0] = 6;
            for (int i = 0; i < numAlign - 1; i++) result[numAlign - 1 - i] = _size - 7 - i * step;
            return result;
        }

        private static int GetNumRawDataModules(int ver)
        {
            if (ver < MinVersion || ver > MaxVersion) throw new ValueError("Version number out of range");
            int result = (16 * ver + 128) * ver + 64;
            if (ver >= 2)
            {
                int numAlign = ver / 7 + 2;
                result -= (25 * numAlign - 10) * numAlign - 55;
                if (ver >= 7) result -= 36;
            }
            return result;
        }

        private static int GetNumDataCodewords(int ver, QrEcc ecl) =>
            GetNumRawDataModules(ver) / 8 - EccCodewordsPerBlock[ecl.Ordinal][ver] * NumErrorCorrectionBlocks[ecl.Ordinal][ver];

        private static byte[] ReedSolomonComputeDivisor(int degree)
        {
            if (degree < 1 || degree > 255) throw new ValueError("Degree out of range");
            var result = new byte[degree];
            result[degree - 1] = 1;
            int root = 1;
            for (int i = 0; i < degree; i++)
            {
                for (int j = 0; j < degree; j++)
                {
                    result[j] = (byte)ReedSolomonMultiply(result[j], root);
                    if (j + 1 < degree) result[j] ^= result[j + 1];
                }
                root = ReedSolomonMultiply(root, 0x02);
            }
            return result;
        }

        private static byte[] ReedSolomonComputeRemainder(byte[] data, byte[] divisor)
        {
            var result = new byte[divisor.Length];
            foreach (byte b in data)
            {
                int factor = b ^ result[0];
                Array.Copy(result, 1, result, 0, result.Length - 1);
                result[result.Length - 1] = 0;
                for (int i = 0; i < divisor.Length; i++)
                    result[i] ^= (byte)ReedSolomonMultiply(divisor[i], factor);
            }
            return result;
        }

        private static int ReedSolomonMultiply(int x, int y)
        {
            if ((x >> 8) != 0 || (y >> 8) != 0) throw new ValueError("Byte out of range");
            int z = 0;
            for (int i = 7; i >= 0; i--)
            {
                z = (z << 1) ^ ((z >> 7) * 0x11D);
                z ^= ((y >> i) & 1) * x;
            }
            return z;
        }

        private static int FinderPenaltyCountPatterns(int[] runHistory)
        {
            int n = runHistory[1];
            bool core = n > 0 && runHistory[2] == n && runHistory[3] == n * 3 && runHistory[4] == n && runHistory[5] == n;
            return (core && runHistory[0] >= n * 4 && runHistory[6] >= n ? 1 : 0)
                 + (core && runHistory[6] >= n * 4 && runHistory[0] >= n ? 1 : 0);
        }

        private int FinderPenaltyTerminateAndCount(bool currentRunColor, int currentRunLength, int[] runHistory)
        {
            if (currentRunColor)
            {
                FinderPenaltyAddHistory(currentRunLength, runHistory);
                currentRunLength = 0;
            }
            currentRunLength += _size;
            FinderPenaltyAddHistory(currentRunLength, runHistory);
            return FinderPenaltyCountPatterns(runHistory);
        }

        private void FinderPenaltyAddHistory(int currentRunLength, int[] runHistory)
        {
            if (runHistory[0] == 0) currentRunLength += _size;
            Array.Copy(runHistory, 0, runHistory, 1, runHistory.Length - 1);
            runHistory[0] = currentRunLength;
        }

        private static bool GetBit(int x, int i) => ((x >> i) & 1) != 0;

        // ---- Data segments ----

        private sealed class Segment
        {
            private const string AlphanumericCharset = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:";

            // character-count widths for versions 1-9, 10-26 and 27-40
            private readonly int[] _charCounts;

            internal int ModeBits { get; }
            internal int NumChars { get; }
            internal List<int> BitData { get; }

            private Segment(int modeBits, int[] charCounts, int numChars, List<int> bitData)
            {
                ModeBits = modeBits;
                _charCounts = charCounts;
                NumChars = numChars;
                BitData = bitData;
            }

            internal int NumCharCountBits(int ver) => _charCounts[(ver + 7) / 17];

            /// <summary>Index of c in the alphanumeric table, or -1.</summary>
            private static int AlphaIndex(char c)
            {
                for (int i = 0; i < AlphanumericCharset.Length; i++)
                    if (AlphanumericCharset[i] == c) return i;
                return -1;
            }

            private static bool IsNumeric(string text)
            {
                foreach (char c in text)
                    if (c < '0' || c > '9') return false;
                return true;
            }

            private static bool IsAlphanumeric(string text)
            {
                foreach (char c in text)
                    if (AlphaIndex(c) < 0) return false;
                return true;
            }

            internal static List<Segment> MakeSegments(string text)
            {
                var segs = new List<Segment>();
                if (text.Length == 0) return segs;
                if (IsNumeric(text)) segs.Add(MakeNumeric(text));
                else if (IsAlphanumeric(text)) segs.Add(MakeAlphanumeric(text));
                else segs.Add(MakeBytes(Py.Utf8Bytes(text)));
                return segs;
            }

            private static Segment MakeBytes(byte[] data)
            {
                var bb = new List<int>(data.Length * 8);
                foreach (byte b in data) AppendBits(bb, b, 8);
                return new Segment(0x4, new[] { 8, 16, 16 }, data.Length, bb);
            }

            private static Segment MakeNumeric(string digits)
            {
                var bb = new List<int>();
                int i = 0;
                while (i < digits.Length)
                {
                    int n = Math.Min(digits.Length - i, 3);
                    int v = 0;
                    for (int k = 0; k < n; k++) v = v * 10 + (digits[i + k] - '0');
                    AppendBits(bb, v, n * 3 + 1);
                    i += n;
                }
                return new Segment(0x1, new[] { 10, 12, 14 }, digits.Length, bb);
            }

            private static Segment MakeAlphanumeric(string text)
            {
                var bb = new List<int>();
                int i = 0;
                for (; i + 1 < text.Length; i += 2)
                    AppendBits(bb, AlphaIndex(text[i]) * 45 + AlphaIndex(text[i + 1]), 11);
                if (text.Length % 2 > 0) AppendBits(bb, AlphaIndex(text[text.Length - 1]), 6);
                return new Segment(0x2, new[] { 9, 11, 13 }, text.Length, bb);
            }

            /// <summary>Bits needed at the given version, or null when a segment's length overflows its count field.</summary>
            internal static int? GetTotalBits(List<Segment> segs, int version)
            {
                int result = 0;
                foreach (var seg in segs)
                {
                    int ccBits = seg.NumCharCountBits(version);
                    if (seg.NumChars >= (1 << ccBits)) return null;
                    result += 4 + ccBits + seg.BitData.Count;
                }
                return result;
            }
        }
    }
}
