// No Python twin: the driver's real transport (Python's rpc.UrllibTransport reads an HTTP answer the same way).
using System;
using System.Globalization;
using System.Net.Http;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Scfg.Core;

namespace Scfg.Core.Driver
{
    /// <summary>An <see cref="ITransport"/> on <see cref="HttpClient"/>: POST the JSON, read the body bytes, hand status and body to ParseRpcBody.</summary>
    public sealed class HttpClientTransport : ITransport
    {
        private static readonly HttpClient Http = new HttpClient { Timeout = Timeout.InfiniteTimeSpan };

        public int Posts { get; private set; }

        public async Task<object?> PostJson(string url, JsonObject payload, double timeout)
        {
            Posts++;
            using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(timeout));
            try
            {
                using var content = new StringContent(Json.Dumps(payload), new UTF8Encoding(false), "application/json");
                using var response = await Http.PostAsync(url, content, cts.Token);
                var body = await response.Content.ReadAsByteArrayAsync(cts.Token);
                return Coio.ParseRpcBody((int)response.StatusCode, body);
            }
            catch (OperationCanceledException)
            {
                throw new TransportError("timed out after " + timeout.ToString(CultureInfo.InvariantCulture) + "s");
            }
            catch (HttpRequestException e)
            {
                throw new TransportError(e.Message);
            }
            catch (InvalidOperationException e) // an unusable URL (relative, unsupported scheme)
            {
                throw new TransportError(e.Message);
            }
            catch (UriFormatException e)
            {
                throw new TransportError(e.Message);
            }
        }
    }
}
