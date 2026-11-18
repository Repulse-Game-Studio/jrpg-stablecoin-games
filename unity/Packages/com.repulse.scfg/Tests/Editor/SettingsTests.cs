// The SCFG Settings asset (Runtime/Unity/ScfgSettings.cs) and its editor side (Editor/ScfgSettingsEditor.cs): the core's
// validation without touching SCFG, applying it through Configure and AddItem, a later Configure from code winning (and
// an AddChain setting it aside), the logo (cut out of any texture, never thrown, one copy), finding the project's asset
// (several: none) and the build step that preloads it and cleans up after a build that did not finish.
#nullable enable
using System.Collections.Generic;
using System.Linq;
using NUnit.Framework;
using Scfg.Editor;
using UnityEditor;
using UnityEditor.Build;
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
            ScfgSettings.SearchFoldersOverride = null;
            if (AssetDatabase.IsValidFolder(Scope)) AssetDatabase.DeleteAsset(Scope); // a test that failed midway
            foreach (var o in _made)
                if (o != null) Object.DestroyImmediate(o);
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

        // No wallet is the opt-out (the game configures from code), not a problem; the rest is still checked.
        [Test]
        public void AnAssetWithoutAWalletIsValidAndNotAppliedAtStartup()
        {
            var s = Make(a => a.wallet = "");
            Assert.That(s.HasWallet, Is.False);
            Assert.That(s.Validate(), Is.Null, "the missing wallet is not a problem");
            var (text, type) = ScfgSettingsEditor.Verdict(s);
            Assert.That(type, Is.EqualTo(MessageType.Info));
            Assert.That(text, Does.Contain("not applied at startup"));
            Assert.That(Make(a => { a.wallet = ""; a.accent = "red"; }).Validate()!.Code, Is.EqualTo("accent.invalid"),
                        "the rest is still checked");
            ScfgSettings.FindOverride = () => s;
            try
            {
                ScfgSettings.ApplyAtStartup();
            }
            finally
            {
                ScfgSettings.FindOverride = null;
            }
            Assert.That(SCFG.Current.Config, Is.Null, "not applied");
            Assert.That(_rig.Reported, Is.Empty, "and not reported as an error");
        }

        // A wallet supplied at runtime, with the asset's options and items.
        [Test]
        public void ApplyWithAWalletUsesThatWalletAndTheAssetsItems()
        {
            const string Runtime = "0xfB6916095ca1df60bB79Ce92cE3Ea74c37c5d359";
            var s = Make(a => a.wallet = "");
            Assert.That(s.Apply(Runtime), Is.Null);
            Assert.That(SCFG.Current.Config!.Wallet, Is.EqualTo(Runtime));
            Assert.That(SCFG.ItemTitle("chapter2"), Is.EqualTo("Chapter 2"));
            _rig.Restart();
            Assert.That(s.Apply(" "), Is.EqualTo("wallet.missing"), "a blank wallet is no wallet");
        }

        [Test]
        public void ApplyAtStartupNeverThrows()
        {
            ScfgSettings.FindOverride = () => throw new System.InvalidOperationException("boom");
            try
            {
                Assert.DoesNotThrow(ScfgSettings.ApplyAtStartup);
            }
            finally
            {
                ScfgSettings.FindOverride = null;
            }
            Assert.That(_rig.Reported.Last(), Does.Contain("could not be applied: InvalidOperationException: boom"));
        }

        // --- the logo ----------------------------------------------------------------------------------------

        /// <summary>A w x h texture whose every pixel is a different color (r = x, g = y).</summary>
        private Texture2D Gradient(int w, int h, bool readable = true)
        {
            var texture = new Texture2D(w, h, TextureFormat.RGBA32, false);
            var pixels = new Color32[w * h];
            for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                pixels[y * w + x] = new Color32((byte)(x * 10), (byte)(y * 10), 7, 255);
            texture.SetPixels32(pixels);
            texture.Apply(false, !readable);
            _made.Add(texture);
            return texture;
        }

        private Sprite SpriteOf(Texture2D texture, Rect rect, SpriteMeshType mesh = SpriteMeshType.FullRect)
        {
            var sprite = Sprite.Create(texture, rect, new Vector2(0.3f, 0.6f), 100, 0, mesh);
            _made.Add(sprite);
            return sprite;
        }

        [Test]
        public void AWholeTextureSpriteIsTheLogoAsIs()
        {
            var texture = Gradient(4, 4);
            var s = Make(a => a.logo = SpriteOf(texture, new Rect(0, 0, 4, 4)));
            Assert.That(s.ToOptions().Logo, Is.SameAs(texture));
            Assert.That(s.LogoCopy, Is.Null, "no copy");
        }

        [Test]
        public void ASheetCellIsCutOutIntoOneCopyTheAssetOwns()
        {
            var texture = Gradient(8, 8);
            var cell = SpriteOf(texture, new Rect(1, 2, 3, 4));
            var s = Make(a => a.logo = cell);
            var logo = s.ToOptions().Logo!;
            Assert.That((logo.width, logo.height), Is.EqualTo((3, 4)));
            Assert.That(logo.GetPixel(0, 0), Is.EqualTo(texture.GetPixel(1, 2)));
            Assert.That(logo.GetPixel(2, 3), Is.EqualTo(texture.GetPixel(3, 5)));
            Assert.That(s.ToOptions().Logo, Is.SameAs(logo), "the copy is reused");
            Assert.That(s.Apply(), Is.Null);
            Assert.That(SCFG.LogoTexture, Is.SameAs(logo), "Apply reuses it too");

            // another sprite: a new copy; the old one is destroyed, unless SCFG still shows it
            _made.Add(logo);
            s.logo = SpriteOf(texture, new Rect(0, 0, 2, 2));
            var second = s.ToOptions().Logo!;
            Assert.That(second, Is.Not.SameAs(logo));
            Assert.That(logo != null, Is.True, "SCFG still shows the first copy");
            s.logo = cell;
            var third = s.ToOptions().Logo!;
            Assert.That(second == null, Is.True, "the copy SCFG did not show was destroyed");
            Assert.That(third, Is.SameAs(s.LogoCopy));
            Assert.That(_rig.Warnings.Where(w => w.Contains("logo")), Is.Empty);
        }

        [Test]
        public void ValidateMakesNoLogo()
        {
            var s = Make(a => a.logo = SpriteOf(Gradient(8, 8, readable: false), new Rect(1, 1, 2, 3)));
            Assert.That(s.Validate(), Is.Null);
            Assert.That(s.LogoCopy, Is.Null);
            Assert.That(_rig.Warnings, Is.Empty);
            Assert.That(_rig.Reported, Is.Empty);
        }

        [Test]
        public void ACellOfANonReadableTextureNeverThrows()
        {
            // The tests run without a graphics device, so the GPU cannot read it either: the whole texture, with a warning.
            // (With a device, Read cuts it out through a RenderTexture.)
            var texture = Gradient(8, 8, readable: false);
            Assert.That(texture.isReadable, Is.False);
            var s = Make(a => a.logo = SpriteOf(texture, new Rect(1, 1, 2, 3)));
            ScfgOptions? o = null;
            Assert.DoesNotThrow(() => o = s.ToOptions());
            Assert.That(o!.Logo, Is.SameAs(texture));
            Assert.That(_rig.Warnings.Single(), Does.Contain("could not be cut out of its texture"));
            Assert.That(s.Apply(), Is.Null, "the asset is still applied");
            Assert.That(SCFG.ItemTitle("chapter2"), Is.EqualTo("Chapter 2"));
        }

        [Test]
        public void ADestroyedLogoTextureMeansNoLogo()
        {
            var texture = Gradient(4, 4);
            var s = Make(a => a.logo = SpriteOf(texture, new Rect(0, 0, 2, 2)));
            Object.DestroyImmediate(texture);
            ScfgOptions? o = null;
            Assert.DoesNotThrow(() => o = s.ToOptions());
            Assert.That(o!.Logo, Is.Null);
            Assert.That(s.Apply(), Is.Null);
        }

        [TestCase(SpriteMeshType.FullRect)]
        [TestCase(SpriteMeshType.Tight)]
        public void APackedSpriteIsDrawnFromItsMesh(SpriteMeshType mesh)
        {
            // Render is the path of a sprite packed tight or rotated in a Sprite Atlas (a player); here its mesh and UVs
            // point into an unpacked texture, so it must give what the rect crop gives.
            var texture = Gradient(16, 12);
            var pixels = texture.GetPixels32();
            for (int y = 0; y < 12; y++) pixels[y * 16 + 2] = new Color32(0, 0, 0, 0); // a transparent column for the tight mesh
            texture.SetPixels32(pixels);
            texture.Apply();
            var sprite = SpriteOf(texture, new Rect(2, 3, 9, 7), mesh);
            var drawn = ScfgSettings.Render(sprite, texture)!;
            _made.Add(drawn);
            var cropped = ScfgSettings.Crop(texture, new RectInt(2, 3, 9, 7))!;
            _made.Add(cropped);
            Assert.That((drawn.width, drawn.height), Is.EqualTo((9, 7)));
            var a = drawn.GetPixels32();
            var b = cropped.GetPixels32();
            int opaque = 0, covered = 0;
            for (int i = 0; i < a.Length; i++)
            {
                if (b[i].a != 0) opaque++;
                if (a[i].a == 0) continue; // a tight mesh leaves out the transparent column
                covered++;
                Assert.That(a[i], Is.EqualTo(b[i]), "pixel " + (i % 9) + "," + (i / 9));
            }
            if (mesh == SpriteMeshType.FullRect) Assert.That(covered, Is.EqualTo(opaque));
            else Assert.That(covered, Is.GreaterThanOrEqualTo(opaque * 8 / 10), "the tight mesh covers the opaque part");
        }

        // The asset tests look only in a folder of their own (ScfgSettings.SearchFoldersOverride), so they pass in a game
        // project that has its own SCFG Settings asset too. Its folder, made and removed by each test.
        private const string Scope = "Assets/ScfgSettingsTestScope";

        private static void EnterScope()
        {
            if (!AssetDatabase.IsValidFolder(Scope)) AssetDatabase.CreateFolder("Assets", "ScfgSettingsTestScope");
            ScfgSettings.SearchFoldersOverride = new[] { Scope };
        }

        private static void LeaveScope()
        {
            ScfgSettings.SearchFoldersOverride = null;
            AssetDatabase.DeleteAsset(Scope);
        }

        [Test]
        public void TheProjectsAssetIsFoundAndPreloadedForABuildOnly()
        {
            EnterScope();
            const string path = Scope + "/ScfgSettingsTest.asset";
            var before = PlayerSettings.GetPreloadedAssets();
            Assert.That(ScfgSettings.Find(), Is.Null, "none in the test's folder");
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
                ScfgSettingsBuild.CleanUp();
                AssetDatabase.DeleteAsset(path);
                PlayerSettings.SetPreloadedAssets(before);
            }
            Assert.That(ScfgSettings.Find(), Is.Null);
            LeaveScope();
        }

        [Test]
        public void ABuildThatDidNotFinishIsCleanedUpWithoutMemory()
        {
            // A failed build has no postprocess, and a script reload forgets everything: the clean-up (run after the
            // build and at every editor load) takes out every SCFG Settings entry and keeps the others.
            EnterScope();
            const string path = Scope + "/ScfgSettingsTest.asset";
            var before = PlayerSettings.GetPreloadedAssets();
            var asset = ScriptableObject.CreateInstance<ScfgSettings>();
            AssetDatabase.CreateAsset(asset, path);
            var other = ScriptableObject.CreateInstance<ScfgSettings>();
            _made.Add(other);
            var theirs = new Texture2D(1, 1);
            _made.Add(theirs);
            try
            {
                new ScfgSettingsBuild().OnPreprocessBuild(null!);
                // a stale entry of an older build (another asset since) and the developer's own preloaded asset
                PlayerSettings.SetPreloadedAssets(PlayerSettings.GetPreloadedAssets().Concat(new Object[] { other, theirs }).ToArray());
                new ScfgSettingsBuild().OnPreprocessBuild(null!);
                var preloaded = PlayerSettings.GetPreloadedAssets();
                Assert.That(preloaded.Count(o => o is ScfgSettings), Is.EqualTo(1), "a build preloads exactly the asset Find uses");
                Assert.That(preloaded, Has.Member(asset).And.Member(theirs));
                PlayerSettings.SetPreloadedAssets(preloaded.Append(other).ToArray());
                ScfgSettingsBuild.CleanUp();
                Assert.That(PlayerSettings.GetPreloadedAssets().Any(o => o is ScfgSettings), Is.False);
                Assert.That(PlayerSettings.GetPreloadedAssets(), Has.Member(theirs));
            }
            finally
            {
                ScfgSettingsBuild.CleanUp();
                AssetDatabase.DeleteAsset(path);
                PlayerSettings.SetPreloadedAssets(before);
                LeaveScope();
            }
        }

        [Test]
        public void SeveralAssetsAreRefusedInPlayModeAndInABuild()
        {
            EnterScope();
            const string a = Scope + "/ScfgSettingsTestA.asset", b = Scope + "/ScfgSettingsTestB.asset";
            var before = PlayerSettings.GetPreloadedAssets();
            AssetDatabase.CreateAsset(ScriptableObject.CreateInstance<ScfgSettings>(), b);
            AssetDatabase.CreateAsset(ScriptableObject.CreateInstance<ScfgSettings>(), a);
            try
            {
                Assert.That(ScfgSettings.FindAll().Select(ScfgSettings.Describe), Is.EqualTo(new[] { a, b }));
                Assert.That(ScfgSettings.Find(), Is.Null, "none is applied");
                Assert.That(_rig.Reported.Last(), Does.Contain("2 SCFG Settings assets were found (" + a + ", " + b + "), so none is applied"));
                var e = Assert.Throws<BuildFailedException>(() => new ScfgSettingsBuild().OnPreprocessBuild(null!));
                Assert.That(e!.Message, Does.Contain(a).And.Contain(b));
                Assert.That(PlayerSettings.GetPreloadedAssets(), Is.EqualTo(before), "the list is left alone");
            }
            finally
            {
                AssetDatabase.DeleteAsset(a);
                AssetDatabase.DeleteAsset(b);
                PlayerSettings.SetPreloadedAssets(before);
                LeaveScope();
            }
        }

        [Test]
        public void TheAssetsOnrampFieldsAreValidatedAndApplied()
        {
            Assert.That(Make(s => s.onrampUrl = "http://buy.example").Validate()!.Code, Is.EqualTo("onramp.bad_url"));
            Assert.That(Make(s => s.onrampChains = new List<string> { "base" }).Validate()!.Code, Is.EqualTo("onramp.disabled_chain"));
            Assert.That(Make().ToOptions().Onramp, Is.Null, "the defaults are left to the core");
            Assert.That(Make(s => s.onrampEnabled = false).Apply(), Is.Null);
            Assert.That(SCFG.Current.Config!.Onramp.Enabled, Is.False);
        }

        // --- AddChain with the asset -------------------------------------------------------------------------

        private static ScfgChain MyL2() => new ScfgChain
        {
            Name = "My L2", ChainId = 4242, Rpc = new List<string> { "https://rpc.example.org" },
            Tokens = new Dictionary<string, string> { { "USDC", "0x0000000000000000000000000000000000000001" } },
            Confirmations = 3, ExplorerTx = "https://explorer.example.org/tx/{}",
        };

        [Test]
        public void AddChainSetsTheAssetsConfigurationAsideForTheCodesConfigure()
        {
            Assert.That(Make().Apply(), Is.Null);
            Assert.That(SCFG.AddChain("myl2", MyL2()), Is.Null);
            Assert.That(SCFG.Current.Config, Is.Null, "the asset's configuration is set aside");
            Assert.That(_rig.Warnings.Last(), Does.Contain("call SCFG.Configure() next"));
            Assert.That(SCFG.Configure(new ScfgOptions { Wallet = ScfgRig.Dev, Chains = new List<string> { "myl2" } }), Is.Null);
            Assert.That(SCFG.ChainName("myl2"), Is.EqualTo("My L2"));
            Assert.That(SCFG.ItemTitle("chapter2"), Is.EqualTo("chapter2"), "the asset's items went with it");
            Assert.That(SCFG.AddChain("other", MyL2()), Is.EqualTo("configure.after_add_chain"), "after a Configure from code: refused");
            Assert.That(_rig.Reported.Last(), Does.EndWith("call SCFG.AddChain() before SCFG.Configure()"));
        }

        [Test]
        public void AddChainWithABadChainLeavesTheAssetsConfigurationInPlace()
        {
            Assert.That(Make().Apply(), Is.Null);
            var warned = _rig.Warnings.Count;
            Assert.That(SCFG.AddChain("Bad Key", MyL2()), Is.EqualTo("chain.invalid_key"));
            Assert.That(SCFG.Current.Config, Is.Not.Null, "the asset's configuration stays");
            Assert.That(SCFG.Current.ConfigFromSettings, Is.True);
            Assert.That(_rig.Warnings.Count, Is.EqualTo(warned), "nothing was set aside");
            Assert.That(SCFG.ItemTitle("chapter2"), Is.EqualTo("Chapter 2"));
            Assert.That(SCFG.AddChain("myl2", MyL2()), Is.Null, "a good chain then sets it aside");
            Assert.That(SCFG.Current.Config, Is.Null);
        }

        [Test]
        public void AddChainAfterTheAssetsPaymentsStartedIsRefused()
        {
            Assert.That(Make().Apply(), Is.Null);
            _rig.Pump();
            Assert.That(SCFG.Current.Worker, Is.Not.Null);
            Assert.That(SCFG.AddChain("myl2", MyL2()), Is.EqualTo("configure.after_add_chain"));
            Assert.That(_rig.Reported.Last(), Does.Contain("the SCFG Settings asset configured SCFG before the first scene"));
            Assert.That(SCFG.ItemTitle("chapter2"), Is.EqualTo("Chapter 2"), "the asset's configuration stays");
        }
    }
}
