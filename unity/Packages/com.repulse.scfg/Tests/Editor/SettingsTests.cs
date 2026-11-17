// The SCFG Settings asset (Runtime/Unity/ScfgSettings.cs) and its editor side (Editor/ScfgSettingsEditor.cs): the core's
// validation without touching SCFG, applying it through Configure and AddItem, a later Configure from code winning,
// finding the project's asset and the build step that preloads it.
#nullable enable
using System.Collections.Generic;
using System.Linq;
using NUnit.Framework;
using Scfg.Editor;
using UnityEditor;
using UnityEngine;

namespace Scfg.Tests
{
    public sealed class SettingsTests
    {
        private ScfgRig _rig = null!;
        private readonly List<Object> _made = new List<Object>();

        [SetUp]
        public void Fresh() => _rig = new ScfgRig();

        [TearDown]
        public void Clean()
        {
            foreach (var o in _made) Object.DestroyImmediate(o);
            _made.Clear();
            _rig.Dispose();
        }

        private ScfgSettings Make(System.Action<ScfgSettings>? edit = null)
        {
            var s = ScriptableObject.CreateInstance<ScfgSettings>();
            s.name = "Test Settings";
            _made.Add(s);
            s.wallet = ScfgRig.Dev;
            s.testnet = false;
            s.chains = new List<string> { "arbitrum" };
            s.items = new List<ScfgSettings.Item>
            {
                new ScfgSettings.Item { id = "chapter2", code = 1, price = "4.99", token = "USDC", title = "Chapter 2" },
            };
            edit?.Invoke(s);
            return s;
        }

        [Test]
        public void AValidAssetPassesValidate()
        {
            var s = Make();
            Assert.That(s.Validate(), Is.Null);
            var (text, type) = ScfgSettingsEditor.Verdict(s);
            Assert.That(type, Is.EqualTo(MessageType.Info));
            Assert.That(text, Does.Contain("1 item"));
            Assert.That(SCFG.Current.Config, Is.Null, "Validate does not configure SCFG");
            Assert.That(_rig.Reported, Is.Empty, "nor report anything");
        }

        [TestCase("wallet", "", "wallet.missing")]
        [TestCase("wallet", "0x123", "address.invalid")]
        [TestCase("accent", "red", "accent.invalid")]
        [TestCase("chains", "nowhere", "chains.unknown")]
        public void ValidateShowsTheCoresCodeAndMessage(string field, string value, string code)
        {
            var s = Make(a =>
            {
                if (field == "wallet") a.wallet = value;
                if (field == "accent") a.accent = value;
                if (field == "chains") a.chains = new List<string> { value };
            });
            var problem = s.Validate();
            Assert.That(problem, Is.Not.Null);
            Assert.That(problem!.Code, Is.EqualTo(code), problem.Message);
            Assert.That(problem.Message, Is.Not.Empty);
            var (text, type) = ScfgSettingsEditor.Verdict(s);
            Assert.That(type, Is.EqualTo(MessageType.Error));
            Assert.That(text, Does.StartWith(code + ": "));
        }

        [Test]
        public void ValidateChecksEveryItem()
        {
            var s = Make(a => a.items.Add(new ScfgSettings.Item { id = "chapter3", code = 1, price = "2", token = "USDC", title = "C3" }));
            Assert.That(s.Validate()!.Code, Is.EqualTo("item.duplicate_code"));
            s.items[1].code = 3;
            s.items[1].price = "2.5.0";
            Assert.That(s.Validate()!.Code, Is.EqualTo("item.invalid_price"));
            Assert.That(s.Validate()!.Message, Does.StartWith("item \"chapter3\": "));
            s.items[1].price = "2.50";
            s.items.Add(null!);
            Assert.That(s.Validate(), Is.Null, "an empty row is skipped");
        }

        [Test]
        public void BlankFieldsKeepTheDefaults()
        {
            var s = Make(a =>
            {
                a.solanaWallet = "  ";
                a.chains = new List<string>();
                a.accent = "";
                a.webStorageId = "";
            });
            var o = s.ToOptions();
            Assert.That(o.SolanaWallet, Is.Null);
            Assert.That(o.Chains, Is.Null);
            Assert.That(o.Tokens, Is.Null);
            Assert.That(o.Accent, Is.Null);
            Assert.That(o.WebStorageId, Is.Null);
            Assert.That(o.Logo, Is.Null);
            Assert.That(o.Wallet, Is.EqualTo(ScfgRig.Dev));
        }

        [Test]
        public void ApplyConfiguresAndAddsTheItems()
        {
            Assert.That(Make(a => a.accent = "#7a5cff").Apply(), Is.Null);
            Assert.That(SCFG.Current.Config, Is.Not.Null);
            Assert.That(SCFG.Accent, Is.EqualTo("#7a5cff"));
            Assert.That(SCFG.ItemTitle("chapter2"), Is.EqualTo("Chapter 2"));
            Assert.That(SCFG.Owns("chapter2"), Is.False);
            Assert.That(_rig.Reported, Is.Empty);
        }

        [Test]
        public void AConfigureFromCodeAfterTheAssetWins()
        {
            Make().Apply();
            Assert.That(SCFG.Configure(new ScfgOptions { Wallet = ScfgRig.Dev, Chains = new List<string> { "base" } }), Is.Null);
            Assert.That(SCFG.Current.Config!.Items, Is.Empty, "the asset's items went with its configuration");
            Assert.That(SCFG.AddItem("chapter2", new ScfgItem { Code = 1, Price = "1", Token = "USDC", Title = "From code" }), Is.Null);
            Assert.That(SCFG.ItemTitle("chapter2"), Is.EqualTo("From code"));
        }

        [Test]
        public void ABadAssetIsReportedNotThrown()
        {
            Assert.That(Make(a => a.wallet = "0x123").Apply(), Is.EqualTo("address.invalid"));
            Assert.That(_rig.Reported.Count, Is.EqualTo(2), string.Join("; ", _rig.Reported));
            Assert.That(_rig.Reported.All(r => r.StartsWith("scfg: ")), Is.True);
            Assert.That(_rig.Reported[1], Does.Contain("\"Test Settings\" was not applied (address.invalid)"));
            Assert.That(Make(a => a.items[0].price = "x").Apply(), Is.EqualTo("item.invalid_price"));
            Assert.That(_rig.Reported.Last(), Does.Contain("item \"chapter2\" was not added"));
        }

        [Test]
        public void ApplyAtStartupUsesTheFoundAsset()
        {
            var s = Make();
            ScfgSettings.FindOverride = () => s;
            try
            {
                ScfgSettings.ApplyAtStartup();
            }
            finally
            {
                ScfgSettings.FindOverride = null;
            }
            Assert.That(SCFG.ItemTitle("chapter2"), Is.EqualTo("Chapter 2"));
            _rig.Restart();
            ScfgSettings.FindOverride = () => null;
            try
            {
                ScfgSettings.ApplyAtStartup();
            }
            finally
            {
                ScfgSettings.FindOverride = null;
            }
            Assert.That(SCFG.Current.Config, Is.Null, "no asset: nothing is configured");
        }

        [Test]
        public void AWholeTextureSpriteIsTheLogoAsIs()
        {
            var texture = new Texture2D(4, 4, TextureFormat.RGBA32, false);
            _made.Add(texture);
            var whole = Sprite.Create(texture, new Rect(0, 0, 4, 4), Vector2.zero);
            _made.Add(whole);
            Assert.That(Make(a => a.logo = whole).ToOptions().Logo, Is.SameAs(texture));
            var part = Sprite.Create(texture, new Rect(1, 1, 2, 3), Vector2.zero);
            _made.Add(part);
            var cropped = Make(a => a.logo = part).ToOptions().Logo!;
            _made.Add(cropped);
            Assert.That((cropped.width, cropped.height), Is.EqualTo((2, 3)), "a readable atlas sprite is cropped");
        }

        [Test]
        public void TheProjectsAssetIsFoundAndPreloadedForABuildOnly()
        {
            const string path = "Assets/ScfgSettingsTest.asset";
            var before = PlayerSettings.GetPreloadedAssets();
            Assert.That(ScfgSettings.Find(), Is.Null, "the dev project has none");
            var asset = ScriptableObject.CreateInstance<ScfgSettings>();
            AssetDatabase.CreateAsset(asset, path);
            try
            {
                Assert.That(ScfgSettings.Find(), Is.SameAs(asset));
                var build = new ScfgSettingsBuild();
                build.OnPreprocessBuild(null!);
                Assert.That(PlayerSettings.GetPreloadedAssets(), Has.Member(asset));
                build.OnPreprocessBuild(null!);
                Assert.That(PlayerSettings.GetPreloadedAssets().Count(o => o == asset), Is.EqualTo(1), "once");
                build.OnPostprocessBuild(null!);
                Assert.That(PlayerSettings.GetPreloadedAssets(), Has.No.Member(asset));
            }
            finally
            {
                ScfgSettingsBuild.Remove();
                AssetDatabase.DeleteAsset(path);
                PlayerSettings.SetPreloadedAssets(before);
            }
            Assert.That(ScfgSettings.Find(), Is.Null);
        }
    }
}
