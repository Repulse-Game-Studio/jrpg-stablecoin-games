// The player-facing English of the payment sheet, the restore screen and the notices, in one table keyed by id. The ids
// and words are RPG Maker's (rpgmaker/src/mz/texts.ts, ID SCHEME there): the checkout messages keep the core's keys
// (conformance/vectors/messages.json, Checkout.Messages), the restore screen's take the prefix "restore_"
// (RestoreFlow.Messages), and the sheet's own strings are named after what they say. English only: there is no
// translation hook in v1. tools/check_text_ids.py checks this table against RPG Maker's by id and the ids the screens ask
// for (Get("..."), Format("...")).
#nullable enable
using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Text;
using Scfg.Core;

namespace Scfg
{
    public static class ScfgUiTexts
    {
        /// <summary>The sheet's own strings and the notices (RPG Maker's SHEET and NOTICES).</summary>
        private static readonly IReadOnlyDictionary<string, string> Own = new ReadOnlyDictionary<string, string>(
            new Dictionary<string, string>(StringComparer.Ordinal)
            {
                // amount and item
                { "choose_amount", "Choose an amount" },
                { "or_enter_your_own", "Or enter your own" },
                { "amount", "Amount" },
                { "tip", "Tip" },
                { "summary_line", "%s: %s %s on %s" },
                // choosing how to pay
                { "how_to_pay", "How do you want to pay?" },
                { "which_network", "Which network holds your %s?" },
                { "fees_lowest", "lowest fees" },
                { "fees_low", "low fees" },
                { "fees_high", "high fees" },
                { "phone_wallet", "Phone wallet" },
                { "scan_with_phone_wallet", "Scan a code with your phone wallet" },
                { "browser_wallet", "Browser wallet" },
                { "manual_transfer", "Manual transfer" },
                { "send_from_exchange", "Send from an exchange or any wallet" },
                { "evm_extension_sub", "MetaMask, Rabby or another wallet extension" },
                { "evm_browser_sub", "MetaMask, Rabby or another wallet in this browser" },
                { "solana_extension_sub", "Phantom, Solflare or another wallet extension" },
                { "solana_browser_sub", "Phantom, Solflare or another wallet in this browser" },
                { "evm_opens_browser_note", "Opens a checkout page in your browser for MetaMask, Rabby, Coinbase Wallet and other wallet extensions." },
                { "solana_opens_browser_note", "Opens a checkout page in your browser for Phantom, Solflare, Backpack and other wallet extensions." },
                { "evm_opens_here_note", "Opens your wallet right here, over the game: MetaMask, Rabby, Coinbase Wallet and other wallet extensions." },
                { "solana_opens_here_note", "Opens your wallet right here, over the game: Phantom, Solflare, Backpack and other wallet extensions." },
                // paying
                { "preparing_payment", "Preparing the payment..." },
                { "qr_step_open", "1. Open your wallet app" },
                { "qr_step_scan", "2. Scan the code" },
                { "qr_step_approve", "3. Approve %s" },
                { "open_wallet_app", "Open wallet app" },
                { "open_wallet", "Open wallet" },
                { "approve_in_wallet", "Approve %s on %s in your wallet." },
                { "wallet_will_ask", "Your wallet will ask for %s. The last digits identify your payment and add less than 1 cent." },
                { "wallet_changes_amount", "If your wallet changes the amount, the game still finds the payment." },
                { "wallet_changes_amount_manual", "If your wallet changes the amount, use Manual transfer and paste the transaction hash." },
                { "open_checkout_browser", "Open checkout in browser" },
                { "reopen_checkout_page", "Reopen checkout page" },
                { "reopen_wallet_window", "Reopen wallet window" },
                { "finish_in_browser", "Finish the payment on the checkout page in your browser." },
                { "finish_in_wallet_window", "Finish the payment in the wallet window." },
                { "to_address", "To address" },
                { "token_contract", "Token contract" },
                { "token_mint", "Token mint" },
                { "send_exact_amount", "Send exactly this amount on %s only. Sending on another network may lose your funds." },
                { "send_exact_amount_solana", "Send exactly this amount on Solana (SPL) only. Withdrawing on another network may lose your funds." },
                { "copy", "Copy" },
                { "copy_hash", "Copy hash" },
                { "copy_signature", "Copy signature" },
                { "paste", "Paste" },
                { "i_sent_it", "I've sent it" },
                { "sent_from_exchange_paste", "Sent from an exchange? Paste the %s:" },
                { "paste_hash_help", "Paste the transaction hash or signature of your payment. You can find it in your wallet's activity, or in your exchange's withdrawal history." },
                { "your_hash", "Your %s:" },
                { "word_tx_hash", "transaction hash" },
                { "word_signature", "signature" },
                { "check", "Check" },
                // waiting and result
                { "waiting_for_payment", "Waiting for your payment on %s..." },
                { "tx_received_waiting", "Transaction received. Waiting for it to be included on %s..." },
                { "confirming", "Confirming on %s... %s/%s" },
                { "checking_blockchain", "Checking the blockchain..." },
                { "already_paid_note", "If you already paid, the game will keep checking for 24 hours and unlock it automatically." },
                { "keep_playing_note", "You can keep playing: the game keeps checking in the background and finishes the purchase by itself." },
                { "continue_playing", "Continue playing" },
                { "keep_as_tip", "Keep as a tip" },
                { "paid_title", "Payment confirmed. Thank you!" },
                { "payment_too_small", "Payment too small" },
                { "underpaid_note", "The amount received was less than the price. Please contact the developer." },
                { "payment_window_closed", "The payment window has closed" },
                { "payment_screen_inactive", "This payment screen is no longer active. Any payment you made is still being checked in the background." },
                { "view_on_explorer", "View on explorer" },
                // restore
                { "restore", "Restore" },
                { "restore_purchases", "Restore purchases" },
                { "restored", "Restored:" },
                { "purchases_on_device", "Purchases on this device" },
                { "keep_this_to_restore", "Keep this %s to restore your purchase on another device:" },
                { "nothing_to_restore", "Nothing new to restore. These purchases are already on this device." },
                { "unmatched_payment_prompt", "Payment of %s %s on %s. What was it for?" },
                { "screen_inactive", "This screen is no longer active." },
                { "saving", "Saving..." },
                { "set", "Set" },
                // developer lines (test play only)
                { "dev_mode_paying_to", "DEV MODE: paying to %s" },
                { "dev_mode_paying_to_testnet", "DEV MODE: paying to %s (testnet)" },
                { "dev_solana_cant_receive", "Your Solana wallet can't receive %s yet: receive any amount once." },
                // accessibility
                { "qr_code", "QR code" },
                // navigation
                { "back", "Back" },
                { "next", "Next" },
                { "continue", "Continue" },
                { "cancel", "Cancel" },
                { "close", "Close" },
                // notices
                { "payment_confirmed_item", "Payment confirmed: %s. Thank you!" },
                { "payment_confirmed", "Payment confirmed. Thank you for your support!" },
                { "purchases_restored", "Purchases restored." },
                { "copy_failed", "Couldn't copy: your browser blocked the clipboard." },
            });

        /// <summary>Every text by id: the table above, the checkout messages and the restore messages ("restore_" + code).</summary>
        public static readonly IReadOnlyDictionary<string, string> All = Build();

        private static IReadOnlyDictionary<string, string> Build()
        {
            var all = new Dictionary<string, string>(StringComparer.Ordinal);
            foreach (var kv in Own) all.Add(kv.Key, kv.Value);
            foreach (var kv in Checkout.Messages) all.Add(kv.Key, kv.Value);
            foreach (var kv in RestoreFlow.Messages) all.Add("restore_" + kv.Key, kv.Value);
            return new ReadOnlyDictionary<string, string>(all);
        }

        /// <summary>The text <paramref name="id"/>; an unknown id shows as itself (and is logged in development builds).</summary>
        public static string Get(string id)
        {
            if (All.TryGetValue(id, out var text)) return text;
            if (UnityEngine.Debug.isDebugBuild) UnityEngine.Debug.LogError("scfg: unknown text id \"" + id + "\"");
            return id;
        }

        /// <summary>The text with <paramref name="args"/> in its %s in order (%% is a percent sign).</summary>
        public static string Format(string id, params object?[] args) => Apply(Get(id), args);

        /// <summary>Python's <c>text % args</c> for %s and %% only.</summary>
        public static string Apply(string text, params object?[] args)
        {
            var output = new StringBuilder();
            int next = 0;
            for (int i = 0; i < text.Length; i++)
            {
                if (text[i] == '%' && i + 1 < text.Length && (text[i + 1] == 's' || text[i + 1] == '%'))
                {
                    if (text[i + 1] == '%') output.Append('%');
                    else if (next < args.Length) output.Append(Convert.ToString(args[next++], System.Globalization.CultureInfo.InvariantCulture));
                    else output.Append("%s");
                    i++;
                }
                else
                {
                    output.Append(text[i]);
                }
            }
            return output.ToString();
        }
    }
}
