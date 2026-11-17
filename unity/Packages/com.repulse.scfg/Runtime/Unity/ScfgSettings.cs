// No Python twin: the optional SCFG Settings asset (Assets > Create > SCFG Settings), configuration without code. Code
// stays the source of truth: the asset is applied before the first scene loads through the same SCFG.Configure and
// SCFG.AddItem calls, and a Configure from code made after it replaces it (with its items).
//
// Where it is found: in a player, the asset the build put in its preloaded assets (Scfg.Editor.ScfgSettingsBuild adds
// the project's one for the build only); in the editor's play mode, the project's one through the asset database. A
// project should hold one; with several, the first by path is used and the others are reported.
//
// Validate() runs the core's validation on the asset without touching SCFG (the inspector's Validate button): the first
// ConfigError's code and message, or null when the configuration and every item are valid.
#nullable enable
using System;
using System.Collections.Generic;
using Scfg.Core;
using UnityEngine;

namespace Scfg
{
    /// <summary>The SCFG Settings asset (see the file header).</summary>
    [CreateAssetMenu(menuName = "SCFG Settings", fileName = "ScfgSettings", order = 1000)]
    [HelpURL("https://jrpg.com/stablecoinforgames/docs/developers/unity/api#scfgsettings")]
    public sealed class ScfgSettings : ScriptableObject
    {
        /// <summary>One paid unlock of the asset (SCFG.AddItem).</summary>
        [Serializable]
        public sealed class Item
        {
            [Tooltip("The item's id, as SCFG.Owns(\"chapter2\") and SCFG.Pay ask for it.")]
            public string id = "";
            [Tooltip("1..99, unique per game. Never change it after release: receipts carry it.")]
            public int code = 1;
            [Tooltip("The price as a decimal string, such as 4.99.")]
            public string price = "";
            [Tooltip("USDC or USDT.")]
            public string token = "USDC";
            [Tooltip("What the player sees on the payment sheet.")]
            public string title = "";
            [Tooltip("Optional: the least a payment may be and still unlock (default: the price).")]
            public string minPrice = "";

            internal ScfgItem ToItem() => new ScfgItem
            {
                Code = code, Price = price, Token = token, Title = title,
                MinPrice = string.IsNullOrEmpty(minPrice) ? null : minPrice,
            };
        }

        [Tooltip("Your EVM payout address (0x...). At least one of the two wallets is required.")]
        public string wallet = "";
        [Tooltip("Your Solana payout address (base58).")]
        public string solanaWallet = "";
        [Tooltip("Use the test networks. Develop with this on; turn it off for the release.")]
        public bool testnet = true;
        [Tooltip("The networks offered, in order (empty: every network your wallets can receive on).")]
        public List<string> chains = new List<string>();
        [Tooltip("The tokens accepted (empty: USDC and USDT).")]
        public List<string> tokens = new List<string>();
        [Tooltip("The accent color of the sheet and the checkout page, #rrggbb (empty: the default).")]
        public string accent = "";
        [Tooltip("The logo on the sheet and the checkout page.")]
        public Sprite? logo;
        [Tooltip("WebGL builds: tells two games with the same product name on one web origin apart (see ScfgOptions.WebStorageId). Set it once and never change it.")]
        public string webStorageId = "";
        [Tooltip("The paid unlocks.")]
        public List<Item> items = new List<Item>();

        /// <summary>The asset as Configure's options.</summary>
        public ScfgOptions ToOptions() => new ScfgOptions
        {
            Wallet = Blank(wallet),
            SolanaWallet = Blank(solanaWallet),
            Testnet = testnet,
            Chains = chains != null && chains.Count > 0 ? new List<string>(chains) : null,
            Tokens = tokens != null && tokens.Count > 0 ? new List<string>(tokens) : null,
            Accent = Blank(accent),
            Logo = LogoTexture(),
            WebStorageId = Blank(webStorageId),
        };

        private static string? Blank(string? s) => string.IsNullOrWhiteSpace(s) ? null : s!.Trim();

        /// <summary>The logo sprite as a texture: its texture when the sprite is all of it, else a cropped copy.</summary>
        private Texture2D? LogoTexture()
        {
            if (logo == null) return null;
            var texture = logo.texture;
            var rect = logo.textureRect;
            if (texture == null) return null;
            if ((int)rect.width == texture.width && (int)rect.height == texture.height) return texture;
            if (!texture.isReadable)
            {
                SCFG.Warn("the SCFG Settings logo is part of a larger texture that is not readable, so the whole texture is"
                          + " shown. Use a sprite that is a texture of its own, or tick Read/Write.");
                return texture;
            }
            var copy = new Texture2D((int)rect.width, (int)rect.height, TextureFormat.RGBA32, false) { name = logo.name };
            copy.SetPixels(texture.GetPixels((int)rect.x, (int)rect.y, (int)rect.width, (int)rect.height));
            copy.Apply();
            return copy;
        }

        /// <summary>
        /// Apply the asset: SCFG.Configure, then SCFG.AddItem for each item. Returns the first error code (reported, as
        /// every SCFG misuse, with Debug.LogError), or null.
        /// </summary>
        public string? Apply()
        {
            var err = SCFG.Configure(ToOptions());
            if (err != null)
            {
                SCFG.Report("the SCFG Settings asset \"" + name + "\" was not applied (" + err + "). Press Validate in its inspector.");
                return err;
            }
            foreach (var item in items ?? new List<Item>())
            {
                if (item == null) continue;
                err = SCFG.AddItem(item.id, item.ToItem());
                if (err != null)
                {
                    SCFG.Report("the SCFG Settings asset \"" + name + "\": item \"" + item.id + "\" was not added (" + err + ").");
                    return err;
                }
            }
            return null;
        }

        /// <summary>What <see cref="Validate"/> found wrong: the core's code and message.</summary>
        public sealed class Problem
        {
            public string Code { get; }
            public string Message { get; }
            public Problem(string code, string message)
            {
                Code = code;
                Message = message;
            }
        }

        /// <summary>The core's validation of the asset, without touching SCFG: the first problem, or null.</summary>
        public Problem? Validate()
        {
            ConfigData cfg;
            try
            {
                var kw = ToOptions().ToKwargs();
                kw["custom_chains"] = new List<object?>();
                cfg = Scfg.Core.Config.BuildConfig(kw);
            }
            catch (Exception e) when (e is ValueError || e is TypeError || e is KeyError)
            {
                return new Problem(SCFG.ErrCode(e), e.Message);
            }
            foreach (var item in items ?? new List<Item>())
            {
                if (item == null) continue;
                try
                {
                    Scfg.Core.Config.AddItem(cfg, item.id, item.ToItem().ToKwargs());
                }
                catch (Exception e) when (e is ValueError || e is TypeError || e is KeyError)
                {
                    return new Problem(SCFG.ErrCode(e), "item \"" + item.id + "\": " + e.Message);
                }
            }
            return null;
        }

        // --- applied at startup ------------------------------------------------------------------------------

        /// <summary>The settings asset of this player or project, or null (see the file header).</summary>
        public static ScfgSettings? Find()
        {
            var found = new List<ScfgSettings>();
#if UNITY_EDITOR
            foreach (var guid in UnityEditor.AssetDatabase.FindAssets("t:" + nameof(ScfgSettings)))
            {
                var asset = UnityEditor.AssetDatabase.LoadAssetAtPath<ScfgSettings>(UnityEditor.AssetDatabase.GUIDToAssetPath(guid));
                if (asset != null) found.Add(asset);
            }
            found.Sort((a, b) => string.CompareOrdinal(UnityEditor.AssetDatabase.GetAssetPath(a), UnityEditor.AssetDatabase.GetAssetPath(b)));
#else
            found.AddRange(Resources.FindObjectsOfTypeAll<ScfgSettings>());
            found.Sort((a, b) => string.CompareOrdinal(a.name, b.name));
#endif
            if (found.Count == 0) return null;
            if (found.Count > 1)
                SCFG.Report(found.Count + " SCFG Settings assets were found; \"" + found[0].name + "\" is used. Keep one per project.");
            return found[0];
        }

        /// <summary>Test hook: when set, the startup application uses this instead of looking (null: none).</summary>
        internal static Func<ScfgSettings?>? FindOverride;

        [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.SubsystemRegistration)]
        private static void ResetStatics() => FindOverride = null;

        [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.BeforeSceneLoad)]
        internal static void ApplyAtStartup()
        {
            var settings = FindOverride != null ? FindOverride() : Find();
            if (settings != null) settings.Apply();
        }
    }
}
