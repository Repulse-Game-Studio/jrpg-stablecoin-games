// Copyright 2026 Repulse
// Twin of scfg_core/sessions.py (and of unity/.../Core/Sessions.cs, rpgmaker/src/core/sessions.ts).
// Pending payment sessions.
//
// A session is one payment attempt, created when the player picks a network. It is stored as a plain Json dict (the
// frozen `pending` schema, conformance/schema/state.json) so it survives restarts and the watcher can keep looking
// for the payment for up to 24 hours. A store is a Json dict of session id -> record; records are shared handles, so
// a record returned here is the stored one (Python returns the dict itself).
//
// Records read back from storage are loosely typed, as in Python: a record that is not a dict is the TypeError of
// Python's rec["..."], a missing field its KeyError, and a created_at that does no arithmetic the TypeError of `-`.
// Randomness comes from an Entropy (Python's `secrets`), drawn in Python's order.
#pragma once

#include "Scfg/Dec.h"
#include "Scfg/Entropy.h"
#include "Scfg/Export.h"
#include "Scfg/Json.h"
#include "Scfg/Result.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace scfg::sessions {

inline constexpr int64_t SESSION_WINDOW_S = 30 * 60;
inline constexpr int64_t SCAN_HORIZON_S = 24 * 3600;
inline constexpr int64_t HINT_TIMEOUT_S = 30 * 60;
// Twin of SOLANA_HINT_TIMEOUT_S in scfg_core/backends/solana.py: a Solana hint is given up on much sooner.
inline constexpr int64_t SOLANA_HINT_TIMEOUT_S = 180;
inline constexpr std::array<std::string_view, 4> UI_STATUSES = {"open", "pending", "cancelled", "expired"};

// "ps_" + token_hex(8).
SCFGCORE_API std::string new_session_id(Entropy& entropy);

// A 0x-prefixed 32-byte hash, ^0x[0-9a-fA-F]{64}$ after str.strip(), returned stripped and lowercased; anything else
// (a non-str included) is ValueError("not a transaction hash: <repr>").
SCFGCORE_API Result<std::string> normalize_tx_hash(const Json& tx_hash);

// create_session's keyword arguments. Every field has Python's meaning and is stored as given, except price (stored
// as str(parse_amount(price))). A nonce of None draws randbelow(100); extra (None or a dict) adds fields that must not
// name a reserved one.
struct NewSession {
    Json kind = Json();  // "unlock" | "tip", else ValueError
    Json item = Json();  // str | None
    Json title = Json();
    Json token = Json();
    Json chain = Json();
    std::variant<Dec, Json> price = Dec();  // a Decimal, or the str/int amounts::parse_amount takes
    Json code = Json();
    Json start_block = Json();  // also the first cursor
    Json now = Json();  // created_at: the clock's value (an int or a float)
    Json nonce = Json();  // None: secrets.randbelow(100)
    Json extra = Json();  // None or a dict
};

// Creates and stores the session; returns the stored record. Python's order: the kind check, the nonce draw,
// parse_amount(price), the id draw (token_hex(8)), encode(price, code, nonce), the reserved-field check
// (ValueError("session field <repr> is reserved")), then the record is stored. A falsy extra is Python's
// `extra or {}`; any other extra that is not a dict is a TypeError.
SCFGCORE_API Result<Json> create_session(Json::Dict& store, const NewSession& args, Entropy& entropy);

// store.get(sid): the record, or None.
SCFGCORE_API Json get_session(const Json::Dict& store, std::string_view sid);

// store.pop(sid, None).
SCFGCORE_API void remove_session(Json::Dict& store, std::string_view sid);

// sorted(store.values(), key=lambda r: r["created_at"]): oldest first, stable. Every record's created_at is read
// (a KeyError or TypeError even for one record). Two or more records must have keys Python can order: numbers
// (bool, int, float, compared exactly) among themselves or strs (by code point) among themselves, else TypeError.
// Port limits: a list key is refused (Python orders lists lexicographically), and a NaN sorts after every number
// (Python's order then depends on which pairs its sort compares).
SCFGCORE_API Result<std::vector<Json>> list_sessions(const Json::Dict& store);

// How a hint's tx id is checked and normalized (a backend's normalize_tx).
using Normalize = std::function<Result<std::string>(const Json&)>;

// rec = store[sid] (KeyError); rec["hint_tx"] = normalize(tx_hash); rec["hint_at"] = now. A record that is not a
// dict fails after normalize, with the TypeError of an item assignment.
SCFGCORE_API Status set_hint(Json::Dict& store, std::string_view sid, const Json& tx_hash, const Json& now,
                             const Normalize& normalize = normalize_tx_hash);

// rec["hint_tx"] = rec["hint_at"] = None.
SCFGCORE_API void clear_hint(Json::Dict& rec);

// The status must be one of UI_STATUSES (else ValueError("invalid ui status <repr>"), checked first); then
// store[sid]["ui_status"] = status (KeyError; TypeError for a record that is not a dict).
SCFGCORE_API Status set_ui_status(Json::Dict& store, std::string_view sid, const Json& status);

// now - rec["created_at"] >= SESSION_WINDOW_S / SCAN_HORIZON_S, with Python's arithmetic: two ints exactly, else in
// floats (an int too large for a float is an OverflowError; a NaN is never past).
SCFGCORE_API Result<bool> is_past_window(const Json& rec, const Json& now);
SCFGCORE_API Result<bool> is_past_horizon(const Json& rec, const Json& now);

// stored_amount(rec["price"]).
SCFGCORE_API Result<Dec> session_price(const Json& rec);

}  // namespace scfg::sessions
