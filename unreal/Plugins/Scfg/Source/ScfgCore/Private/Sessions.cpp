// Copyright 2026 Repulse
// Twin of scfg_core/sessions.py (see Sessions.h).
#include "Scfg/Sessions.h"

#include "Scfg/Amounts.h"
#include "Scfg/BigInt.h"
#include "Scfg/Err.h"
#include "Scfg/Py.h"

#include <algorithm>
#include <string>
#include <utility>

namespace scfg::sessions::detail {

// rec[key] = value on a stored record: the TypeError Python raises when the record is not a dict.
Status set_item(Json& rec, std::string key, Json value) {
    switch (rec.type()) {
        case Json::Type::Dict:
            rec.as_dict().set(std::move(key), std::move(value));
            return ok();
        case Json::Type::List:
            return Err::type_error("list indices must be integers or slices, not str");
        default:
            return Err::type_error("'" + std::string(rec.type_name()) + "' object does not support item assignment");
    }
}

// now - created >= limit, with Python's arithmetic: exact for two ints, else in floats (a NaN is never past).
Result<bool> elapsed_at_least(const Json& rec, const Json& now, int64_t limit) {
    SCFG_TRY(Json created, py::get(rec, "created_at"));
    SCFG_TRY(Json elapsed, py::sub(now, created));
    return py::compare(elapsed, py::Cmp::Ge, Json(limit));
}

}  // namespace scfg::sessions::detail

namespace scfg::sessions {

std::string new_session_id(Entropy& entropy) { return "ps_" + entropy.token_hex(8); }

Result<std::string> normalize_tx_hash(const Json& tx_hash) {
    if (tx_hash.is_str()) {
        const std::string s = py::strip(tx_hash.as_str());
        if (s.size() == 66 && s[0] == '0' && s[1] == 'x' && std::all_of(s.begin() + 2, s.end(), py::is_hex_digit)) {
            return py::lower_ascii(s);
        }
    }
    SCFG_TRY(std::string shown, py::repr(tx_hash));
    return Err::value_error("not a transaction hash: " + shown);
}

Result<Json> create_session(Json::Dict& store, const NewSession& a, Entropy& entropy) {
    if (!a.kind.is_str() || (a.kind.as_str() != "unlock" && a.kind.as_str() != "tip")) {
        SCFG_TRY(std::string shown, py::repr(a.kind));
        return Err::value_error("invalid kind " + shown);
    }
    Json nonce = a.nonce;
    if (nonce.is_null()) {
        SCFG_TRY(int64_t drawn, entropy.randbelow(100));
        nonce = Json(drawn);
    }
    Result<Dec> parsed = std::holds_alternative<Dec>(a.price) ? amounts::parse_amount(std::get<Dec>(a.price))
                                                               : amounts::parse_amount(std::get<Json>(a.price));
    SCFG_TRY(Dec p, std::move(parsed));
    std::string sid = new_session_id(entropy);
    SCFG_TRY(BigInt units, amounts::encode(p, a.code, nonce));
    Json rec = Json::dict({{"id", Json(sid)},
                           {"kind", a.kind},
                           {"item", a.item},
                           {"title", a.title},
                           {"token", a.token},
                           {"chain", a.chain},
                           {"price", Json(p.str())},
                           {"code", a.code},
                           {"nonce", nonce},
                           {"units", Json(units)},
                           {"start_block", a.start_block},
                           {"cursor", a.start_block},
                           {"created_at", a.now},
                           {"state", "awaiting"},
                           {"ui_status", "open"},
                           {"hint_tx", Json()},
                           {"hint_at", Json()},
                           {"match", Json()}});
    if (a.extra.is_dict()) {
        for (const auto& [key, value] : a.extra.as_dict()) {
            (void)value;
            if (rec.as_dict().contains(key)) return Err::value_error("session field " + py::repr_str(key) + " is reserved");
        }
        for (const auto& [key, value] : a.extra.as_dict()) rec.as_dict().set(key, value);
    } else if (py::truthy(a.extra)) {
        return Err::type_error("session extra fields must be a dict, not " + std::string(a.extra.type_name()));
    }
    store.set(sid, rec);
    return rec;
}

Json get_session(const Json::Dict& store, std::string_view sid) { return store.get(sid); }

void remove_session(Json::Dict& store, std::string_view sid) { store.erase(sid); }

Result<std::vector<Json>> list_sessions(const Json::Dict& store) {
    struct Entry {
        Json rec;
        Json key;
    };
    std::vector<Entry> entries;
    entries.reserve(store.size());
    for (const auto& [sid, rec] : store) {
        (void)sid;
        SCFG_TRY(Json key, py::get(rec, "created_at"));
        entries.push_back(Entry{rec, std::move(key)});
    }
    // Any sort of two or more records compares some pair across two kinds of key when there are two kinds, so a
    // mix is refused up front (Python raises from inside its sort); a lone record is never compared.
    auto kind = [](const Json& k) { return py::number(k) ? 1 : k.is_str() ? 2 : 0; };
    for (size_t i = 1; i < entries.size(); ++i) {
        const int x = kind(entries[i].key);
        const int y = kind(entries[i - 1].key);
        if (x == 0 || x != y) {
            return Err::type_error("'<' not supported between instances of '" +
                                   std::string(entries[i].key.type_name()) + "' and '" +
                                   std::string(entries[i - 1].key.type_name()) + "'");
        }
    }
    std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.key.is_str()) return py::compare_code_points(a.key.as_str(), b.key.as_str()) < 0;
        return py::number_order(*py::number(a.key), *py::number(b.key)) < 0;
    });
    std::vector<Json> out;
    out.reserve(entries.size());
    for (Entry& e : entries) out.push_back(std::move(e.rec));
    return out;
}

Status set_hint(Json::Dict& store, std::string_view sid, const Json& tx_hash, const Json& now,
                const Normalize& normalize) {
    const Json* found = store.find(sid);
    if (found == nullptr) return py::key_error(sid);
    Json rec = *found;  // the stored record itself (a shared handle)
    SCFG_TRY(std::string hint, normalize(tx_hash));
    SCFG_TRY_VOID(detail::set_item(rec, "hint_tx", Json(std::move(hint))));
    return detail::set_item(rec, "hint_at", now);
}

void clear_hint(Json::Dict& rec) {
    rec.set("hint_tx", Json());
    rec.set("hint_at", Json());
}

Status set_ui_status(Json::Dict& store, std::string_view sid, const Json& status) {
    const bool known = status.is_str() && std::find(UI_STATUSES.begin(), UI_STATUSES.end(),
                                                    std::string_view(status.as_str())) != UI_STATUSES.end();
    if (!known) {
        SCFG_TRY(std::string shown, py::repr(status));
        return Err::value_error("invalid ui status " + shown);
    }
    const Json* found = store.find(sid);
    if (found == nullptr) return py::key_error(sid);
    Json rec = *found;
    return detail::set_item(rec, "ui_status", status);
}

Result<bool> is_past_window(const Json& rec, const Json& now) {
    return detail::elapsed_at_least(rec, now, SESSION_WINDOW_S);
}

Result<bool> is_past_horizon(const Json& rec, const Json& now) {
    return detail::elapsed_at_least(rec, now, SCAN_HORIZON_S);
}

Result<Dec> session_price(const Json& rec) {
    SCFG_TRY(Json price, py::get(rec, "price"));
    return amounts::stored_amount(price);
}

}  // namespace scfg::sessions
