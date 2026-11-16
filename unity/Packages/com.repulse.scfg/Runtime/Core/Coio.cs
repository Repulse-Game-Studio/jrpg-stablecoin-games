// Twin of scfg_core/coio.py, its transport half (TransportError, Request as ITransport.PostJson,
// response_from_envelope), and of rpc.UrllibTransport's rules for reading an HTTP answer (ParseRpcBody).
//
// The core never performs a network request itself. Python's cooperative generators (yield a Request, receive
// the parsed response or have a TransportError thrown in) become async methods awaiting an injected
// ITransport, as in the TypeScript core (rpgmaker/src/core/coio.ts). Task<T> is only a promise here: the core
// never starts, waits on or schedules a task, so a transport completed by a later Unity frame resumes the
// continuation on the main thread.
//
// Not ported: run_sync, Sync and synced (await replaces them) and UrllibTransport itself (a real transport is
// platform code; it reads the answer's status and bytes and hands them to ParseRpcBody).
#nullable enable
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Numerics;
using System.Text;
using System.Threading.Tasks;

namespace Scfg.Core
{
    /// <summary>Python <c>coio.TransportError</c>: a network or HTTP failure talking to one endpoint.</summary>
    public class TransportError : Exception
    {
        /// <summary>The HTTP status of an error answer; null when no answer arrived (connection, timeout, TLS).</summary>
        public int? Status { get; }

        public TransportError(string message, int? status = null) : base(message)
        {
            Status = status;
        }
    }

    /// <summary>
    /// The transport contract (Python's <c>post_json(url, payload, timeout)</c>, driven by run_sync or the
    /// PumpWorker). POST <paramref name="payload"/> as JSON to <paramref name="url"/>, waiting at most
    /// <paramref name="timeout"/> seconds, and complete with the parsed JSON-RPC response (a JSON value as
    /// <see cref="Json.Loads"/> makes it), or fail with a <see cref="TransportError"/> when the endpoint failed.
    /// <see cref="Coio.ParseRpcBody"/> turns an HTTP answer into exactly that. Any other exception is not an
    /// endpoint failure: the RPC client lets it propagate, as Python's <c>except TransportError</c> does.
    /// The returned task may complete later (a frame-driven transport) or already be complete (a test fake).
    /// </summary>
    public interface ITransport
    {
        Task<object?> PostJson(string url, JsonObject payload, double timeout);
    }

    public static class Coio
    {
        /// <summary>
        /// The most bytes of one HTTP answer a transport reads; a longer one is TransportError("response too
        /// large") (coio.ts's MAX_RESPONSE_BYTES; scfg_core's UrllibTransport reads without a cap). An
        /// <c>eth_getLogs</c> page filtered to one recipient is a few kilobytes and public providers cap their own
        /// answers near 10 MB, so 16 MiB only refuses an endpoint that is misbehaving or hostile.
        /// </summary>
        public const int MaxResponseBytes = 16 * 1024 * 1024;

        public const string ResponseTooLarge = "response too large";

        public const string InvalidJson = "invalid JSON response";

        /// <summary>
        /// The JSON-RPC response for an HTTP answer, by rpc.UrllibTransport's rules, as every real transport
        /// reads one:
        /// <list type="bullet">
        /// <item>more than <see cref="MaxResponseBytes"/> bytes is TransportError("response too large");</item>
        /// <item>a 2xx answer is its body read as Python's <c>ints.json_loads(raw.decode("utf-8"))</c> reads it
        /// (strict UTF-8, a BOM refused, an integer literal of more than 4300 digits refused), or
        /// TransportError("invalid JSON response") when it cannot be read;</item>
        /// <item>any other status is TransportError("HTTP &lt;status&gt;", status), unless the body is a JSON object
        /// with a truthy <c>error</c> (providers send JSON-RPC errors such as "range too large" with a 4xx
        /// status): that body is returned.</item>
        /// </list>
        /// </summary>
        public static object? ParseRpcBody(int status, byte[] body)
        {
            if (body.Length > MaxResponseBytes) throw new TransportError(ResponseTooLarge);
            bool readable = false;
            object? parsed = null;
            if (IsUtf8(body))
            {
                try
                {
                    // valid UTF-8 decodes the same with any decoder; GetString keeps a BOM, which Loads refuses
                    parsed = Ints.JsonLoads(Encoding.UTF8.GetString(body));
                    readable = true;
                }
                catch (ValueError)
                {
                }
            }
            return ResponseFor(status, readable, parsed);
        }

        /// <summary>
        /// Python <c>response_from_envelope</c>: a browser fetch() result (<c>{"status", "body"}</c> or
        /// <c>{"error"}</c>) as the JSON-RPC response, by the same rules as <see cref="ParseRpcBody"/> (no size
        /// cap: the page already read the body). The status is read with <c>int_from</c> and the body must be a
        /// str (json.loads raises TypeError otherwise, which Python does not catch).
        /// </summary>
        public static object? ResponseFromEnvelope(JsonObject envelope)
        {
            if (envelope.TryGetValue("error", out var error)) throw new TransportError(Str(error));
            var rawStatus = envelope.Get("status");
            var status = Ints.IntFrom(Py.Truthy(rawStatus) ? rawStatus : 0L);
            var rawBody = envelope.Get("body");
            object? text = Py.Truthy(rawBody) ? rawBody : "";
            if (!(text is string s))
                throw new TypeError("the JSON object must be str, bytes or bytearray, not " + Py.TypeName(text));
            bool readable = false;
            object? parsed = null;
            try
            {
                parsed = Ints.JsonLoads(s);
                readable = true;
            }
            catch (ValueError)
            {
            }
            return ResponseFor(status, readable, parsed);
        }

        private static object? ResponseFor(BigInteger status, bool readable, object? body)
        {
            if (status < 200 || status >= 300)
            {
                if (readable && body is JsonObject o && Py.Truthy(o.Get("error"))) return body;
                int? code = status >= int.MinValue && status <= int.MaxValue ? (int)status : (int?)null;
                throw new TransportError("HTTP " + status.ToString(CultureInfo.InvariantCulture), code);
            }
            if (!readable) throw new TransportError(InvalidJson);
            return body;
        }

        /// <summary>
        /// Strict UTF-8, as CPython's decoder checks it: no overlong forms, no surrogates, nothing above U+10FFFF,
        /// no truncated sequence (coio.gd's is_utf8). Checked here rather than trusting a runtime's decoder.
        /// </summary>
        public static bool IsUtf8(byte[] b)
        {
            int n = b.Length;
            int i = 0;
            while (i < n)
            {
                int c = b[i];
                if (c < 0x80)
                {
                    i++;
                    continue;
                }
                int need, lo = 0x80, hi = 0xbf;
                if (c >= 0xc2 && c <= 0xdf)
                {
                    need = 1;
                }
                else if (c >= 0xe0 && c <= 0xef)
                {
                    need = 2;
                    if (c == 0xe0) lo = 0xa0;
                    else if (c == 0xed) hi = 0x9f;
                }
                else if (c >= 0xf0 && c <= 0xf4)
                {
                    need = 3;
                    if (c == 0xf0) lo = 0x90;
                    else if (c == 0xf4) hi = 0x8f;
                }
                else
                {
                    return false;
                }
                if (i + need >= n) return false;
                for (int k = 1; k <= need; k++)
                {
                    int cc = b[i + k];
                    if (k == 1 ? cc < lo || cc > hi : cc < 0x80 || cc > 0xbf) return false;
                }
                i += need + 1;
            }
            return true;
        }

        /// <summary>Python's <c>str(v)</c> of a JSON value: a str as it is, anything else as its repr.</summary>
        public static string Str(object? v) => v is string s ? s : Py.Repr(v);

        /// <summary>
        /// Python's iteration of a JSON value (<c>list.extend(v)</c>, <c>for x in v</c>): a list's items, a dict's
        /// keys, a str's code points (each a str); anything else is TypeError("'int' object is not iterable").
        /// </summary>
        internal static IEnumerable<object?> Iterate(object? v)
        {
            switch (v)
            {
                case List<object?> list:
                    return list;
                case JsonObject o:
                    return o.Keys;
                case string s:
                {
                    var chars = new List<object?>(s.Length);
                    for (int i = 0; i < s.Length;)
                    {
                        Py.CodePointAt(s, i, out int w);
                        chars.Add(s.Substring(i, w));
                        i += w;
                    }
                    return chars;
                }
                default:
                    throw new TypeError("'" + Py.TypeName(v) + "' object is not iterable");
            }
        }
    }
}
