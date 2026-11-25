// Twin of scfg_core/paylinks.py (and of rpgmaker/src/core/paylinks.ts).
// Payment links (EIP-681), the Pay with card page (onramp) and QR code geometry. The QR matrix comes from QrCodeGen.cs
// (Project Nayuki's generator). Rendering is left to the engine: QrRects merges each row's dark modules into
// horizontal runs.
#nullable enable
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

    /// <summary>One <c>{name}</c> of an onramp template: [Start, End) in the string, and the name between the braces.</summary>
    public readonly struct OnrampPlaceholder
    {
        public int Start { get; }
        public int End { get; }
        public string Name { get; }

        public OnrampPlaceholder(int start, int end, string name)
        {
            Start = start;
            End = end;
            Name = name;
        }
    }

    public static class Paylinks
    {
        /// <summary>
        /// The "Pay with card" page (configure's onramp): a template whose {placeholders} are filled in for the network
        /// the player picked and the payment's session. The default is thirdweb's hosted Checkout page, which needs no
        /// account or key: the player pays by card and thirdweb sends the token to <c>seller</c>, the developer's wallet,
        /// on that network. {amount} is the session's exact amount with all 6 decimals, so the game finds the payment as
        /// it finds any other. paymentMethods=card offers card payment only (paylinks.py). NOT YET CHECKED with a real
        /// card: that thirdweb delivers the amount exactly, to 6 decimals. (Python's ONRAMP_URL.)
        /// </summary>
        public const string DefaultOnrampUrl = "https://thirdweb.com/bridge/checkout-widget?chain={chain_id}"
                                               + "&tokenAddress={token_address}&amount={amount}&seller={recipient}"
                                               + "&paymentMethods=card";

        public static readonly IReadOnlyList<string> OnrampFields =
            System.Array.AsReadOnly(new[] { "amount", "chain", "chain_id", "recipient", "token", "token_address" });

        /// <summary>
        /// The matches of Python's ONRAMP_PLACEHOLDER, <c>\{([^{}]*)\}</c>, left to right without overlaps: a
        /// <c>{</c> whose next brace is a <c>}</c>.
        /// </summary>
        public static List<OnrampPlaceholder> OnrampPlaceholders(string template)
        {
            var found = new List<OnrampPlaceholder>();
            int i = 0;
            while (i < template.Length)
            {
                if (template[i] != '{')
                {
                    i++;
                    continue;
                }
                int j = i + 1;
                while (j < template.Length && template[j] != '{' && template[j] != '}') j++;
                if (j < template.Length && template[j] == '}')
                {
                    found.Add(new OnrampPlaceholder(i, j + 1, template.Substring(i + 1, j - i - 1)));
                    i = j + 1;
                }
                else
                {
                    i = j; // no match at i; the next try is the next brace (nothing between can start one)
                }
            }
            return found;
        }

        /// <summary>
        /// Fill a validated onramp template (Config's onramp url check). Every value is made of URL-safe characters (a
        /// chain key, digits, a 0x address, a token symbol, an amount, a wallet address), so none is escaped. A
        /// placeholder outside <see cref="OnrampFields"/> is Python's KeyError.
        /// </summary>
        public static string OnrampUrl(string template, string chain, BigInteger chainId, string token, string tokenAddress,
                                       string amount, string recipient)
        {
            var sb = new System.Text.StringBuilder(template.Length + 64);
            int at = 0;
            foreach (var p in OnrampPlaceholders(template))
            {
                sb.Append(template, at, p.Start - at);
                switch (p.Name)
                {
                    case "amount": sb.Append(amount); break;
                    case "chain": sb.Append(chain); break;
                    case "chain_id": sb.Append(chainId.ToString(CultureInfo.InvariantCulture)); break;
                    case "token": sb.Append(token); break;
                    case "token_address": sb.Append(tokenAddress); break;
                    case "recipient": sb.Append(recipient); break;
                    default: throw new KeyError(p.Name);
                }
                at = p.End;
            }
            sb.Append(template, at, template.Length - at);
            return sb.ToString(); // culture-ok: StringBuilder.ToString copies the characters
        }

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
