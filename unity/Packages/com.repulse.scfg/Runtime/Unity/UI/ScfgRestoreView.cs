// The restore screen, the UI Toolkit twin of Godot's ui/restore.gd (and of the scfg_restore screen in
// plugin/game/scfg/scfg_screens.rpy), shown by ScfgDefaultSheetHost for SCFG.Restore.
//
// By the RestoreController's state: input / checking (the hash field and Check), choose (each ambiguous payment with its
// choices, then Restore), saving, done (what was restored); then any message and the purchases on this device, each
// with Copy hash. Close is pinned at the bottom and closes with "close"; so does cancel (Escape, gamepad B).
#nullable enable
using System;
using System.Globalization;
using System.Linq;
using System.Text;
using Scfg.Core;
using UnityEngine.UIElements;

namespace Scfg
{
    public sealed class ScfgRestoreView : ScfgScreenView
    {
        public ScfgRestoreController Controller { get; }
        private RestoreController Ctl => Controller.Core;
        private string? _synced;

        public ScfgRestoreView(ScfgRestoreController controller)
        {
            Controller = controller ?? throw new ArgumentNullException(nameof(controller));
            Root.AddToClassList("scfg-screen--restore");
        }

        protected override bool ReadyToShow() => true;

        protected override string Signature()
        {
            var c = Ctl;
            var sb = new StringBuilder();
            void Add(object? v) => sb.Append(v?.ToString() ?? "∅").Append('\u001f');
            Add(c.State); Add(c.Error); Add(c.Candidates.Count);
            Add(string.Join(",", c.Choices.OrderBy(kv => kv.Key).Select(kv => kv.Key.ToString(CultureInfo.InvariantCulture) + "=" + kv.Value)));
            Add(c.CanConfirm); Add(c.Added.Count);
            // the purchases on this device change only with ReceiptsChanged: its revision, not a copy each frame
            Add(Controller.ReceiptsRevision);
            return sb.ToString();
        }

        protected override bool Moved(string? before, string after) =>
            before == null || before.Substring(0, before.IndexOf('\u001f')) != after.Substring(0, after.IndexOf('\u001f'));

        protected override string Primary()
        {
            var c = Ctl;
            switch (c.State)
            {
                case "input":
                case "checking":
                    return "tx_input";
                case "choose":
                    foreach (var i in c.PendingChoices())
                        if (!c.Choices.ContainsKey(i)) return "choice_" + Int(i) + "_" + c.Candidates[i].Choices[0];
                    return "restore";
            }
            return "close";
        }

        protected override void SyncFields()
        {
            if (Control("tx_input") is TextField f && _synced != Ctl.TxInput)
            {
                f.SetValueWithoutNotify(Ctl.TxInput);
                _synced = Ctl.TxInput;
            }
        }

        protected override void OnCancel() => CloseWith("close");

        protected override void Build()
        {
            var c = Ctl;
            _synced = null;
            var body = ScrollingBody(Layout);
            var mark = Logo();
            if (mark != null) body.Add(mark);
            body.Add(Reg(Text(ScfgUiTexts.Get("restore_purchases"), "scfg-title"), "title"));
            switch (c.State)
            {
                case "closed":
                    body.Add(Text(ScfgUiTexts.Get("screen_inactive")));
                    break;
                case "input":
                case "checking":
                    InputPart(body);
                    break;
                case "choose":
                    ChoosePart(body);
                    break;
                case "saving":
                    body.Add(StatusRow(ScfgUiTexts.Get("saving"), "accent", "status"));
                    break;
                case "done":
                    if (c.Added.Count > 0)
                    {
                        body.Add(Text(ScfgUiTexts.Get("restored"), "scfg-ok"));
                        int n = 0;
                        foreach (var r in ScfgReceipt.ListOf(c.Added)) body.Add(Reg(Text(ReceiptLine(r)), "added_" + Int(n++)));
                    }
                    else
                    {
                        body.Add(Reg(Text(ScfgUiTexts.Get("nothing_to_restore")), "nothing"));
                    }
                    break;
            }
            var msg = c.Message();
            if (Py.Truthy(msg)) body.Add(Reg(Text(msg!, "scfg-error"), "message"));
            var owned = Controller.DeviceReceipts();
            if (owned.Count > 0)
            {
                body.Add(Text(ScfgUiTexts.Get("purchases_on_device"), "scfg-label"));
                for (int n = 0; n < owned.Count; n++)
                {
                    var r = owned[n];
                    var row = Row();
                    var line = Text(ReceiptLine(r));
                    line.AddToClassList("scfg-grow");
                    row.Add(Reg(line, "owned_" + Int(n)));
                    var tx = r.TxHash ?? "";
                    row.Add(Button("copy_" + Int(n), ScfgUiTexts.Get("copy_hash"), "scfg-button--small", () => Controller.Copy(tx)));
                    body.Add(row);
                }
            }
            var footer = Box("scfg-footer");
            footer.Add(Rule());
            footer.Add(Button("close", ScfgUiTexts.Get("close"), "", () => CloseWith("close")));
            Layout.Add(footer);
        }

        private void InputPart(VisualElement body)
        {
            var c = Ctl;
            body.Add(Text(ScfgUiTexts.Get("paste_hash_help"), "scfg-muted"));
            var row = Row();
            var f = Field("tx_input", c.TxInput, SCFG.TxMaxLength, SCFG.TxChars, t => { Ctl.TxInput = t; _synced = t; }, Check, "tx");
            f.AddToClassList("scfg-grow");
            f.isReadOnly = c.State != "input";
            row.Add(f);
            _synced = c.TxInput;
            if (Controller.IsWeb)
            {
                var paste = Button("paste", ScfgUiTexts.Get("paste"), "scfg-button--small", () => Controller.Paste(), "tx");
                paste.SetEnabled(c.State == "input");
                row.Add(paste);
            }
            body.Add(row);
            var check = Button("check", ScfgUiTexts.Get("check"), "scfg-button--accent", Check);
            check.SetEnabled(c.State == "input");
            body.Add(check);
            if (c.State == "checking") body.Add(StatusRow(ScfgUiTexts.Get("checking_blockchain"), "accent", "status"));
        }

        private void ChoosePart(VisualElement body)
        {
            var c = Ctl;
            foreach (var i in c.PendingChoices())
            {
                var cand = c.Candidates[i];
                body.Add(Reg(Text(CandidateLine(cand)), "candidate_" + Int(i)));
                var grid = Row("scfg-grid");
                for (int j = 0; j < cand.Choices.Count; j++)
                {
                    var choice = cand.Choices[j];
                    int index = i;
                    var option = Option("choice_" + Int(i) + "_" + choice, ChoiceLabel(choice), "",
                        c.Choices.TryGetValue(i, out var picked) && picked == choice, () => Ctl.Choose(index, choice),
                        "choices_" + Int(i) + ":" + Int(j / 2));
                    option.AddToClassList("scfg-option--half");
                    grid.Add(option);
                }
                body.Add(grid);
            }
            var restore = Button("restore", ScfgUiTexts.Get("restore"), "scfg-button--primary", () => Ctl.Confirm());
            restore.SetEnabled(c.CanConfirm);
            body.Add(restore);
        }

        private void Check()
        {
            if (Ctl.State == "input") Ctl.Check();
        }

        private string ReceiptLine(ScfgReceipt r)
        {
            var what = r.Kind == ScfgPayKind.Unlock ? Controller.ItemTitle(r.Item) : ScfgUiTexts.Get("tip");
            return ScfgUiTexts.Format("summary_line", what, r.Amount, r.Token, Controller.ChainName(r.Chain));
        }

        private string CandidateLine(RestoreCandidate c)
        {
            string amount;
            try
            {
                var value = c.Transfer is Transfer t ? t.Value : ((TransferLog)c.Transfer).Value;
                amount = Amounts.FormatUnits(value);
            }
            catch (Exception e)
            {
                SCFG.ReportError("cannot format a restore candidate's amount:", e);
                amount = "?";
            }
            return ScfgUiTexts.Format("unmatched_payment_prompt", amount, c.Token, Controller.ChainName(c.Chain));
        }

        private string ChoiceLabel(string choice) =>
            choice == Scfg.Core.Restore.TipChoice ? ScfgUiTexts.Get("keep_as_tip") : Controller.ItemTitle(choice);
    }
}
