// No Python twin: the checkout controllers' server factory on desktop (Godot's _server_factory / _server_opts): a
// Scfg.Desktop.CheckoutServer serving the shared checkout page (ScfgWebAssets), the logo as PNG, and the controller's
// session info, status and Solana build. The controller's Poll() pumps it every frame. Not built for WebGL players
// (Scfg.Desktop excludes WebGL; the web checkout is the overlay, SCFG.Web.cs).
#nullable enable
#if !UNITY_WEBGL || UNITY_EDITOR
using System;
using Scfg.Core;
using Scfg.Desktop;

namespace Scfg
{
    internal static class ScfgDesktop
    {
        /// <summary>The options of a localhost page for <paramref name="args"/> (Godot's _server_opts).</summary>
        public static CheckoutServerOptions OptionsFor(ServerArgs args)
        {
            var logo = SCFG.LogoPng();
            // The core's session_info says whether the core config has a logo (a path); the glue's logo is a texture.
            var info = (JsonObject)DeepCopy.Of(args.SessionInfo)!;
            info["logo"] = logo != null;
            return new CheckoutServerOptions
            {
                LoadAsset = name => ScfgWebAssets.Load(name)!, // null is a 500, as the server documents
                SessionInfo = info,
                OnTx = args.OnTx,
                GetStatus = args.GetStatus,
                BuildTx = args.BuildTx,
                LoadLogo = logo == null ? (Func<(byte[] Body, string ContentType)>?)null : () => (logo, "image/png"),
            };
        }

        public static ICheckoutServer CreateServer(ServerArgs args) => new CheckoutServer(OptionsFor(args));
    }
}
#endif
