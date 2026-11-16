// Twin of scfg_core/paylinks.py (and of rpgmaker/src/core/paylinks.ts).
// Payment links (EIP-681) and QR code geometry. The QR matrix comes from QrCodeGen.cs (Project Nayuki's
// generator). Rendering is left to the engine: QrRects merges each row's dark modules into horizontal runs.
using System.Collections.Generic;
using System.Globalization;
using System.Numerics;

namespace Scfg.Core
{
    /// <summary>One dark run of <see cref="Paylinks.QrRects"/>: (x, y, width), the border already added.</summary>
    public readonly struct QrRun
    {
        public int X { get; }
        public int Y { get; }
        public int Width { get; }

        public QrRun(int x, int y, int width)
        {
            X = x;
            Y = y;
            Width = width;
        }
    }

    public static class Paylinks
    {
        /// <summary>ERC-20 transfer request that mobile wallets understand when scanned or opened.</summary>
        public static string Eip681TransferUri(string tokenAddress, long chainId, string recipient, BigInteger units) =>
            "ethereum:" + tokenAddress + "@" + chainId.ToString(CultureInfo.InvariantCulture) + "/transfer?address="
            + recipient + "&uint256=" + units.ToString(CultureInfo.InvariantCulture);

        /// <summary>Square matrix of modules, true = dark. Medium error correction.</summary>
        public static bool[][] QrMatrix(string text)
        {
            var qr = QrCode.EncodeText(text, QrEcc.Medium);
            int n = qr.Size;
            var m = new bool[n][];
            for (int y = 0; y < n; y++)
            {
                m[y] = new bool[n];
                for (int x = 0; x < n; x++) m[y][x] = qr.GetModule(x, y);
            }
            return m;
        }

        /// <summary>(size in modules including the quiet-zone border, dark runs).</summary>
        public static (int Size, List<QrRun> Runs) QrRects(string text, int border = 2)
        {
            bool[][] matrix = QrMatrix(text);
            int n = matrix.Length;
            var runs = new List<QrRun>();
            for (int y = 0; y < n; y++)
            {
                bool[] row = matrix[y];
                int x = 0;
                while (x < n)
                {
                    if (row[x])
                    {
                        int start = x;
                        while (x < n && row[x]) x++;
                        runs.Add(new QrRun(start + border, y + border, x - start));
                    }
                    else
                    {
                        x++;
                    }
                }
            }
            return (n + 2 * border, runs);
        }
    }
}
