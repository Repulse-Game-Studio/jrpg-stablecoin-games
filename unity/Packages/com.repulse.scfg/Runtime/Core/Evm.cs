// Twin of scfg_core/evm.py (and of rpgmaker/src/core/evm.ts).
// EVM helpers: addresses (EIP-55) and event topics.
using System.Collections.Generic;
using System.Numerics;
using System.Text;

namespace Scfg.Core
{
    public static class Evm
    {
        /// <summary>topic0 of ERC-20/721 <c>Transfer(address,address,uint256)</c>.</summary>
        public static readonly string TransferTopic =
            "0x" + Bytes.ToHex(Keccak.Keccak256(Bytes.AsciiBytes("Transfer(address,address,uint256)")));

        // Python: re.fullmatch(r"0x[0-9a-fA-F]{n}", s) (a trailing newline is not part of an address).
        private static bool IsHexWord(string s, int digits)
        {
            if (s.Length != digits + 2 || s[0] != '0' || s[1] != 'x') return false;
            for (int i = 2; i < s.Length; i++)
            {
                char c = s[i];
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
            }
            return true;
        }

        /// <summary>
        /// The EIP-55 mixed-case form of a 20-byte hex address (any input case). Anything but a <c>0x</c> and 40
        /// hex digits (a non-string included) is a ValueError.
        /// </summary>
        public static string ToChecksumAddress(object? address)
        {
            if (!(address is string a) || !IsHexWord(a, 40))
                throw new ValueError("not a 20-byte hex address: " + Py.Repr(address));
            var lower = a.Substring(2).ToLowerInvariant(); // hex digits only: ASCII
            var digest = Bytes.ToHex(Keccak.Keccak256(Bytes.AsciiBytes(lower)));
            var sb = new StringBuilder("0x", 42);
            for (int i = 0; i < lower.Length; i++)
            {
                char c = lower[i];
                bool up = digest[i] >= '8' && c >= 'a'; // the digest is lowercase hex: its nibble is >= 8 for '8'..'f'
                sb.Append(up ? (char)(c - 32) : c);
            }
            return sb.ToString(); // culture-ok: a StringBuilder of ASCII
        }

        /// <summary>True only if the address is already exactly in EIP-55 form. Never throws.</summary>
        public static bool IsChecksumAddress(object? address)
        {
            try
            {
                return ToChecksumAddress(address) == (string)address!;
            }
            catch (ValueError)
            {
                return false;
            }
        }

        /// <summary>
        /// Validate a user-supplied address and return its checksummed form. Single-case input carries no checksum,
        /// so it is accepted and converted. Mixed-case input claims a checksum, so it must be correct.
        /// </summary>
        public static string NormalizeAddress(object? address)
        {
            var checksummed = ToChecksumAddress(address);
            var body = ((string)address!).Substring(2);
            if (body == body.ToLowerInvariant() || body == body.ToUpperInvariant()) return checksummed;
            if (checksummed != (string)address!) throw new ValueError("bad EIP-55 checksum: " + (string)address!);
            return checksummed;
        }

        /// <summary>The address left-padded to 32 bytes, as it appears in indexed event topics.</summary>
        public static string AddressTopic(object? address) =>
            "0x" + new string('0', 24) + ToChecksumAddress(address).Substring(2).ToLowerInvariant();

        private static string TopicToAddress(object? topic)
        {
            if (!(topic is string t)) throw new TypeError("'" + Py.TypeName(topic) + "' object is not subscriptable");
            return ToChecksumAddress("0x" + (t.Length > 40 ? t.Substring(t.Length - 40) : t));
        }

        // Python's int(text, 16): int()'s whitespace, an optional sign, an optional 0x/0X prefix, then hex digits
        // (any script's decimal digits count) with single underscores between them (also right after the prefix).
        private static BigInteger IntHex(object? text)
        {
            if (!(text is string s)) throw new TypeError("int() can't convert non-string with explicit base");
            int start = 0, end = s.Length;
            while (start < end && Py.IsIntSpace(s[start])) start++;
            while (end > start && Py.IsIntSpace(s[end - 1])) end--;
            int i = start;
            bool negative = false;
            if (i < end && (s[i] == '+' || s[i] == '-'))
            {
                negative = s[i] == '-';
                i++;
            }
            bool prefixed = false;
            if (i + 1 < end && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X'))
            {
                i += 2;
                prefixed = true;
            }
            BigInteger value = BigInteger.Zero;
            bool any = false;
            bool canUnderscore = prefixed; // after a digit, or right after the prefix
            while (i < end)
            {
                int cp = Py.CodePointAt(s, i, out int w);
                i += w;
                if (cp == '_')
                {
                    if (!canUnderscore) throw BadHex(s);
                    canUnderscore = false;
                    continue;
                }
                int d = Py.DecimalValue(cp);
                if (d < 0)
                {
                    if (cp >= 'a' && cp <= 'f') d = cp - 'a' + 10;
                    else if (cp >= 'A' && cp <= 'F') d = cp - 'A' + 10;
                    else throw BadHex(s);
                }
                value = value * 16 + d;
                any = true;
                canUnderscore = true;
            }
            if (!any || !canUnderscore) throw BadHex(s);
            return negative ? -value : value;
        }

        private static ValueError BadHex(string s) => new ValueError("invalid literal for int() with base 16: " + Py.Repr(s));

        private static long ToLong(BigInteger v, string name)
        {
            if (v < long.MinValue || v > long.MaxValue) throw new ValueError("log " + name + " does not fit 64 bits");
            return (long)v;
        }

        private static string Lower(object? v, string name)
        {
            if (!(v is string s)) throw new TypeError("log " + name + " must be a string");
            return s.ToLowerInvariant();
        }

        /// <summary>
        /// Parse a raw JSON-RPC log into a TransferLog, or null if it isn't an ERC-20 Transfer. ERC-721 Transfer
        /// events share topic0 but have 4 topics (the token id is indexed), so requiring exactly 3 topics filters
        /// them out. The data must be exactly the 32-byte value ("0x" and 64 hex digits): an empty, shorter, longer
        /// or malformed one is not an ERC-20 Transfer either. A null log is a TypeError, a missing field a KeyError,
        /// and (where Python raises AttributeError) a field of the wrong type a TypeError. A log index or block
        /// number beyond 64 bits is a ValueError (Python's ints are unbounded).
        /// </summary>
        public static TransferLog? ParseTransferLog(JsonObject log)
        {
            if (log == null) throw new TypeError("'NoneType' object has no attribute 'get'");
            var topicsValue = log.Get("topics");
            List<object?> topics;
            if (topicsValue is List<object?> list) topics = list;
            else if (!Py.Truthy(topicsValue)) topics = new List<object?>();
            else if (topicsValue is string) return null; // len() works, but no character equals the topic (or removed wins): not a Transfer
            else throw new TypeError("log topics must be a list");
            if (Py.Truthy(log.Get("removed"))) return null;
            if (topics.Count != 3) return null;
            if (!(topics[0] is string t0)) throw new TypeError("log topic must be a string");
            if (t0.ToLowerInvariant() != TransferTopic) return null;
            var data = log.Get("data");
            if (!(data is string d) || !IsHexWord(d, 64)) return null; // not exactly the 32-byte value
            var token = ToChecksumAddress(Py.Get(log, "address"));
            var sender = TopicToAddress(topics[1]);
            var recipient = TopicToAddress(topics[2]);
            var value = new BigInteger(Bytes.FromHex(d), isUnsigned: true, isBigEndian: true);
            var txHash = Lower(Py.Get(log, "transactionHash"), "transactionHash");
            var logIndex = ToLong(IntHex(Py.Get(log, "logIndex")), "logIndex");
            var blockNumber = ToLong(IntHex(Py.Get(log, "blockNumber")), "blockNumber");
            var blockHash = Lower(Py.Get(log, "blockHash"), "blockHash");
            return new TransferLog(token, sender, recipient, value, txHash, logIndex, blockNumber, blockHash);
        }
    }
}
