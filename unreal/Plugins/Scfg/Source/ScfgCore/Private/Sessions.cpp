// Twin of scfg_core/sessions.py (see Sessions.h).
#include "Scfg/Sessions.h"

#include "Scfg/Amounts.h"
#include "Scfg/BigInt.h"
#include "Scfg/Err.h"
#include "Scfg/Py.h"
#include "Scfg/PyFloat.h"

#include <algorithm>
#include <cmath>
#include <compare>
#include <optional>
#include <string>
#include <utility>

namespace scfg::sessions::detail {

bool is_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

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

// A Python number: an int (bool included) exactly, or a float.
struct Num {
    bool is_int = false;
    BigInt i;
    double f = 0.0;
};

std::optional<Num> number(const Json& v) {
    if (v.is_bool()) return Num{true, BigInt(v.as_bool() ? 1 : 0), 0.0};
    if (v.is_int()) return Num{true, v.as_bigint(), 0.0};
    if (v.is_float()) return Num{false, BigInt(), v.as_float()};
    return std::nullopt;
}

// float(n) for an int, correctly rounded (as PyLong_AsDouble); OverflowError beyond the float range.
Result<double> to_float(const Num& n) {
    if (!n.is_int) return n.f;
    if (n.i.fits_int64()) return static_cast<double>(n.i.to_int64().value());  // round-to-nearest-even, as Python
    std::optional<double> f = pyfloat::parse(n.i.to_decimal());                // strtod rounds correctly too
    if (!f || std::isinf(*f)) return Err::overflow_error("int too large to convert to float");
    return *f;
}

// now - created >= limit, with Python's arithmetic: exact for two ints, else in floats.
Result<bool> elapsed_at_least(const Json& rec, const Json& now, int64_t limit) {
    SCFG_TRY(Json created, py::get(rec, "created_at"));
    std::optional<Num> a = number(now);
    std::optional<Num> b = number(created);
    if (!a || !b) {
        return Err::type_error("unsupported operand type(s) for -: '" + std::string(now.type_name()) + "' and '" +
                               std::string(created.type_name()) + "'");
    }
    if (a->is_int && b->is_int) return a->i - b->i >= BigInt(limit);
    SCFG_TRY(double x, to_float(*a));
    SCFG_TRY(double y, to_float(*b));
    return x - y >= static_cast<double>(limit);  // a NaN is never past
}

// Exact order of two numbers. Python's int/float comparisons are exact; a NaN is put after every number (Python's
// comparisons with it are all false).
std::weak_ordering compare_numbers(const Num& a, const Num& b) {
    if (a.is_int && b.is_int) return a.i <=> b.i;
    if (!a.is_int && !b.is_int) {
        if (std::isnan(a.f) || std::isnan(b.f)) return std::isnan(a.f) <=> std::isnan(b.f);
        return a.f < b.f ? std::weak_ordering::less : b.f < a.f ? std::weak_ordering::greater : std::weak_ordering::equivalent;
    }
    const bool flip = !a.is_int;  // compare (int i, float f) and flip the answer when a is the float
    const BigInt& i = a.is_int ? a.i : b.i;
    const double f = a.is_int ? b.f : a.f;
    std::weak_ordering r = std::weak_ordering::equivalent;
    if (std::isnan(f)) {
        r = std::weak_ordering::less;
    } else if (std::isinf(f)) {
        r = f > 0 ? std::weak_ordering::less : std::weak_ordering::greater;
    } else if (i.fits_int64() && i.to_int64().value() >= -(int64_t{1} << 53) &&
               i.to_int64().value() <= (int64_t{1} << 53)) {
        const double d = static_cast<double>(i.to_int64().value());  // exact below 2**53
        r = d < f ? std::weak_ordering::less : f < d ? std::weak_ordering::greater : std::weak_ordering::equivalent;
    } else {
        r = Dec::from_int(i) <=> *Dec::from_double(f);  // both exact
    }
    if (!flip) return r;
    return r == std::weak_ordering::less ? std::weak_ordering::greater
           : r == std::weak_ordering::greater ? std::weak_ordering::less
                                              : std::weak_ordering::equivalent;
}

}  // namespace scfg::sessions::detail

namespace scfg::sessions {

std::string new_session_id(Entropy& entropy) { return "ps_" + entropy.token_hex(8); }

Result<std::string> normalize_tx_hash(const Json& tx_hash) {
    if (tx_hash.is_str()) {
        const std::string s = py::strip(tx_hash.as_str());
        if (s.size() == 66 && s[0] == '0' && s[1] == 'x' && std::all_of(s.begin() + 2, s.end(), detail::is_hex)) {
            return py::lower_ascii(s);
        }
    }
    return Err::value_error("not a transaction hash: " + py::repr(tx_hash));
}

Result<Json> create_session(Json::Dict& store, const NewSession& a, Entropy& entropy) {
    if (!a.kind.is_str() || (a.kind.as_str() != "unlock" && a.kind.as_str() != "tip")) {
        return Err::value_error("invalid kind " + py::repr(a.kind));
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
    auto kind = [](const Json& k) { return detail::number(k) ? 1 : k.is_str() ? 2 : 0; };
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
        return detail::compare_numbers(*detail::number(a.key), *detail::number(b.key)) < 0;
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
    if (!known) return Err::value_error("invalid ui status " + py::repr(status));
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
