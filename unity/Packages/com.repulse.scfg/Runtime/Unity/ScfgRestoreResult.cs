// What `await SCFG.Restore()` and a Restore callback give the game: the core's RestoreResult as a typed object
// (Godot's restore_result.gd).
#nullable enable
using System.Collections.Generic;
using Scfg.Core;

namespace Scfg
{
    /// <summary>How a restore ended (ScfgRestoreResult.Status).</summary>
    public enum ScfgRestoreStatus
    {
        /// <summary>Receipts were added (Restored lists them).</summary>
        Restored,
        /// <summary>The lookup finished, and everything it found was already on this device.</summary>
        NothingFound,
        /// <summary>The screen was closed before a restore finished (Error: the last lookup error, if any).</summary>
        Cancelled,
        /// <summary>A misused call (Error is the ConfigError code), or payments could not start.</summary>
        Failed,
    }

    /// <summary>What a restore found (see the file header).</summary>
    public sealed class ScfgRestoreResult
    {
        /// <summary>How the restore ended.</summary>
        public ScfgRestoreStatus Status { get; internal set; }
        /// <summary>The receipts it added to this device (empty unless Restored).</summary>
        public IReadOnlyList<ScfgReceipt> Restored { get; internal set; } = new List<ScfgReceipt>();
        /// <summary>The core's lookup error ("tx_not_found"), or a ConfigError code for a misused Restore.</summary>
        public string? Error { get; internal set; }

        internal ScfgRestoreResult(ScfgRestoreStatus status)
        {
            Status = status;
        }

        internal static ScfgRestoreResult FromCore(RestoreResult r) => new ScfgRestoreResult(
            r.Status == "restored" ? ScfgRestoreStatus.Restored
            : r.Status == "nothing_found" ? ScfgRestoreStatus.NothingFound
            : r.Status == "cancelled" ? ScfgRestoreStatus.Cancelled
            : ScfgRestoreStatus.Failed)
        {
            Restored = ScfgReceipt.ListOf(r.Restored),
            Error = r.Error,
        };

        public override string ToString() =>
            "ScfgRestoreResult(" + Status + ", restored=" + Restored.Count + ", error=" + (Error ?? "null") + ")";
    }
}
