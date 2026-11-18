// Copyright 2026 Repulse
// No Python twin: the screens' English texts by id (see ScfgTexts.h).
#include "UI/ScfgTexts.h"

#include "ScfgString.h"

#include "Scfg/Checkout.h"
#include "Scfg/RestoreFlow.h"

namespace ScfgTextsDetail
{

struct FEntry
{
	const TCHAR* Id;
	const TCHAR* English;
};

// tools/check_text_ids.py reads the lines between the two markers: one {TEXT("id"), TEXT("English")}, entry per
// line, nothing else on it (a line comment is fine).
static const FEntry GEntries[] = {
	// SCFG-TEXTS-BEGIN
	// the checkout's messages (conformance/vectors/messages.json, CheckoutController's MESSAGES)
	{TEXT("browser_unavailable"), TEXT("Couldn't start the browser checkout. Use another method.")},
	{TEXT("internal_error"), TEXT("Something went wrong while checking the payment. Retrying...")},
	{TEXT("invalid_amount"), TEXT("Enter an amount of at least 0.50 with up to 2 decimals.")},
	{TEXT("invalid_hash"), TEXT("That doesn't look like a transaction hash or signature.")},
	{TEXT("network_unavailable"), TEXT("Can't reach the network. Retrying...")},
	{TEXT("paste_blocked"), TEXT("Your browser blocked pasting. Type it instead: the game also finds the payment without it.")},
	{TEXT("solana_unavailable"), TEXT("Payments on Solana aren't available right now.")},
	{TEXT("start_failed"), TEXT("Couldn't start the payment. Check your connection and try again.")},
	{TEXT("tx_not_a_payment"), TEXT("That transaction isn't a payment for this purchase.")},
	{TEXT("tx_not_landed"), TEXT("That transaction didn't reach the network. You can pay again.")},
	{TEXT("tx_reverted"), TEXT("That transaction failed on-chain; no funds were sent.")},
	// the restore screen's messages (restore_flow's MESSAGES, prefixed)
	{TEXT("restore_invalid_hash"), TEXT("That doesn't look like a transaction hash or signature.")},
	{TEXT("restore_tx_not_found"), TEXT("Transaction not found. Check the hash, or try again later.")},
	{TEXT("restore_tx_reverted"), TEXT("That transaction failed on-chain; nothing was paid.")},
	{TEXT("restore_tx_not_a_payment"), TEXT("This transaction isn't a payment to this game.")},
	{TEXT("restore_network_unavailable"), TEXT("Can't reach the network. Try again in a moment.")},
	{TEXT("restore_paste_blocked"), TEXT("Your browser blocked pasting. Type the hash instead.")},
	{TEXT("restore_failed"), TEXT("Something went wrong. Please try again.")},
	// the notifications
	{TEXT("payment_confirmed_item"), TEXT("Payment confirmed: %s. Thank you!")},
	{TEXT("payment_confirmed"), TEXT("Payment confirmed. Thank you for your support!")},
	{TEXT("purchases_restored"), TEXT("Purchases restored.")},
	// amount and item
	{TEXT("choose_amount"), TEXT("Choose an amount")},
	{TEXT("or_enter_your_own"), TEXT("Or enter your own")},
	{TEXT("amount"), TEXT("Amount")},
	{TEXT("tip"), TEXT("Tip")},
	{TEXT("summary_line"), TEXT("%s: %s %s on %s")},
	// choosing how to pay
	{TEXT("how_to_pay"), TEXT("How do you want to pay?")},
	{TEXT("which_network"), TEXT("Which network holds your %s?")},
	{TEXT("fees_lowest"), TEXT("lowest fees")},
	{TEXT("fees_low"), TEXT("low fees")},
	{TEXT("fees_high"), TEXT("high fees")},
	{TEXT("phone_wallet"), TEXT("Phone wallet")},
	{TEXT("scan_with_phone_wallet"), TEXT("Scan a code with your phone wallet")},
	{TEXT("browser_wallet"), TEXT("Browser wallet")},
	{TEXT("manual_transfer"), TEXT("Manual transfer")},
	{TEXT("send_from_exchange"), TEXT("Send from an exchange or any wallet")},
	{TEXT("evm_extension_sub"), TEXT("MetaMask, Rabby or another wallet extension")},
	{TEXT("solana_extension_sub"), TEXT("Phantom, Solflare or another wallet extension")},
	{TEXT("evm_opens_browser_note"), TEXT("Opens a checkout page in your browser for MetaMask, Rabby, Coinbase Wallet and other wallet extensions.")},
	{TEXT("solana_opens_browser_note"), TEXT("Opens a checkout page in your browser for Phantom, Solflare, Backpack and other wallet extensions.")},
	{TEXT("onramp_link"), TEXT("No %s yet? Buy some with a card")},
	{TEXT("onramp_hint"), TEXT("No %s yet? Pick another network to buy some with a card")},
	// paying
	{TEXT("preparing_payment"), TEXT("Preparing the payment...")},
	{TEXT("qr_step_open"), TEXT("1. Open your wallet app")},
	{TEXT("qr_step_scan"), TEXT("2. Scan the code")},
	{TEXT("qr_step_approve"), TEXT("3. Approve %s")},
	{TEXT("open_wallet_app"), TEXT("Open wallet app")},
	{TEXT("approve_in_wallet"), TEXT("Approve %s on %s in your wallet.")},
	{TEXT("wallet_will_ask"), TEXT("Your wallet will ask for %s. The last digits identify your payment and add less than 1 cent.")},
	{TEXT("wallet_changes_amount"), TEXT("If your wallet changes the amount, the game still finds the payment.")},
	{TEXT("wallet_changes_amount_manual"), TEXT("If your wallet changes the amount, use Manual transfer and paste the transaction hash.")},
	{TEXT("open_checkout_browser"), TEXT("Open checkout in browser")},
	{TEXT("reopen_checkout_page"), TEXT("Reopen checkout page")},
	{TEXT("finish_in_browser"), TEXT("Finish the payment on the checkout page in your browser.")},
	{TEXT("to_address"), TEXT("To address")},
	{TEXT("token_contract"), TEXT("Token contract")},
	{TEXT("token_mint"), TEXT("Token mint")},
	{TEXT("send_exact_amount"), TEXT("Send exactly this amount on %s only. Sending on another network may lose your funds.")},
	{TEXT("send_exact_amount_solana"), TEXT("Send exactly this amount on Solana (SPL) only. Withdrawing on another network may lose your funds.")},
	{TEXT("copy"), TEXT("Copy")},
	{TEXT("copy_hash"), TEXT("Copy hash")},
	{TEXT("copy_signature"), TEXT("Copy signature")},
	{TEXT("i_sent_it"), TEXT("I've sent it")},
	{TEXT("sent_from_exchange_paste"), TEXT("Sent from an exchange? Paste the %s:")},
	{TEXT("paste_hash_help"), TEXT("Paste the transaction hash or signature of your payment. You can find it in your wallet's activity, or in your exchange's withdrawal history.")},
	{TEXT("your_hash"), TEXT("Your %s:")},
	{TEXT("word_tx_hash"), TEXT("transaction hash")},
	{TEXT("word_signature"), TEXT("signature")},
	{TEXT("check"), TEXT("Check")},
	// waiting and result
	{TEXT("waiting_for_payment"), TEXT("Waiting for your payment on %s...")},
	{TEXT("tx_received_waiting"), TEXT("Transaction received. Waiting for it to be included on %s...")},
	{TEXT("confirming"), TEXT("Confirming on %s... %s/%s")},
	{TEXT("checking_blockchain"), TEXT("Checking the blockchain...")},
	{TEXT("already_paid_note"), TEXT("If you already paid, the game will keep checking for 24 hours and unlock it automatically.")},
	{TEXT("keep_playing_note"), TEXT("You can keep playing: the game keeps checking in the background and finishes the purchase by itself.")},
	{TEXT("continue_playing"), TEXT("Continue playing")},
	{TEXT("keep_as_tip"), TEXT("Keep as a tip")},
	{TEXT("paid_title"), TEXT("Payment confirmed. Thank you!")},
	{TEXT("payment_too_small"), TEXT("Payment too small")},
	{TEXT("underpaid_note"), TEXT("The amount received was less than the price. Please contact the developer.")},
	{TEXT("payment_window_closed"), TEXT("The payment window has closed")},
	{TEXT("payment_screen_inactive"), TEXT("This payment screen is no longer active. Any payment you made is still being checked in the background.")},
	{TEXT("view_on_explorer"), TEXT("View on explorer")},
	// restore
	{TEXT("restore"), TEXT("Restore")},
	{TEXT("restore_purchases"), TEXT("Restore purchases")},
	{TEXT("restored"), TEXT("Restored:")},
	{TEXT("purchases_on_device"), TEXT("Purchases on this device")},
	{TEXT("keep_this_to_restore"), TEXT("Keep this %s to restore your purchase on another device:")},
	{TEXT("nothing_to_restore"), TEXT("Nothing new to restore. These purchases are already on this device.")},
	{TEXT("unmatched_payment_prompt"), TEXT("Payment of %s %s on %s. What was it for?")},
	{TEXT("screen_inactive"), TEXT("This screen is no longer active.")},
	{TEXT("saving"), TEXT("Saving...")},
	{TEXT("set"), TEXT("Set")},
	// developer lines (not in Shipping; Godot does not translate them, RPG Maker keys them)
	{TEXT("dev_mode_paying_to"), TEXT("DEV MODE: paying to %s")},
	{TEXT("dev_mode_paying_to_testnet"), TEXT("DEV MODE: paying to %s (testnet)")},
	// navigation
	{TEXT("back"), TEXT("Back")},
	{TEXT("next"), TEXT("Next")},
	{TEXT("continue"), TEXT("Continue")},
	{TEXT("cancel"), TEXT("Cancel")},
	{TEXT("close"), TEXT("Close")},
	// SCFG-TEXTS-END
};

static TMap<FString, FString> Build()
{
	TMap<FString, FString> Out;
	for (const FEntry& E : GEntries)
	{
		check(!Out.Contains(E.Id));
		Out.Add(E.Id, E.English);
	}
	return Out;
}

}  // namespace ScfgTextsDetail

namespace ScfgTexts
{

const TMap<FString, FString>& All()
{
	static const TMap<FString, FString> Texts = ScfgTextsDetail::Build();
	return Texts;
}

bool Has(const FString& Id)
{
	return All().Contains(Id);
}

FString Get(const FString& Id)
{
	const FString* Found = All().Find(Id);
	return Found ? *Found : Id;
}

FString Format(const FString& Id, const TArray<FString>& Args)
{
	const FString Text = Get(Id);
	FString Out;
	Out.Reserve(Text.Len() + 16);
	int32 Next = 0;
	for (int32 I = 0; I < Text.Len(); ++I)
	{
		if (Text[I] == TEXT('%') && I + 1 < Text.Len())
		{
			const TCHAR C = Text[I + 1];
			if (C == TEXT('%'))
			{
				Out.AppendChar(TEXT('%'));
				++I;
				continue;
			}
			if (C == TEXT('s') && Next < Args.Num())
			{
				Out += Args[Next++];
				++I;
				continue;
			}
		}
		Out.AppendChar(Text[I]);
	}
	return Out;
}

FString Message(const FString& Id)
{
	if (Id.IsEmpty() || scfg::checkout::MESSAGES().find(ScfgString::ToUtf8(Id)) == nullptr)
	{
		return FString();
	}
	return Get(Id);
}

FString RestoreMessage(const FString& Code)
{
	if (Code.IsEmpty() || scfg::restore_flow::MESSAGES().find(ScfgString::ToUtf8(Code)) == nullptr)
	{
		return FString();
	}
	return Get(TEXT("restore_") + Code);
}

}  // namespace ScfgTexts
