// Copyright 2026 Repulse
// No Python twin: every player-facing text of the Unreal screens (the payment sheet, the restore screen, the
// notifications), in English, by id. The ids are RPG Maker's (rpgmaker/src/mz/texts.ts, "ID SCHEME"), the one id
// scheme the engines share:
//   - the checkout's messages keep the core's own keys, the ids of conformance/vectors/messages.json (what
//     CheckoutController's error holds: "invalid_hash", "tx_reverted", ...);
//   - the restore screen's messages take the prefix "restore_" ("restore_tx_reverted"), since some keys read
//     differently there (restore_flow's MESSAGES, Godot's restore_flow.gd);
//   - the notifications: "payment_confirmed_item", "payment_confirmed", "purchases_restored";
//   - everything else is named after what it says or where it sits ("cancel", "how_to_pay", "qr_step_scan", ...).
// Only "%s" takes arguments (in order; "%%" is a percent sign), numbers too.
//
// tools/check_text_ids.py --engine unreal reads the table in ScfgTexts.cpp and checks it: every messages.json id with
// the same English; every restore message of Godot's restore_flow.gd as "restore_<code>", word for word; every id
// the code asks for through ScfgTexts::Get(TEXT("...")) / ScfgTexts::Format(TEXT("..."), ...) (Private/UI/*.cpp and
// the facade) exists; every other text is, by value, a string of Godot's template (scfg.pot), so the engines show
// the same words; and, when Node is installed, every id is RPG Maker's id with the same English.
//
// English only for now (spec: "English only, keyed by messages.json ids"): no override layer yet. An id nobody
// defines shows as itself (RPG Maker's rule), which the checker makes unreachable.
#pragma once

#include "CoreMinimal.h"

namespace ScfgTexts
{

// Every text by id.
SCFG_API const TMap<FString, FString>& All();
SCFG_API bool Has(const FString& Id);
// The English of Id, else Id itself.
SCFG_API FString Get(const FString& Id);
// Get(Id) with Args in its "%s"s, in order ("%%" is "%"; a "%s" past the last argument stays as it is).
SCFG_API FString Format(const FString& Id, const TArray<FString>& Args);
// The text of a message id of the checkout (CheckoutController's error, a messages.json id), or "" for none.
SCFG_API FString Message(const FString& Id);
// The text of a restore screen's message code (RestoreController's error): "restore_<code>"; "" for none.
SCFG_API FString RestoreMessage(const FString& Code);

}  // namespace ScfgTexts
