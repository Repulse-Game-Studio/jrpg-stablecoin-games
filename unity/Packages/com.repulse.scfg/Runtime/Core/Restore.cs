// Twin of scfg_core/restore.py (and of rpgmaker/src/core/restore.ts): for now only the RestoreCandidate dataclass,
// which the Engine and the backends name. RestoreError is in Errors.cs. Task 12 ports classify, lookup and apply.
using System.Collections.Generic;

namespace Scfg.Core
{
    /// <summary>Python's <c>RestoreCandidate</c> dataclass: one transfer to the developer a restore found.</summary>
    public sealed class RestoreCandidate
    {
        public string Chain { get; set; }
        public string Token { get; set; }
        /// <summary>A <see cref="Scfg.Core.Transfer"/> (Solana) or a <see cref="TransferLog"/> (EVM).</summary>
        public object Transfer { get; set; }
        /// <summary>"unlock", "tip", or null when the player must choose.</summary>
        public string? Kind { get; set; }
        public string? Item { get; set; }
        /// <summary>Item ids (and "tip") offered when <see cref="Kind"/> is null.</summary>
        public IReadOnlyList<string> Choices { get; set; }
        public bool AlreadyRestored { get; set; }

        public RestoreCandidate(string chain, string token, object transfer, string? kind, string? item,
                                IReadOnlyList<string> choices, bool alreadyRestored)
        {
            if (!(transfer is Transfer) && !(transfer is TransferLog))
                throw new TypeError("a restore candidate's transfer is a Transfer or a TransferLog, not " + Py.TypeName(transfer));
            Chain = chain;
            Token = token;
            Transfer = transfer;
            Kind = kind;
            Item = item;
            Choices = choices;
            AlreadyRestored = alreadyRestored;
        }
    }
}
