// The facade's persistence (Godot's _stored / _sync / _save) and queries (owns, has_tipped, tip_total, receipts,
// pending, clear_local_data, export_receipts): see SCFG.cs for the overview.
#nullable enable
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using Scfg.Core;
using UnityEngine;

namespace Scfg
{
    public static partial class SCFG
    {
        /// <summary>
        /// Loads the mirrored state from disk on first use; on the web, the first call once configured also merges the
        /// receipts' copy in localStorage (SCFG.Web.cs).
        /// </summary>
        private static void Stored()
        {
            if (S.PersistedPending == null)
            {
                var got = ScfgStore.Load(StorePath);
                if (got.Warning != null) Warn(got.Warning);
                foreach (var p in got.ReceiptProblems) Warn("a stored receipt does not match the state schema (" + p + "); it is checked again and removed");
                S.StoreError = got.Error;
                S.PersistedPending = got.Pending;
                S.PersistedReceipts = got.Receipts;
            }
            // an unreadable state file leaves the copy alone too: its receipts are unknown
            if (!S.MirrorMerged && S.Config != null && S.StoreError == null && S.Platform.IsWeb) MergeMirror();
        }

        /// <summary>What must reach disk at once: which sessions exist, their state and tx hint.</summary>
        private static JsonObject PendingKey(JsonObject pending)
        {
            var key = new JsonObject();
            foreach (var kv in pending)
                key[kv.Key] = kv.Value is JsonObject r ? new List<object?> { r.Get("state"), r.Get("hint_tx") } : kv.Value;
            return key;
        }

        /// <summary>Mirror a snapshot to disk (see SCFG.cs's header).</summary>
        private static void Sync(Snapshot snap)
        {
            Stored();
            bool receiptsChanged = !Json.Equal(S.PersistedReceipts, snap.Receipts);
            bool pendingChanged = !Json.Equal(S.PersistedPending, snap.Pending);
            if (!receiptsChanged && !pendingChanged) return;
            bool sessionsChanged = !Json.Equal(PendingKey(S.PersistedPending!), PendingKey(snap.Pending));
            S.PersistedPending = (JsonObject)DeepCopy.Of(snap.Pending)!;
            S.PersistedReceipts = (JsonObject)DeepCopy.Of(snap.Receipts)!;
            S.Dirty = true;
            double now = S.Clock();
            if (receiptsChanged || sessionsChanged || now - S.LastSave >= PendingSaveInterval)
            {
                S.LastSave = now;
                Save();
            }
            if (receiptsChanged) EmitReceiptsChanged();
        }

        private static void EmitReceiptsChanged()
        {
            S.ReceiptsRev++;
            var handlers = S.ReceiptsChanged;
            if (handlers == null) return;
            try
            {
                handlers();
            }
            catch (Exception e)
            {
                Debug.LogException(e);
            }
        }

        /// <summary>
        /// Writes the state to disk, and on the web into the receipts' copy (<paramref name="clear"/>: the copy is emptied,
        /// for ClearLocalData); never over a file that could not be read.
        /// </summary>
        private static void Save(bool clear = false)
        {
            Stored();
            if (S.StoreError != null) return;
            var err = ScfgStore.Save(StorePath, S.PersistedPending!, S.PersistedReceipts!);
            if (err == null) SyncWebFs();
            WriteMirror(clear); // also when the file failed: the copy is then the only place they are kept
            if (err != null)
            {
                if (!S.SaveFailing) // once, not at every retry, until a save succeeds
                {
                    S.SaveFailing = true;
                    ReportError("cannot save the payment state:", err);
                }
                return;
            }
            S.SaveFailing = false;
            S.Dirty = false;
        }

        // --- queries -------------------------------------------------------------------------------------

        /// <summary>The Worker's snapshot when readers should see it, else null (before it starts, during ClearLocalData).</summary>
        private static Snapshot? LiveSnapshot() => S.Worker != null && S.Wipe == null ? S.Worker.Snapshot : null;

        /// <summary>The owned item ids, or null when the stored receipts cannot be read (reported).</summary>
        private static IReadOnlyList<string>? OwnedItems() => Ownership()?.Owned;

        private static (IReadOnlyList<string> Owned, Dec TipTotal)? Ownership()
        {
            var live = LiveSnapshot();
            if (live != null) return (live.Owned, live.TipTotal);
            if (S.Config == null) return null;
            Stored();
            try
            {
                var o = Scfg.Core.Receipts.ComputeOwnership(S.PersistedReceipts!, S.Config.Items);
                return (o.Owned, o.TipTotal);
            }
            catch (Exception e)
            {
                ReportError("cannot read the stored receipts:", e);
                return null;
            }
        }

        /// <summary>True if a verified receipt unlocks <paramref name="itemId"/>. Instant and offline. An unknown item is misuse (false).</summary>
        public static bool Owns(string itemId)
        {
            var cfg = RequireConfig(true);
            if (cfg == null) return false;
            if (itemId == null || !cfg.Items.ContainsKey(itemId))
            {
                Report("unknown item " + Py.Repr((object?)itemId) + "; declare it with SCFG.AddItem()");
                return false;
            }
            return OwnedItems()?.Contains(itemId) == true;
        }

        /// <summary>True once the player has tipped any amount.</summary>
        public static bool HasTipped()
        {
            if (RequireConfig(true) == null) return false;
            var o = Ownership();
            return o != null && !o.Value.TipTotal.IsZero && !o.Value.TipTotal.Negative;
        }

        /// <summary>The sum of the player's tips as a decimal string ("0", "2.50").</summary>
        public static string TipTotal()
        {
            if (RequireConfig(true) == null) return "0";
            var o = Ownership();
            return o == null ? "0" : o.Value.TipTotal.ToString();
        }

        private static JsonObject SourceReceipts()
        {
            var live = LiveSnapshot();
            if (live != null) return live.Receipts;
            Stored();
            return S.PersistedReceipts!;
        }

        private static JsonObject SourcePending()
        {
            var live = LiveSnapshot();
            if (live != null) return live.Pending;
            Stored();
            return S.PersistedPending!;
        }

        /// <summary>The stored receipts by receipt key (a copy).</summary>
        public static IReadOnlyDictionary<string, ScfgReceipt> Receipts()
        {
            var output = new Dictionary<string, ScfgReceipt>(StringComparer.Ordinal);
            foreach (var kv in SourceReceipts())
                if (kv.Value is JsonObject r) output[kv.Key] = new ScfgReceipt(kv.Key, r);
            return output;
        }

        /// <summary>The payments still being watched for, by session id (a copy).</summary>
        public static IReadOnlyDictionary<string, ScfgPendingPayment> Pending()
        {
            var output = new Dictionary<string, ScfgPendingPayment>(StringComparer.Ordinal);
            foreach (var kv in SourcePending())
                if (kv.Value is JsonObject r) output[kv.Key] = new ScfgPendingPayment(kv.Key, r);
            return output;
        }

        /// <summary>
        /// Testing helper: forget every receipt and pending payment on THIS device, to see your paywall again after a
        /// test purchase. Real purchases are not lost: they are still on the blockchain and Restore brings them back.
        /// </summary>
        public static void ClearLocalData()
        {
            bool hadReceipts = SourceReceipts().Count > 0;
            var worker = S.Worker;
            if (worker != null)
            {
                var wipe = new Wipe();
                wipe.Job = worker.Submit(Commands.Sync(engine =>
                {
                    foreach (var k in engine.Receipts.Keys.ToList()) engine.Receipts.Remove(k);
                    foreach (var k in engine.Pending.Keys.ToList()) engine.Pending.Remove(k);
                    wipe.Before = worker.Snapshot; // the last snapshot made before the wipe
                    wipe.Done = true;
                }));
                S.Wipe = wipe;
            }
            if (S.Platform.IsWeb && !S.MirrorMerged) S.ClearMirrorAtMerge = true; // before Configure: the copy is emptied at the merge
            S.PersistedPending = new JsonObject();
            S.PersistedReceipts = new JsonObject();
            S.LastSave = S.Clock();
            Save(true);
            if (hadReceipts) EmitReceiptsChanged();
        }

        /// <summary>
        /// Support helper: write the receipts as readable JSON (a list, sorted keys, indented), by default to
        /// persistentDataPath/scfg-receipts.json. Returns the file's full path, or null (logged) when it cannot be written.
        /// </summary>
        public static string? ExportReceipts(string? path = null)
        {
            var target = path ?? Path.Combine(Application.persistentDataPath, "scfg-receipts.json");
            var list = new List<object?>();
            foreach (var kv in SourceReceipts()) list.Add(DeepCopy.Of(kv.Value));
            try
            {
                var full = Path.GetFullPath(target);
                var dir = Path.GetDirectoryName(full);
                if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
                File.WriteAllText(full, Json.Dumps(list, sortKeys: true, indent: 2), new System.Text.UTF8Encoding(false));
                SyncWebFs();
                return full;
            }
            catch (Exception e) when (e is IOException || e is UnauthorizedAccessException || e is ArgumentException
                                      || e is NotSupportedException || e is System.Security.SecurityException)
            {
                Report("cannot write " + target + ": " + e.Message);
                return null;
            }
        }
    }
}
