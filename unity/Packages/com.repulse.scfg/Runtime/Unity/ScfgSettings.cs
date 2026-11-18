// No Python twin: the optional SCFG Settings asset (Assets > Create > SCFG Settings), configuration without code. Code
// stays the source of truth: the asset is applied before the first scene loads through the same SCFG.Configure and
// SCFG.AddItem calls, and a Configure from code made after it replaces it (with its items).
//
// Where it is found: in a player, the asset the build put in its preloaded assets (Scfg.Editor.ScfgSettingsBuild adds
// the project's one for the build only); in the editor's play mode, the project's one through the asset database. A
// project holds one: with several, none is applied (the editor's play mode and the player report it, and a build
// fails), so the editor and the player can never pick different assets.
//
// The logo: a sprite that is a whole texture is used as is. Any other sprite (a sheet's cell, a Sprite Atlas entry,
// tight or rotated) is cut out into a copy the asset owns: read directly from a readable, uncompressed texture, else
// through the GPU (Blit to a RenderTexture, ReadPixels), so Read/Write, compression and packing do not matter. Where
// it cannot be cut out (no graphics device) the whole texture is shown with a warning; an error on the way is
// reported and the asset is applied without a logo. Nothing here throws at startup. The asset keeps one copy per
// sprite (reused by every Apply and ToOptions), destroys it when it makes another or is unloaded, and every play
// session starts by dropping the last session's copies.
//
// No wallet: an asset with neither wallet is not applied at startup (opt-out: the game configures from code), and its
// items, chains and options can still be applied with a wallet the game supplies at runtime, Apply(wallet,
// solanaWallet) (a wallet per build, or read from a file). It is never an error.
//
// Validate() runs the core's validation on the asset without touching SCFG (the inspector's Validate button): the first
// ConfigError's code and message, or null when the configuration and every item are valid. It does not need the logo.
// An asset without a wallet is validated with stand-in wallets, so the rest is checked and the missing wallet is not a
// problem (HasWallet says it is not applied at startup).
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
        [Tooltip("The \"No USDC yet? Buy some with a card\" link on the sheet (on mainnet, on the built-in EVM networks). Off hides it.")]
        public bool onrampEnabled = true;
        [Tooltip("Optional: your own buy page, an https:// template with {chain}, {chain_id}, {token}, {token_address} and {amount} (empty: thirdweb's buy page).")]
        public string onrampUrl = "";
        [Tooltip("Optional: the EVM networks the buy link is offered on (empty: the built-in EVM networks of a mainnet game; none on testnet).")]
        public List<string> onrampChains = new List<string>();
        [Tooltip("WebGL builds: tells two games with the same product name on one web origin apart (see ScfgOptions.WebStorageId). Set it once and never change it.")]
        public string webStorageId = "";
        [Tooltip("The paid unlocks.")]
        public List<Item> items = new List<Item>();

        /// <summary>
        /// The asset as Configure's options. The logo may be a copy this asset owns (see the file header): it stays valid
        /// until the asset makes another or is unloaded.
        /// </summary>
        public ScfgOptions ToOptions() => ToOptions(true);

        /// <summary>A wallet is set (EVM or Solana): the asset is applied at startup. Without one it is not (opt-out).</summary>
        public bool HasWallet => Blank(wallet) != null || Blank(solanaWallet) != null;

        internal ScfgOptions ToOptions(bool withLogo) => new ScfgOptions
        {
            Wallet = Blank(wallet),
            SolanaWallet = Blank(solanaWallet),
            Testnet = testnet,
            Chains = chains != null && chains.Count > 0 ? new List<string>(chains) : null,
            Tokens = tokens != null && tokens.Count > 0 ? new List<string>(tokens) : null,
            Accent = Blank(accent),
            Onramp = OnrampOptions(),
            Logo = withLogo ? LogoTexture() : null,
            WebStorageId = Blank(webStorageId),
        };

        /// <summary>The buy link's options, or null when every field keeps the default.</summary>
        private ScfgOnramp? OnrampOptions()
        {
            var url = Blank(onrampUrl);
            var keys = onrampChains != null && onrampChains.Count > 0 ? new List<string>(onrampChains) : null;
            if (onrampEnabled && url == null && keys == null) return null;
            return new ScfgOnramp { Enabled = onrampEnabled ? (bool?)null : false, Url = url, Chains = keys };
        }

        private static string? Blank(string? s) => string.IsNullOrWhiteSpace(s) ? null : s!.Trim();

        // --- the logo (see the file header) --------------------------------------------------------------

        [NonSerialized] private Texture2D? _logoCopy;
        [NonSerialized] private Sprite? _logoCopyOf;

        /// <summary>Test hook: the copy of the logo this asset owns, or null.</summary>
        internal Texture2D? LogoCopy => _logoCopy;

        /// <summary>The logo sprite as a texture, or null (no logo, or one that failed: reported). Never throws.</summary>
        internal Texture2D? LogoTexture()
        {
            if (logo == null) return null;
            try
            {
                return LogoTextureOf(logo);
            }
            catch (Exception e)
            {
                SCFG.ReportError("the SCFG Settings logo \"" + logo.name + "\" cannot be used, so the sheet shows none:", e);
                return null;
            }
        }

        private Texture2D? LogoTextureOf(Sprite sprite)
        {
            var texture = sprite.texture;
            if (texture == null)
            {
                SCFG.Warn("the SCFG Settings logo \"" + sprite.name + "\" has no texture (a Sprite Atlas that is not loaded?), so"
                          + " the sheet shows no logo.");
                return null;
            }
            // textureRect throws for a tightly packed sprite, and a rotated one is not an upright rectangle of the atlas
            bool byRect = !sprite.packed
                          || (sprite.packingMode == SpritePackingMode.Rectangle && sprite.packingRotation == SpritePackingRotation.None);
            RectInt rect = default;
            if (byRect)
            {
                var r = sprite.textureRect;
                rect = new RectInt(Mathf.RoundToInt(r.x), Mathf.RoundToInt(r.y), Mathf.RoundToInt(r.width), Mathf.RoundToInt(r.height));
                if (rect.x == 0 && rect.y == 0 && rect.width == texture.width && rect.height == texture.height) return texture;
            }
            if (_logoCopy != null && _logoCopyOf == sprite) return _logoCopy;
            var copy = byRect ? Crop(texture, rect) : Render(sprite, texture);
            if (copy == null)
            {
                SCFG.Warn("the SCFG Settings logo \"" + sprite.name + "\" could not be cut out of its texture (no graphics"
                          + " device), so the whole texture is shown.");
                return texture;
            }
            copy.name = sprite.name;
            // not DontUnloadUnusedAsset: a copy SCFG still showed when it was replaced goes with the next unused-asset sweep
            copy.hideFlags = HideFlags.HideInHierarchy | HideFlags.DontSaveInEditor | HideFlags.DontSaveInBuild;
            DropLogoCopy();
            _logoCopy = copy;
            _logoCopyOf = sprite;
            return copy;
        }

        /// <summary>Destroys the logo copy this asset made, unless SCFG still shows it (then a later Configure lets it go).</summary>
        private void DropLogoCopy(bool evenIfShown = false)
        {
            var copy = _logoCopy;
            _logoCopy = null;
            _logoCopyOf = null;
            if (copy == null || (!evenIfShown && ReferenceEquals(SCFG.Current.Logo, copy))) return;
            if (Application.isPlaying) Destroy(copy);
            else DestroyImmediate(copy);
        }

        // unloaded, or the scripts reload (SCFG's state goes with them)
        private void OnDisable() => DropLogoCopy(true);

        /// <summary>The rect of a texture as a readable copy, or null when no graphics device can read it.</summary>
        internal static Texture2D? Crop(Texture2D texture, RectInt rect)
        {
            var pixels = Read(texture, rect);
            if (pixels == null) return null;
            var copy = new Texture2D(rect.width, rect.height, TextureFormat.RGBA32, false);
            copy.SetPixels32(pixels);
            copy.Apply();
            return copy;
        }

        /// <summary>
        /// A packed sprite (tight or rotated in its atlas) drawn upright into a copy of its own size: each pixel of the
        /// sprite's rect that its mesh covers takes the atlas pixel its UV points at. Null when the atlas cannot be read.
        /// </summary>
        internal static Texture2D? Render(Sprite sprite, Texture2D texture)
        {
            int w = Mathf.Max(1, Mathf.RoundToInt(sprite.rect.width)), h = Mathf.Max(1, Mathf.RoundToInt(sprite.rect.height));
            var vertices = sprite.vertices;
            var uv = sprite.uv;
            var triangles = sprite.triangles;
            // the part of the atlas the mesh reads
            float u0 = 1, v0 = 1, u1 = 0, v1 = 0;
            foreach (var t in uv)
            {
                u0 = Mathf.Min(u0, t.x); v0 = Mathf.Min(v0, t.y);
                u1 = Mathf.Max(u1, t.x); v1 = Mathf.Max(v1, t.y);
            }
            int ax = Mathf.Clamp(Mathf.FloorToInt(u0 * texture.width), 0, texture.width - 1);
            int ay = Mathf.Clamp(Mathf.FloorToInt(v0 * texture.height), 0, texture.height - 1);
            int aw = Mathf.Clamp(Mathf.CeilToInt(u1 * texture.width), ax + 1, texture.width) - ax;
            int ah = Mathf.Clamp(Mathf.CeilToInt(v1 * texture.height), ay + 1, texture.height) - ay;
            var source = Read(texture, new RectInt(ax, ay, aw, ah));
            if (source == null) return null;
            var output = new Color32[w * h];
            var pivot = sprite.pivot;
            float ppu = sprite.pixelsPerUnit;
            for (int i = 0; i + 2 < triangles.Length; i += 3)
            {
                int a = triangles[i], b = triangles[i + 1], c = triangles[i + 2];
                // the vertices in the sprite's pixels (its rect, origin bottom left)
                Vector2 pa = vertices[a] * ppu + pivot, pb = vertices[b] * ppu + pivot, pc = vertices[c] * ppu + pivot;
                float area = (pb.x - pa.x) * (pc.y - pa.y) - (pc.x - pa.x) * (pb.y - pa.y);
                if (Mathf.Abs(area) < 1e-6f) continue;
                int x0 = Mathf.Max(0, Mathf.FloorToInt(Mathf.Min(pa.x, Mathf.Min(pb.x, pc.x))));
                int x1 = Mathf.Min(w - 1, Mathf.CeilToInt(Mathf.Max(pa.x, Mathf.Max(pb.x, pc.x))));
                int y0 = Mathf.Max(0, Mathf.FloorToInt(Mathf.Min(pa.y, Mathf.Min(pb.y, pc.y))));
                int y1 = Mathf.Min(h - 1, Mathf.CeilToInt(Mathf.Max(pa.y, Mathf.Max(pb.y, pc.y))));
                for (int y = y0; y <= y1; y++)
                for (int x = x0; x <= x1; x++)
                {
                    float px = x + 0.5f, py = y + 0.5f;
                    float wa = ((pb.x - px) * (pc.y - py) - (pc.x - px) * (pb.y - py)) / area;
                    float wb = ((pc.x - px) * (pa.y - py) - (pa.x - px) * (pc.y - py)) / area;
                    float wc = 1 - wa - wb;
                    const float eps = -1e-4f;
                    if (wa < eps || wb < eps || wc < eps) continue;
                    var t = uv[a] * wa + uv[b] * wb + uv[c] * wc;
                    int sx = Mathf.Clamp(Mathf.FloorToInt(t.x * texture.width) - ax, 0, aw - 1);
                    int sy = Mathf.Clamp(Mathf.FloorToInt(t.y * texture.height) - ay, 0, ah - 1);
                    output[y * w + x] = source[sy * aw + sx];
                }
            }
            var copy = new Texture2D(w, h, TextureFormat.RGBA32, false);
            copy.SetPixels32(output);
            copy.Apply();
            return copy;
        }

        /// <summary>A rect of a texture's pixels (row by row from the bottom), or null when no graphics device can read it.</summary>
        private static Color32[]? Read(Texture2D texture, RectInt rect)
        {
            if (texture.isReadable && !IsCrunched(texture.format))
            {
                try
                {
                    var colors = texture.GetPixels(rect.x, rect.y, rect.width, rect.height);
                    var pixels = new Color32[colors.Length];
                    for (int i = 0; i < colors.Length; i++) pixels[i] = colors[i];
                    return pixels;
                }
                catch (Exception)
                {
                    // a format the CPU cannot read: through the GPU below
                }
            }
            if (SystemInfo.graphicsDeviceType == UnityEngine.Rendering.GraphicsDeviceType.Null) return null;
            var rt = RenderTexture.GetTemporary(texture.width, texture.height, 0, RenderTextureFormat.ARGB32,
                texture.isDataSRGB ? RenderTextureReadWrite.sRGB : RenderTextureReadWrite.Linear);
            var previous = RenderTexture.active;
            var read = new Texture2D(rect.width, rect.height, TextureFormat.RGBA32, false);
            try
            {
                Graphics.Blit(texture, rt);
                RenderTexture.active = rt;
                read.ReadPixels(new Rect(rect.x, rect.y, rect.width, rect.height), 0, 0);
                return read.GetPixels32();
            }
            finally
            {
                RenderTexture.active = previous;
                RenderTexture.ReleaseTemporary(rt);
                if (Application.isPlaying) Destroy(read);
                else DestroyImmediate(read);
            }
        }

        private static bool IsCrunched(TextureFormat f) =>
            f == TextureFormat.DXT1Crunched || f == TextureFormat.DXT5Crunched || f == TextureFormat.ETC_RGB4Crunched
            || f == TextureFormat.ETC2_RGBA8Crunched;

        // --- applying and validating ---------------------------------------------------------------------

        /// <summary>
        /// Apply the asset: SCFG.Configure, then SCFG.AddItem for each item. Returns the first error code (reported, as
        /// every SCFG misuse, with Debug.LogError), or null.
        /// </summary>
        public string? Apply() => ApplyOptions(ToOptions());

        /// <summary>
        /// Apply the asset with the payout wallets given here instead of its own (a wallet supplied at runtime: per build,
        /// or read from a file): its options, then its items, as <see cref="Apply()"/>. A null or blank wallet is not
        /// set; at least one is required (wallet.missing otherwise). Call it before payments start (the first scene's
        /// Awake), as any Configure.
        /// </summary>
        public string? Apply(string? wallet, string? solanaWallet = null)
        {
            var options = ToOptions();
            options.Wallet = Blank(wallet);
            options.SolanaWallet = Blank(solanaWallet);
            return ApplyOptions(options);
        }

        private string? ApplyOptions(ScfgOptions options)
        {
            var err = SCFG.Configure(options);
            if (err != null)
            {
                SCFG.Report("the SCFG Settings asset \"" + name + "\" was not applied (" + err + "). Press Validate in its inspector.");
                return err;
            }
            SCFG.Current.ConfigFromSettings = true;
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

        // Validate's stand-ins for an asset without a wallet (the EIP-55 spec's example address, an on-curve example key).
        private const string StandInWallet = "0x52908400098527886E0F7030069857D2E4169EE7";
        private const string StandInSolanaWallet = "7xKXtg2CW87d97TXJSDpbD5jBkheTqA83TZRuJosgAsU";

        /// <summary>The core's validation of the asset, without touching SCFG: the first problem, or null.</summary>
        public Problem? Validate()
        {
            ConfigData cfg;
            try
            {
                var options = ToOptions(false);
                if (!HasWallet)
                {
                    // not applied at startup (opt-out), not a problem: check the rest with stand-ins, never applied
                    options.Wallet = StandInWallet;
                    options.SolanaWallet = StandInSolanaWallet;
                }
                var kw = options.ToKwargs();
                kw["custom_chains"] = new List<object?>();
                cfg = Scfg.Core.Config.BuildConfig(kw);
            }
            catch (Exception e) when (e is ValueError || e is TypeError || e is KeyError)
            {
                return new Problem(SCFG.ErrCode(e), SCFG.Spelled(e));
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

        /// <summary>
        /// The settings asset of this player or project, or null: none, or several (reported, see the file header).
        /// </summary>
        public static ScfgSettings? Find()
        {
            var found = FindAll();
            if (found.Count == 0) return null;
            if (found.Count > 1)
            {
                SCFG.Report(found.Count + " SCFG Settings assets were found (" + string.Join(", ", found.ConvertAll(Describe))
                            + "), so none is applied. Keep one per project.");
                return null;
            }
            return found[0];
        }

        /// <summary>Every settings asset of this player (the loaded ones) or project (the asset database), by path or name.</summary>
        internal static List<ScfgSettings> FindAll()
        {
            var found = new List<ScfgSettings>();
#if UNITY_EDITOR
            var filter = "t:" + nameof(ScfgSettings);
            var guids = SearchFoldersOverride != null
                ? UnityEditor.AssetDatabase.FindAssets(filter, SearchFoldersOverride)
                : UnityEditor.AssetDatabase.FindAssets(filter);
            foreach (var guid in guids)
            {
                var asset = UnityEditor.AssetDatabase.LoadAssetAtPath<ScfgSettings>(UnityEditor.AssetDatabase.GUIDToAssetPath(guid));
                if (asset != null && !found.Contains(asset)) found.Add(asset);
            }
#else
            found.AddRange(Resources.FindObjectsOfTypeAll<ScfgSettings>());
#endif
            found.Sort((a, b) => string.CompareOrdinal(Describe(a), Describe(b)));
            return found;
        }

        /// <summary>The asset's path in the editor, its name in a player.</summary>
        internal static string Describe(ScfgSettings s)
        {
#if UNITY_EDITOR
            var path = UnityEditor.AssetDatabase.GetAssetPath(s);
            if (!string.IsNullOrEmpty(path)) return path;
#endif
            return s.name;
        }

        /// <summary>Test hook: when set, the startup application uses this instead of looking (null: none).</summary>
        internal static Func<ScfgSettings?>? FindOverride;

        /// <summary>
        /// Test hook (editor): when set, FindAll looks only in these folders, so the package's tests run the same in a
        /// game project that has its own SCFG Settings asset (testbed U14).
        /// </summary>
        internal static string[]? SearchFoldersOverride;

        [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.SubsystemRegistration)]
        private static void ResetStatics()
        {
            FindOverride = null;
            // a new play session: the last session's state, which showed these copies, is gone
            foreach (var s in Resources.FindObjectsOfTypeAll<ScfgSettings>()) s.DropLogoCopy(true);
        }

        /// <summary>Applies the project's asset before the first scene. Never throws: a problem is reported.</summary>
        [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.BeforeSceneLoad)]
        internal static void ApplyAtStartup()
        {
            try
            {
                var settings = FindOverride != null ? FindOverride() : Find();
                if (settings == null) return;
                if (!settings.HasWallet)
                {
                    // opt-out: the game configures from code, maybe with this asset and a runtime wallet; a line
                    // only if nothing configured SCFG by the end of the first frame (ScfgSettingsNotice)
                    if ((settings.items?.Count ?? 0) > 0) ScfgSettingsNotice.Arm(settings.name);
                    return;
                }
                settings.Apply();
            }
            catch (Exception e)
            {
                SCFG.ReportError("the SCFG Settings asset could not be applied:", e);
            }
        }
    }
}
