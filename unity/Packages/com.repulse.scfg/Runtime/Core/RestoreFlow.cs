// Twin of scfg_core/restore_flow.py: the state behind the restore screen, independent of any engine. Ported from
// rpgmaker/src/core/restore_flow.ts (and godot/addons/scfg/core/restore_flow.gd).
//
//   input -> checking -> (choose ->) saving -> done
//
// The screen shows the fields (State, TxInput, Error, Candidates, Choices, Added), reads Message() and CanConfirm, and
// calls Check, Choose and Confirm from its buttons and Poll() every frame. The Engine work goes through the SubmitFn
// (the Worker). A job that failed or was cancelled sends the screen back to input with the RestoreError's code as
// Error ("failed" for any other exception, a cancel included: Python's exception() would raise CancelledError). A job
// that succeeded with the wrong kind of value (not a list of candidates, not a list of added receipts) is "failed"
// too, where Python would have raised (as the GDScript twin, godot/addons/scfg/core/restore_flow.gd, does).
#nullable enable
using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Linq;

namespace Scfg.Core
{
    /// <summary>restore_flow.py's module table, read-only (Unity's domain reload off keeps statics between plays).</summary>
    public static class RestoreFlow
    {
        /// <summary>The English text of each restore error id (restore_flow.py's MESSAGES, in its order).</summary>
        public static readonly IReadOnlyDictionary<string, string> Messages = new ReadOnlyDictionary<string, string>(
            new Dictionary<string, string>(StringComparer.Ordinal)
            {
                { "invalid_hash", "That doesn't look like a transaction hash or signature." },
                { "tx_not_found", "Transaction not found. Check the hash, or try again later." },
                { "tx_reverted", "That transaction failed on-chain; nothing was paid." },
                { "tx_not_a_payment", "This transaction isn't a payment to this game." },
                { "network_unavailable", "Can't reach the network. Try again in a moment." },
                { "paste_blocked", "Your browser blocked pasting. Type the hash instead." },
                { "failed", "Something went wrong. Please try again." },
            });
    }

    public sealed class RestoreController
    {
        public SubmitFn Submit { get; }
        /// <summary>"input", "checking", "choose", "saving" or "done".</summary>
        public string State { get; private set; } = "input";
        /// <summary>What the player typed or pasted.</summary>
        public string TxInput { get; set; } = "";
        /// <summary>
        /// A key of <see cref="RestoreFlow.Messages"/>, an error id the screen has no text for, or null. The glue sets
        /// it too ("paste_blocked" when the browser refused a paste).
        /// </summary>
        public string? Error { get; set; }
        /// <summary>The transfers the lookup found.</summary>
        public List<RestoreCandidate> Candidates { get; private set; } = new List<RestoreCandidate>();
        /// <summary>A candidate's index to the player's choice (an item id or <see cref="Restore.TipChoice"/>).</summary>
        public Dictionary<int, string> Choices { get; } = new Dictionary<int, string>();
        /// <summary>The receipts the save added.</summary>
        public List<JsonObject> Added { get; private set; } = new List<JsonObject>();

        private Job? _future;
        private string? _phase;

        public RestoreController(SubmitFn submit)
        {
            Submit = submit ?? throw new TypeError("a RestoreController needs a submit function");
        }

        /// <summary>The text for <see cref="Error"/>, or null (no error, or a code with no text).</summary>
        public string? Message() =>
            !string.IsNullOrEmpty(Error) && RestoreFlow.Messages.TryGetValue(Error!, out var text) ? text : null;

        /// <summary>Indexes of the candidates that wait for the player's choice.</summary>
        public List<int> PendingChoices()
        {
            var output = new List<int>();
            for (int i = 0; i < Candidates.Count; i++)
                if (Candidates[i].Kind == null && !Candidates[i].AlreadyRestored) output.Add(i);
            return output;
        }

        /// <summary>
        /// Looks the text (default: <see cref="TxInput"/>) up. False, with Error "invalid_hash", when it is not a
        /// transaction id of any family.
        /// </summary>
        public bool Check(string? text = null)
        {
            string tx;
            try
            {
                tx = Backends.NormalizeAnyTx(text ?? TxInput);
            }
            catch (ValueError)
            {
                Error = "invalid_hash";
                return false;
            }
            Error = null;
            State = "checking";
            _phase = "lookup";
            _future = Submit(Commands.Of(e => e.RestoreLookup(tx)));
            return true;
        }

        public void Choose(int index, string choice) => Choices[index] = choice;

        /// <summary>Every candidate that needs the player's choice has one.</summary>
        public bool CanConfirm => PendingChoices().All(i => Choices.ContainsKey(i));

        public void Confirm()
        {
            if ((State != "checking" && State != "choose") || !CanConfirm) return;
            var cands = Candidates.Where(c => !c.AlreadyRestored).ToList();
            // Python's {self.candidates.index(c): i for i, c in enumerate(cands)}: index() finds the first equal one
            var indexMap = new Dictionary<int, int>();
            for (int i = 0; i < cands.Count; i++)
            {
                var c = cands[i];
                indexMap[Candidates.FindIndex(x => x.Equals(c))] = i;
            }
            var choices = new Dictionary<int, object?>();
            foreach (var kv in Choices)
                if (indexMap.TryGetValue(kv.Key, out var mapped)) choices[mapped] = kv.Value;
            State = "saving";
            _phase = "apply";
            _future = Submit(Commands.Sync(e => e.RestoreApply(cands, choices)));
        }

        /// <summary>Picks up the finished lookup or save (call every frame).</summary>
        public void Poll()
        {
            var fut = _future;
            if (fut == null || !fut.Done()) return;
            _future = null;
            var exc = fut.Exception();
            if (exc != null)
            {
                State = "input";
                Error = exc is RestoreError re ? re.Code : "failed";
                return;
            }
            var value = fut.Result();
            if (_phase == "lookup")
            {
                if (!(value is IEnumerable<RestoreCandidate> found) || found.Any(c => c == null))
                {
                    WrongValue();
                    return;
                }
                Candidates = found.ToList();
                if (Candidates.All(c => c.AlreadyRestored))
                    State = "done";
                else if (PendingChoices().Count > 0)
                    State = "choose";
                else
                    Confirm();
            }
            else
            {
                if (!(value is IEnumerable<JsonObject> added) || added.Any(r => r == null))
                {
                    WrongValue();
                    return;
                }
                Added = added.ToList();
                State = "done";
            }
        }

        private void WrongValue()
        {
            State = "input";
            Error = "failed";
        }

        /// <summary>Status "restored" / "nothing_found" once done, else "cancelled" (with the last error).</summary>
        public RestoreResult Result()
        {
            if (State == "done")
                return new RestoreResult(Added.Count > 0 ? "restored" : "nothing_found") { Restored = new List<JsonObject>(Added) };
            return new RestoreResult("cancelled") { Error = Error };
        }
    }
}
