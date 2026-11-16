// Twin of scfg_core/solana/paylinks.py (and of rpgmaker/src/core/solana/paylinks.ts).
// Solana Pay transfer-request links (https://docs.solanapay.com/spec).
#nullable enable
using System.Numerics;
using System.Text;

namespace Scfg.Core.Solana
{
    public static class SolanaPaylinks
    {
        /// <summary>The amount always carries 6 decimals, so the item code in it survives. Empty values are omitted.</summary>
        public static string SolanaPayUri(string recipient, BigInteger units, string mint, string reference,
            string label, string message)
        {
            var q = new StringBuilder();
            void Add(string k, string v)
            {
                if (v.Length == 0) return;
                if (q.Length > 0) q.Append('&');
                q.Append(k).Append('=').Append(Py.Quote(v, ""));
            }
            Add("amount", Amounts.FormatUnits(units));
            Add("spl-token", mint);
            Add("reference", reference);
            Add("label", label);
            Add("message", message);
            return "solana:" + recipient + "?" + q.ToString(); // culture-ok: StringBuilder
        }
    }
}
