// Twin of godot/addons/scfg/web_mirror.gd: a second copy of the receipts in a WebGL build, in the page's localStorage.
//
// Every Unity WebGL build on one origin shares one IndexedDB database (/idbfs) and writes back the whole tree it loaded
// at startup, so another game on the origin can delete persistentDataPath/scfg/state.json. localStorage is written key
// by key, so this key survives (SCFG.cs, WEB: A SECOND COPY OF THE RECEIPTS).
//
// The key is "scfg:<Application.productName>:<sha256 hex of the storage id>", Godot's scheme: the storage id is
// ScfgOptions.WebStorageId when set, else the product name (so two games sharing a product name can tell their copies
// apart). The value is {"version": 1, "receipts": {...}}: receipts only, as a lost pending session is brought back with
// Restore.
//
// The page answers through IScfgWebJs.Storage, which catches everything, so a missing, blocked or full localStorage is
// an answer, never an exception.
// - Read() gives the receipts, a warning and whether localStorage is usable. A value that is not that document: empty
//   receipts and a warning. localStorage unusable: Available false, with a warning.
// - Write(receipts) replaces the copy; null, or a warning when the browser refused.
// - Update(receipts, dropped) merges instead, since a game open in two tabs would otherwise drop the other tab's new
//   receipts: it removes the keys in dropped (receipts this tab removed itself), adds or replaces every receipt of
//   receipts, and writes unless nothing changed. Null, or a warning.
// The receipts read are re-verified like any stored receipt.
#nullable enable
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using Scfg.Core;

namespace Scfg
{
    /// <summary>What <see cref="ScfgWebMirror.Read"/> found.</summary>
    public sealed class ScfgWebMirrorRead
    {
        public JsonObject Receipts { get; }
        public string? Warning { get; }
        /// <summary>False when localStorage cannot be used at all (no copy is kept this run).</summary>
        public bool Available { get; }

        public ScfgWebMirrorRead(JsonObject receipts, string? warning, bool available)
        {
            Receipts = receipts;
            Warning = warning;
            Available = available;
        }
    }

    /// <summary>The receipts' copy in localStorage (see the file header).</summary>
    public sealed class ScfgWebMirror
    {
        public const long Version = 1;
        public const string Prefix = "scfg:";

        private readonly IScfgWebJs _js;

        public string Key { get; }

        public ScfgWebMirror(IScfgWebJs js, string key)
        {
            _js = js ?? throw new TypeError("a ScfgWebMirror needs the page");
            Key = key ?? throw new TypeError("a ScfgWebMirror needs a key");
        }

        /// <summary>The key for a product name and storage id (see the file header).</summary>
        public static string KeyFor(string productName, string? storageId = null)
        {
            var id = string.IsNullOrEmpty(storageId) ? productName : storageId!;
            return Prefix + productName + ":" + Bytes.ToHex(Sha256.Hash(new UTF8Encoding(false).GetBytes(id))).ToLowerInvariant();
        }

        public ScfgWebMirrorRead Read()
        {
            var answer = Answer(_js.Storage("get", Key, null), out var refused);
            if (answer == null)
                return new ScfgWebMirrorRead(new JsonObject(),
                    "the page's localStorage cannot be read (" + refused + "), so the receipts have no second copy there"
                    + " (localStorage key " + Key + "). See \"WebGL builds\" in the package's documentation.", false);
            var value = answer.Get("value");
            if (value == null) return new ScfgWebMirrorRead(new JsonObject(), null, true);
            string why = "";
            object? doc = null;
            if (!(value is string text)) why = "is not valid JSON";
            else
            {
                try
                {
                    doc = Json.Loads(text);
                }
                catch (Exception)
                {
                    why = "is not valid JSON";
                }
            }
            if (why.Length == 0)
            {
                if (!(doc is JsonObject d)) why = "is not a JSON object";
                else if (!(d.Get("version") is long v && v == Version)) why = "has an unknown version " + Py.Repr(d.Get("version"));
                else if (!(d.Get("receipts") is JsonObject receipts)) why = "has no receipts object";
                else return new ScfgWebMirrorRead(receipts, null, true);
            }
            return new ScfgWebMirrorRead(new JsonObject(),
                "the receipts' copy in localStorage (key " + Key + ") " + why + "; it is ignored and replaced at the next save.", true);
        }

        /// <summary>Replaces the copy with <paramref name="receipts"/>; null, or a warning.</summary>
        public string? Write(JsonObject receipts)
        {
            var text = Json.Dumps(new JsonObject { { "version", Version }, { "receipts", receipts } });
            if (Answer(_js.Storage("set", Key, text), out var refused) != null) return null;
            return "the browser did not keep the receipts' copy in localStorage (" + refused + "; key " + Key + "), so the"
                   + " receipts are only in persistentDataPath, where another game on this origin can delete them. See"
                   + " \"WebGL builds\" in the package's documentation.";
        }

        /// <summary>Merges <paramref name="receipts"/> into the copy (see the file header); null, or a warning.</summary>
        public string? Update(JsonObject receipts, ISet<string> dropped)
        {
            var got = Read();
            var current = got.Receipts;
            var merged = new JsonObject();
            foreach (var kv in current)
                if (!dropped.Contains(kv.Key))
                    merged[kv.Key] = receipts.TryGetValue(kv.Key, out var mine) ? mine : kv.Value;
            foreach (var kv in receipts) merged[kv.Key] = kv.Value;
            if (got.Warning == null && Json.Equal(merged, current) && merged.Keys.SequenceEqual(current.Keys))
                return null; // the copy already holds exactly this
            return Write(merged);
        }

        /// <summary>The page's answer as an object, or null with the reason it is not one (the browser's error, or no JSON answer).</summary>
        private static JsonObject? Answer(string? raw, out string refused)
        {
            refused = "the page gave no answer";
            object? data;
            try
            {
                data = raw == null ? null : Json.Loads(raw);
            }
            catch (Exception)
            {
                return null;
            }
            if (!(data is JsonObject o)) return null;
            if (o.ContainsKey("error"))
            {
                refused = Coio.Str(o.Get("error"));
                return null;
            }
            return o;
        }
    }
}
