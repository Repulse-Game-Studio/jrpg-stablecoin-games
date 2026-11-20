#nullable enable
using NUnit.Framework;
using Scfg.Core;

namespace Scfg.Tests
{
    /// <summary>The core runs inside the editor: a config, an item and a prepared payment.</summary>
    public sealed class SmokeTests
    {
        [Test]
        public void TheCoreBuildsAConfigAndPreparesAPayment()
        {
            var cfg = Config.BuildConfig(new JsonObject { { "wallet", "0x000000000000000000000000000000000000dEaD" }, { "testnet", true } });
            Config.AddItem(cfg, "chapter2", new JsonObject { { "code", 1L }, { "price", "4.99" }, { "token", "USDC" }, { "title", "Chapter 2" } });
            var spec = Payments.PreparePayment(cfg, "unlock", new JsonObject { { "item", "chapter2" } });
            Assert.AreEqual("unlock", spec.Kind);
            Assert.AreEqual("4.99", spec.Price.ToString());
            Assert.IsNotEmpty(spec.Chains);
        }
    }
}
