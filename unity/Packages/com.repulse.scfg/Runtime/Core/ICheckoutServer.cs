// Twin of scfg_core/checkout.py's server contract: what CheckoutController's server_factory receives and returns
// (local_server.CheckoutServer on desktop, overlay.OverlayCheckout on the web), following rpgmaker/src/core/checkout.ts
// (ServerArgs, CheckoutServer). Implemented by Scfg.Desktop.CheckoutServer (the localhost page) and OverlayCheckout.
#nullable enable
using System;

namespace Scfg.Core
{
    /// <summary>What a server factory receives from the checkout controller (checkout.py's keyword arguments).</summary>
    public sealed class ServerArgs
    {
        /// <summary>What the page shows and needs to build the payment (amount, recipient, chain, token...).</summary>
        public JsonObject SessionInfo { get; }

        /// <summary>
        /// The page reported a sent transaction. The server first runs the page's message through
        /// <see cref="PageMessages.ParseTxMessage"/> and passes its results: the tx hash is then already normalised
        /// and the sender is a normalised address or null. A message ParseTxMessage rejects never reaches OnTx.
        /// Called from the game loop's turn (inside the server's Pump), never from inside a worker command.
        /// </summary>
        public Action<string, string?> OnTx { get; }

        /// <summary>The controller's current StatusInfo() for the page's status poll.</summary>
        public Func<JsonObject> GetStatus { get; }

        /// <summary>Solana only: the unsigned transaction for the player's account, as a Job the server polls.</summary>
        public Func<string, Job>? BuildTx { get; }

        public ServerArgs(JsonObject sessionInfo, Action<string, string?> onTx, Func<JsonObject> getStatus,
                          Func<string, Job>? buildTx = null)
        {
            SessionInfo = sessionInfo ?? throw new TypeError("ServerArgs needs session_info");
            OnTx = onTx ?? throw new TypeError("ServerArgs needs on_tx");
            GetStatus = getStatus ?? throw new TypeError("ServerArgs needs get_status");
            BuildTx = buildTx;
        }
    }

    /// <summary>The browser checkout the controller starts for a session: the localhost page or the web overlay.</summary>
    public interface ICheckoutServer
    {
        /// <summary>Starts serving; throws when it cannot (the controller then offers another method).</summary>
        void Start();

        /// <summary>Stops serving and cancels any build it was waiting on. Safe to call twice.</summary>
        void Stop();

        /// <summary>What the browser opens (the overlay's is a placeholder: there is no page to open).</summary>
        string Url { get; }

        bool Running { get; }

        /// <summary>Show the page again (the overlay); a no-op for the localhost server.</summary>
        void Reopen();

        /// <summary>Serve without waiting: called every frame by the controller's Poll().</summary>
        void Pump();
    }
}
