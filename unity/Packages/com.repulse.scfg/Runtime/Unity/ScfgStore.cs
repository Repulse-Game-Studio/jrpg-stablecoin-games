// Twin of godot/addons/scfg/store.gd: the player's payment state on disk, Application.persistentDataPath/scfg/state.json,
// global to the install and never inside a save slot (Ren'Py's persistent.scfg_pending / persistent.scfg_receipts).
//
// The file is {"version": 1, "pending": {...}, "receipts": {...}}, the Engine's two stores as they are
// (conformance/schema/state.json); StateFile.Parse / StateFile.Serialize do the format and the schema check.
//
// - Load(path): a missing file gives empty stores and no warning. When the file is missing but <path>.tmp exists (a
//   save stopped between deleting the old file and moving the new one in), the .tmp is read. A file StateFile.Parse
//   refuses (not JSON, not an object, an unknown version, no stores, a pending record off the schema), or bytes that
//   are not UTF-8, is moved aside to <path>.corrupt (then .corrupt.2, ...) and the stores start empty with a warning
//   naming the kept file, so the next save cannot overwrite what support might recover. Receipt records off the schema
//   keep the file (the Engine's reverify removes them and reports each); ReceiptProblems lists them. A file that cannot
//   be read right now (a permission, a lock, an I/O error), or a bad one that cannot be moved aside, is left alone and
//   Error is set: the caller must not save over it this run.
// - Save(path, pending, receipts): writes <path>.tmp (flushed to disk), then replaces <path> with it, so a crash leaves
//   the previous file whole. Returns null or the error.
#nullable enable
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using Scfg.Core;

namespace Scfg
{
    /// <summary>What <see cref="ScfgStore.Load"/> read.</summary>
    public sealed class ScfgStoreLoad
    {
        public JsonObject Pending { get; }
        public JsonObject Receipts { get; }
        /// <summary>Developer advice: the file was corrupt and moved aside (debug builds show it).</summary>
        public string? Warning { get; }
        /// <summary>The file exists but cannot be read now: never save over it this run.</summary>
        public Exception? Error { get; }
        /// <summary>Receipt records that do not match the schema (kept: reverify removes them).</summary>
        public IReadOnlyList<string> ReceiptProblems { get; }

        public ScfgStoreLoad(JsonObject? pending, JsonObject? receipts, string? warning, Exception? error,
                             IReadOnlyList<string>? receiptProblems = null)
        {
            Pending = pending ?? new JsonObject();
            Receipts = receipts ?? new JsonObject();
            Warning = warning;
            Error = error;
            ReceiptProblems = receiptProblems ?? Array.Empty<string>();
        }
    }

    /// <summary>The state file (see the file header).</summary>
    public static class ScfgStore
    {
        /// <summary>Strict UTF-8: invalid bytes are a corrupt file, never U+FFFD-patched text.</summary>
        private static readonly UTF8Encoding Strict = new UTF8Encoding(false, true);

        private static readonly UTF8Encoding Utf8 = new UTF8Encoding(false);

        /// <summary>True when an entry is at <paramref name="path"/>, readable or not.</summary>
        private static bool Exists(string path) => File.Exists(path) || Directory.Exists(path);

        public static ScfgStoreLoad Load(string path)
        {
            string source = path;
            if (!Exists(path))
            {
                if (!File.Exists(path + ".tmp")) return new ScfgStoreLoad(null, null, null, null); // a file, never a folder
                source = path + ".tmp";
            }
            byte[] raw;
            try
            {
                raw = File.ReadAllBytes(source);
            }
            catch (Exception e) when (e is IOException || e is UnauthorizedAccessException || e is NotSupportedException
                                      || e is System.Security.SecurityException)
            {
                return new ScfgStoreLoad(null, null, null, new IOException("cannot read " + source + ": " + e.Message, e));
            }
            string? why;
            StateLoad? parsed = null;
            string text;
            try
            {
                text = Strict.GetString(raw);
                parsed = StateFile.Parse(text);
                why = parsed.Why;
            }
            catch (DecoderFallbackException)
            {
                why = "is not valid UTF-8";
            }
            catch (ArgumentException)
            {
                why = "is not valid UTF-8";
            }
            if (why == null && parsed != null)
                return new ScfgStoreLoad(parsed.Pending, parsed.Receipts, null, null, parsed.ReceiptProblems);
            string aside = path + ".corrupt";
            int n = 1;
            while (Exists(aside))
            {
                n++;
                aside = path + ".corrupt." + n.ToString(System.Globalization.CultureInfo.InvariantCulture);
            }
            try
            {
                File.Move(source, aside);
            }
            catch (Exception e) when (e is IOException || e is UnauthorizedAccessException || e is NotSupportedException)
            {
                // starting empty would let the next save replace it with nothing kept anywhere
                return new ScfgStoreLoad(null, null, null, new IOException(
                    "the payment state file " + source + " " + why + ", and it could not be moved aside (" + e.Message + ")", e));
            }
            return new ScfgStoreLoad(null, null,
                "the payment state file " + source + " " + why + "; starting with no receipts or pending payments (the file was kept as "
                + aside + "). Players can bring purchases back with Restore.", null);
        }

        /// <summary>Writes the stores to <paramref name="path"/> atomically; null, or the error.</summary>
        public static Exception? Save(string path, JsonObject pending, JsonObject receipts)
        {
            string text;
            try
            {
                text = StateFile.Serialize(pending, receipts);
            }
            catch (Exception e)
            {
                return e;
            }
            string tmp = path + ".tmp";
            try
            {
                var dir = Path.GetDirectoryName(Path.GetFullPath(path));
                if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
                using (var f = new FileStream(tmp, FileMode.Create, FileAccess.Write, FileShare.None))
                {
                    var bytes = Utf8.GetBytes(text);
                    f.Write(bytes, 0, bytes.Length);
                    f.Flush(true);
                }
            }
            catch (Exception e) when (e is IOException || e is UnauthorizedAccessException || e is NotSupportedException
                                      || e is System.Security.SecurityException)
            {
                TryDelete(tmp);
                return new IOException("cannot write " + tmp + ": " + e.Message, e);
            }
            try
            {
                if (File.Exists(path))
                {
                    try
                    {
                        File.Replace(tmp, path, null);
                    }
                    catch (PlatformNotSupportedException)
                    {
                        File.Delete(path); // Load reads the .tmp if a crash comes between the two steps
                        File.Move(tmp, path);
                    }
                }
                else
                {
                    File.Move(tmp, path);
                }
            }
            catch (Exception e) when (e is IOException || e is UnauthorizedAccessException || e is NotSupportedException)
            {
                return new IOException("cannot replace " + path + ": " + e.Message, e);
            }
            return null;
        }

        private static void TryDelete(string path)
        {
            try
            {
                if (File.Exists(path)) File.Delete(path);
            }
            catch (Exception e) when (e is IOException || e is UnauthorizedAccessException)
            {
                // the .tmp is overwritten by the next save anyway
            }
        }
    }
}
