// Twin of scfg_core/rpc.py: the JSON-RPC client with endpoint failover (RpcClient), RpcError, RpcUnavailable,
// is_range_error, suggested_range and the error parsing. Modeled on rpgmaker/src/core/rpc.ts.
//
// Public RPC endpoints are free but unreliable: they rate-limit, go down, and cap eth_getLogs block ranges. The
// client tries endpoints in order on every call, skipping ones in cooldown (30 s doubling to 10 min, on the
// injected Clock), and remembers per endpoint the largest eth_getLogs range it accepts. Every network method is
// async and awaits the injected ITransport where Python's generator yields a Request. Response ids are never
// checked against request ids, as in Python.
//
// Not ported: UrllibTransport (a real transport is platform code; Coio.ParseRpcBody holds its rules).
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.Numerics;
using System.Text;
using System.Threading.Tasks;

namespace Scfg.Core
{
    /// <summary>
    /// Python <c>rpc.RpcError</c>: a JSON-RPC error object returned by the node. <see cref="Exception.Message"/>
    /// is the node's raw message (Python's <c>.message</c>); <see cref="PyStr"/> is Python's <c>str(e)</c>.
    /// </summary>
    public class RpcError : Exception
    {
        /// <summary>The error code as Python's int holds it (any size).</summary>
        public BigInteger FullCode { get; }

        /// <summary>
        /// The error code, or null when it does not fit a long (only a hostile node sends one; such a code equals
        /// no code the core looks for, as in Python).
        /// </summary>
        public long? Code => FullCode >= long.MinValue && FullCode <= long.MaxValue ? (long)FullCode : (long?)null;

        /// <summary>The endpoint that answered with this error (Python sets <c>err.url</c>); null if none did.</summary>
        public string? Url { get; set; }

        public RpcError(BigInteger code, string message) : base(message)
        {
            FullCode = code;
        }

        /// <summary>Python's <c>str(RpcError)</c>: "code: message".</summary>
        public string PyStr => FullCode.ToString(CultureInfo.InvariantCulture) + ": " + Message;
    }

    /// <summary>Python <c>rpc.RpcUnavailable</c>: every endpoint failed or is cooling down.</summary>
    public class RpcUnavailable : Exception
    {
        public RpcUnavailable(string message) : base(message) { }
    }

    /// <summary>
    /// What <see cref="RpcClient.TryAt"/> got from an endpoint that answered: Python's <c>("ok", result)</c> or
    /// <c>("error", RpcError)</c>. (An endpoint that failed is null, Python's None.)
    /// </summary>
    public sealed class RpcAttempt
    {
        public bool IsError { get; }

        /// <summary>The call's result; null for an error.</summary>
        public object? ResultValue { get; }

        /// <summary>The node's error; null for a result.</summary>
        public RpcError? Error { get; }

        private RpcAttempt(bool isError, object? result, RpcError? error)
        {
            IsError = isError;
            ResultValue = result;
            Error = error;
        }

        public static RpcAttempt Ok(object? result) => new RpcAttempt(false, result, null);

        public static RpcAttempt Failed(RpcError error) => new RpcAttempt(true, null, error);

        /// <summary>Python's <c>attempt[1]</c>: the result, or the RpcError.</summary>
        public object? Value => IsError ? Error : ResultValue;
    }

    public static class Rpc
    {
        public const double CooldownStart = 30.0;
        public const double CooldownMax = 600.0;

        /// <summary>The smallest eth_getLogs block range a chain may be configured with.</summary>
        public const long MinLogRange = 10;

        private static readonly string[] RateHints = { "rate limit", "too many requests", "rate-limit" };

        // Phrases providers use for "your eth_getLogs block range is too wide". Some say "archive" (publicnode
        // answers wide ranges with "Archive requests require a personal token").
        private static readonly string[] RangeHints =
            { "range", "too many", "limit", "exceed", "max results", "response size", "too large", "archive" };

        public static bool IsRangeError(RpcError err) => ContainsAny(PyLowerForAscii(err.Message), RangeHints);

        internal static bool IsRateError(RpcError err) => ContainsAny(PyLowerForAscii(err.Message), RateHints);

        private static bool ContainsAny(string m, string[] hints)
        {
            foreach (var h in hints)
                if (m.IndexOf(h, StringComparison.Ordinal) >= 0) return true;
            return false;
        }

        /// <summary>
        /// Python's <c>s.lower()</c> as far as a search for an ASCII phrase can tell: A-Z become a-z, U+212A
        /// (Kelvin) becomes k and U+0130 becomes "i\u0307", the only non-ASCII characters whose lowercase holds an
        /// ASCII character (checked over every code point with CPython 3.9). Every other character stays itself:
        /// it and its Python lowercase are both non-ASCII, so neither can match. Not .NET's ToLowerInvariant, which
        /// maps U+0130 to a bare i (so "L\u0130MIT" would contain "limit", which in Python it does not).
        /// </summary>
        internal static string PyLowerForAscii(string s)
        {
            StringBuilder? o = null;
            for (int i = 0; i < s.Length; i++)
            {
                char c = s[i];
                string? r = null;
                if (c >= 'A' && c <= 'Z') r = ((char)(c + 32)).ToString(); // culture-ok: a char
                else if (c == '\u212a') r = "k";
                else if (c == '\u0130') r = "i\u0307";
                if (r == null)
                {
                    o?.Append(c);
                    continue;
                }
                if (o == null) o = new StringBuilder(s, 0, i, s.Length + 8);
                o.Append(r);
            }
            return o == null ? s : o.ToString(); // culture-ok: StringBuilder
        }

        /// <summary>
        /// Python <c>suggested_range</c>: the next eth_getLogs range to try after <paramref name="err"/> rejected a
        /// <paramref name="tried"/>-block request. Uses the limit when the provider states one ("limited to 0 - 50
        /// blocks range"), otherwise halves. A number is a run of Python's <c>\d</c> (any Unicode 13 decimal
        /// digit); one of more than 18 significant digits is ignored (Ruling 6, <see cref="Ints.DigitRunValue"/>),
        /// so no long run is converted.
        /// </summary>
        public static long SuggestedRange(RpcError err, long tried)
        {
            long? best = null;
            foreach (var run in DigitRuns(err.Message))
            {
                var n = Ints.DigitRunValue(run);
                if (n != null && MinLogRange <= n && n < tried && (best == null || n > best)) best = n;
            }
            return best ?? Math.Max(MinLogRange, FloorDiv2(tried));
        }

        private static long FloorDiv2(long n) => n >= 0 ? n / 2 : -((-(n + 1)) / 2) - 1;

        /// <summary>Python's <c>re.findall(r"\d+", s)</c>: every maximal run of Python 3.9 decimal digits.</summary>
        internal static List<string> DigitRuns(string s)
        {
            var runs = new List<string>();
            int start = -1;
            for (int i = 0; i < s.Length;)
            {
                int cp = Py.CodePointAt(s, i, out int w);
                if (Py.IsDecimalDigit(cp))
                {
                    if (start < 0) start = i;
                }
                else if (start >= 0)
                {
                    runs.Add(s.Substring(start, i - start));
                    start = -1;
                }
                i += w;
            }
            if (start >= 0) runs.Add(s.Substring(start));
            return runs;
        }

        /// <summary>
        /// Python <c>_parse_error</c>: providers are inconsistent, the error may be an object, a bare string, or
        /// carry a non-numeric code. A code <c>int_from</c> cannot read (TypeError, ValueError) is 0; an infinite
        /// one is Python's OverflowError, which is not caught. The message is <c>str(message)</c>.
        /// </summary>
        internal static RpcError ParseError(object? e)
        {
            object? code, message;
            if (e is JsonObject d)
            {
                code = d.Get("code", 0L);
                message = d.Get("message", "");
            }
            else
            {
                code = 0L;
                message = e;
            }
            BigInteger n;
            try
            {
                n = Ints.IntFrom(code);
            }
            catch (Exception x) when (x is TypeError || x is ValueError)
            {
                n = BigInteger.Zero;
            }
            return new RpcError(n, Coio.Str(message));
        }

        /// <summary>
        /// Python <c>int(x, 16)</c>: TypeError unless a str; then int()'s whitespace around an optional sign, an
        /// optional <c>0x</c>/<c>0X</c> prefix (its 0 may be any script's zero, as CPython reads decimal digits of
        /// every script) and an optional underscore after it, then hex digits (a-f, A-F and any Unicode decimal
        /// digit) with single underscores between them. Anything else is a ValueError. Base 16 has no digit limit.
        /// </summary>
        public static BigInteger HexInt(object? x)
        {
            if (!(x is string s)) throw new TypeError("int() can't convert non-string with explicit base");
            int i = 0, end = s.Length;
            while (i < end && Py.IsIntSpace(s[i])) i++;
            while (end > i && Py.IsIntSpace(s[end - 1])) end--;
            bool negative = false;
            if (i < end && (s[i] == '+' || s[i] == '-'))
            {
                negative = s[i] == '-';
                i++;
            }
            if (i < end && Py.DecimalValue(Py.CodePointAt(s, i, out int zw)) == 0 && i + zw < end
                && (s[i + zw] == 'x' || s[i + zw] == 'X'))
            {
                i += zw + 1;
                if (i < end && s[i] == '_') i++;
            }
            var digits = new StringBuilder("0", end - i + 1); // a leading 0 keeps the hex parse non-negative
            bool needDigit = true;
            while (i < end)
            {
                int cp = Py.CodePointAt(s, i, out int w);
                i += w;
                if (cp == '_')
                {
                    if (needDigit) throw InvalidHex(s);
                    needDigit = true;
                    continue;
                }
                int d = HexValue(cp);
                if (d < 0) throw InvalidHex(s);
                digits.Append("0123456789abcdef"[d]);
                needDigit = false;
            }
            if (needDigit) throw InvalidHex(s);
            var text = digits.ToString(); // culture-ok: StringBuilder of ASCII hex digits
            var value = BigInteger.Parse(text, NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture);
            return negative ? -value : value;
        }

        private static int HexValue(int cp)
        {
            if (cp >= 'a' && cp <= 'f') return cp - 'a' + 10;
            if (cp >= 'A' && cp <= 'F') return cp - 'A' + 10;
            return Py.DecimalValue(cp);
        }

        private static ValueError InvalidHex(string s) =>
            new ValueError("invalid literal for int() with base 16: " + Py.Repr(s));

        /// <summary>Python <c>hex(n)</c>: <c>0x</c> and lowercase hex digits, <c>-0x5</c> for a negative.</summary>
        public static string Hex(long n)
        {
            if (n >= 0) return "0x" + n.ToString("x", CultureInfo.InvariantCulture);
            ulong magnitude = (ulong)(-(n + 1)) + 1;
            return "-0x" + magnitude.ToString("x", CultureInfo.InvariantCulture);
        }

        /// <summary>A Python int the port holds as a long (a block number, a slot); beyond a long is an OverflowError.</summary>
        internal static long ToLong(BigInteger n, string what)
        {
            if (n < long.MinValue || n > long.MaxValue) throw new OverflowError(what + " out of range: " + n.ToString(CultureInfo.InvariantCulture));
            return (long)n;
        }

        /// <summary>A monotonic clock in whole seconds (Python's <c>time.monotonic</c> default); tests and Tier 2 inject their own.</summary>
        internal static long MonotonicSeconds() => Stopwatch.GetTimestamp() / Stopwatch.Frequency;
    }

    /// <summary>
    /// Python <c>rpc.RpcClient</c>: endpoints in order, cooldowns on <see cref="Clock"/>, per-endpoint eth_getLogs
    /// ranges. Every network method is async and awaits <see cref="Transport"/>; nothing blocks or schedules.
    /// </summary>
    public sealed class RpcClient
    {
        private sealed class EndpointState
        {
            public double Delay;
            public double Until;
        }

        public IReadOnlyList<string> Urls { get; }

        /// <summary>The clock cooldowns run on, in seconds (Python's settable <c>rpc.clock</c>).</summary>
        public Func<long> Clock { get; set; }

        /// <summary>Seconds each request may take, passed to the transport.</summary>
        public double Timeout { get; set; }

        public ITransport Transport { get; }

        private readonly Dictionary<string, long> _ranges = new Dictionary<string, long>(StringComparer.Ordinal);
        private readonly Dictionary<string, EndpointState> _state = new Dictionary<string, EndpointState>(StringComparer.Ordinal);
        private long _nextId = 1;

        public RpcClient(IReadOnlyList<string> urls, ITransport transport, long logRange = 1000, Func<long>? clock = null,
            double timeout = 10.0)
        {
            if (urls == null || urls.Count == 0) throw new ValueError("at least one RPC url required");
            var copy = new List<string>(urls);
            Urls = copy;
            foreach (var u in copy)
            {
                _ranges[u] = logRange;
                _state[u] = new EndpointState();
            }
            Transport = transport;
            Clock = clock ?? Rpc.MonotonicSeconds;
            Timeout = timeout;
        }

        private void Fail(string url, long now)
        {
            var st = _state[url];
            st.Delay = st.Delay == 0 ? Rpc.CooldownStart : Math.Min(Rpc.CooldownMax, st.Delay * 2);
            st.Until = now + st.Delay;
        }

        private void Ok(string url)
        {
            var st = _state[url];
            st.Delay = 0.0;
            st.Until = 0.0;
        }

        /// <summary>The endpoint the next call will try first.</summary>
        private string Preferred()
        {
            long now = Clock();
            foreach (var url in Urls)
                if (_state[url].Until <= now) return url;
            return Urls[0];
        }

        /// <summary>Python <c>log_range</c>: the eth_getLogs block range accepted by the currently preferred endpoint.</summary>
        public long LogRange => _ranges[Preferred()];

        /// <summary>The eth_getLogs block range remembered for one endpoint.</summary>
        public long RangeOf(string url) => _ranges[url];

        /// <summary>An endpoint's current cooldown in seconds (0 when it last answered).</summary>
        public double CooldownOf(string url) => _state[url].Delay;

        /// <summary>The clock time an endpoint's cooldown ends (0 when it last answered).</summary>
        public double CooldownUntil(string url) => _state[url].Until;

        /// <summary>Endpoints not cooling down right now, in preference order.</summary>
        public List<string> LiveUrls()
        {
            long now = Clock();
            var live = new List<string>();
            foreach (var u in Urls)
                if (_state[u].Until <= now) live.Add(u);
            return live;
        }

        public async Task<object?> Call(string method, List<object?> prms) => (await CallWithUrl(method, prms)).Value;

        /// <summary>Python <c>_attempt</c>: an RpcAttempt, or null when the endpoint failed (and now cools down).</summary>
        private async Task<RpcAttempt?> Attempt(string url, string method, List<object?> prms, long now)
        {
            var payload = new JsonObject { { "jsonrpc", "2.0" }, { "id", _nextId++ }, { "method", method }, { "params", prms } };
            object? resp;
            try
            {
                resp = await Transport.PostJson(url, payload, Timeout);
            }
            catch (TransportError)
            {
                Fail(url, now);
                return null;
            }
            if (!(resp is JsonObject body) || (!body.ContainsKey("result") && !body.ContainsKey("error")))
            {
                Fail(url, now);
                return null;
            }
            var error = body.Get("error");
            if (error != null)
            {
                var err = Rpc.ParseError(error);
                if (Rpc.IsRateError(err))
                {
                    Fail(url, now);
                    return null;
                }
                Ok(url);
                err.Url = url;
                return RpcAttempt.Failed(err);
            }
            Ok(url);
            // Python's resp["result"]: {"error": null} has no result, a KeyError (not an endpoint failure)
            return RpcAttempt.Ok(Py.Get(body, "result"));
        }

        /// <summary>Python <c>_call</c>: like Call, but also returns which endpoint answered.</summary>
        private async Task<(object? Value, string Url)> CallWithUrl(string method, List<object?> prms)
        {
            long now = Clock();
            foreach (var url in Urls)
            {
                if (_state[url].Until > now) continue;
                var res = await Attempt(url, method, prms, now);
                if (res == null) continue;
                if (res.IsError) throw res.Error!;
                return (res.ResultValue, url);
            }
            throw new RpcUnavailable("all endpoints failed for " + method);
        }

        /// <summary>Ask one endpoint only. RpcUnavailable if it fails, RpcError for a node error.</summary>
        public async Task<object?> CallAt(string url, string method, List<object?> prms)
        {
            var res = await Attempt(url, method, prms, Clock());
            if (res == null) throw new RpcUnavailable(url + " failed for " + method);
            if (res.IsError) throw res.Error!;
            return res.ResultValue;
        }

        /// <summary>Ask one endpoint: an RpcAttempt (a result or the node's error), or null if it failed.</summary>
        public Task<RpcAttempt?> TryAt(string url, string method, List<object?> prms) => Attempt(url, method, prms, Clock());

        /// <summary>Python <c>block_number</c>: <c>int(result, 16)</c> (held as a long).</summary>
        public async Task<long> BlockNumber() =>
            Rpc.ToLong(Rpc.HexInt(await Call("eth_blockNumber", new List<object?>())), "block number");

        /// <summary>Python <c>get_transaction_receipt</c>: the hash goes out as given (a stored hint is sent unchecked).</summary>
        public Task<object?> GetTransactionReceipt(object? txHash) =>
            Call("eth_getTransactionReceipt", new List<object?> { txHash });

        /// <summary>
        /// Python <c>get_block_hash</c>: the block's <c>hash</c> lowercased, or null for a falsy block. A block that is
        /// not a dict is a TypeError and one without a hash a KeyError, as in Python; a hash that is not a str is a
        /// TypeError (Python's AttributeError, which nothing catches either). Lowercased with ToLowerInvariant, as the
        /// core's other hex: the two differ only for a non-hex hash.
        /// </summary>
        public async Task<string?> GetBlockHash(long number)
        {
            var block = await Call("eth_getBlockByNumber", new List<object?> { Rpc.Hex(number), false });
            if (!Py.Truthy(block)) return null;
            var hash = Py.Get(block, "hash");
            if (!(hash is string h)) throw new TypeError("'" + Py.TypeName(hash) + "' object has no attribute 'lower'");
            return h.ToLowerInvariant();
        }

        /// <summary>
        /// Python <c>get_logs</c>: eth_getLogs over [fromBlock, toBlock] in chunks of <see cref="LogRange"/>. A range
        /// error from the endpoint that answered shrinks its range (<see cref="Rpc.SuggestedRange"/>) and retries
        /// the chunk, down to <see cref="Rpc.MinLogRange"/>. Each answer extends the result as Python's
        /// <c>out.extend(logs or [])</c> does.
        /// </summary>
        public async Task<List<object?>> GetLogs(string address, List<object?> topics, long fromBlock, long toBlock)
        {
            var output = new List<object?>();
            long start = fromBlock;
            while (start <= toBlock)
            {
                long end = Math.Min(toBlock, start + LogRange - 1);
                object? logs;
                try
                {
                    var filter = new JsonObject
                    {
                        { "address", address }, { "topics", topics }, { "fromBlock", Rpc.Hex(start) }, { "toBlock", Rpc.Hex(end) },
                    };
                    logs = (await CallWithUrl("eth_getLogs", new List<object?> { filter })).Value;
                }
                catch (RpcError e)
                {
                    // Python getattr(e, "url", self._preferred()): the fallback only when url is unset
                    var url = e.Url ?? Preferred();
                    long tried = Math.Min(_ranges[url], end - start + 1);
                    if (Rpc.IsRangeError(e) && tried > Rpc.MinLogRange)
                    {
                        _ranges[url] = Rpc.SuggestedRange(e, tried);
                        continue;
                    }
                    throw;
                }
                if (Py.Truthy(logs)) output.AddRange(Coio.Iterate(logs));
                start = end + 1;
            }
            return output;
        }
    }
}
