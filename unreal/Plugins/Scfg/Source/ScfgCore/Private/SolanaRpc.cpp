// Twin of scfg_core/solana/rpc.py: SolanaRpc and its _named_version.
#include "Scfg/SolanaRpc.h"

#include "Scfg/Ints.h"
#include "Scfg/Py.h"

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>
#include <vector>

namespace scfg::solana::rpc::detail {

// The code points re.IGNORECASE lets each letter of "version" match in CPython 3.9 (checked over every code point,
// as Rpc.cs's VersionLetters): s also matches U+017F (long s), i also U+0130 and U+0131. No other letter folds.
bool version_letter(size_t k, uint32_t cp) {
    constexpr std::array<char, 7> lower = {'v', 'e', 'r', 's', 'i', 'o', 'n'};
    char c = lower[k];
    if (cp == static_cast<uint32_t>(c) || cp == static_cast<uint32_t>(c - 'a' + 'A')) return true;
    if (c == 's') return cp == 0x17F;
    if (c == 'i') return cp == 0x130 || cp == 0x131;
    return false;
}

// The digits group of the leftmost `version \((\d+)\)` match, IGNORECASE, or nullopt. Backtracking into \d+ cannot
// help (a shorter run is followed by a digit, not ")"), so each start is tried once; and the digit run after one
// matched "version (" holds no other start, so the scan stays linear.
std::optional<std::string_view> find_named_version(std::string_view m) {
    std::vector<uint32_t> cps;
    std::vector<size_t> at;  // at[k]: byte offset of cps[k]; at[size] = m.size()
    for (size_t i = 0; i < m.size();) {
        at.push_back(i);
        cps.push_back(py::utf8_decode(m, i));
    }
    at.push_back(m.size());
    size_t n = cps.size();
    for (size_t i = 0; i + 9 <= n; ++i) {
        size_t k = 0;
        while (k < 7 && version_letter(k, cps[i + k])) ++k;
        if (k < 7 || cps[i + 7] != ' ' || cps[i + 8] != '(') continue;
        size_t j = i + 9;
        while (j < n && py::is_decimal_digit(cps[j])) ++j;
        if (j > i + 9 && j < n && cps[j] == ')') return m.substr(at[i + 9], at[j] - at[i + 9]);
    }
    return std::nullopt;
}

}  // namespace scfg::solana::rpc::detail

namespace scfg::solana::rpc {

using ::scfg::rpc::Attempt;

std::optional<int64_t> named_version(const Err& error, int64_t tx_version) {
    if (error.rpc_code != UNSUPPORTED_VERSION) return std::nullopt;
    std::optional<std::string_view> run = detail::find_named_version(error.message);
    if (!run) return std::nullopt;
    // A run of \d holds decimal digits only, so digit_run_value has nothing to refuse; nullopt: over 18 digits.
    Result<std::optional<int64_t>> version = ints::digit_run_value(*run);
    if (!version.ok() || !version.value() || *version.value() <= tx_version) return std::nullopt;
    return *version.value();
}

Json SolanaRpc::tx_params(const Json& sig, const std::string& commitment, std::optional<int64_t> version) const {
    return Json::list({sig, Json::dict({{"encoding", "json"},
                                        {"commitment", commitment},
                                        {"maxSupportedTransactionVersion", version ? *version : tx_version}})});
}

// Older nodes reject a maxSupportedTransactionVersion they don't know (-32602).
bool SolanaRpc::lower_version(const Err& error) {
    if (tx_version > 0 && error.rpc_code == -32602 &&
        py::lower_for_ascii(error.message).find("version") != std::string::npos) {
        tx_version -= 1;
        return true;
    }
    return false;
}

Co<Json> SolanaRpc::call(std::string method, Json params) {
    co_return co_await client_->call(std::move(method), std::move(params));
}

Co<BigInt> SolanaRpc::get_slot(std::string commitment) {
    Json params = Json::list({Json::dict({{"commitment", commitment}})});
    SCFG_CO_TRY(Json slot, co_await call("getSlot", std::move(params)));
    co_return ints::int_from(slot);
}

Co<Json> SolanaRpc::get_signatures_for_address(Json address, SignatureOptions options) {
    Json opts = Json::dict({{"limit", options.limit}, {"commitment", options.commitment}});
    if (py::truthy(options.until)) opts.as_dict().set("until", options.until);
    if (py::truthy(options.before)) opts.as_dict().set("before", options.before);
    Json params = Json::list({address, opts});
    SCFG_CO_TRY(Json res, co_await call("getSignaturesForAddress", std::move(params)));
    co_return py::truthy(res) ? res : Json::list();
}

Co<Json> SolanaRpc::get_transaction(Json sig, std::string commitment) {
    Result<Json> r = co_await call("getTransaction", tx_params(sig, commitment, std::nullopt));
    if (r.ok() || !r.err().is_a("RpcError")) co_return std::move(r);
    Err e = std::move(r).err();
    if (lower_version(e)) co_return co_await get_transaction(std::move(sig), std::move(commitment));
    std::optional<int64_t> version = named_version(e, tx_version);
    if (!version) co_return e;
    co_return co_await call("getTransaction", tx_params(sig, commitment, version));
}

Co<BigInt> SolanaRpc::first_available(std::string url) {
    if (const BigInt* cached = first_slot_.find(url)) co_return *cached;
    Result<Json> answer = co_await client_->call_at(url, "getFirstAvailableBlock", Json::list());
    Result<BigInt> slot = answer.ok() ? ints::int_from(answer.value()) : Result<BigInt>(std::move(answer).err());
    if (!slot.ok()) {
        // except (RpcUnavailable, RpcError, TypeError, ValueError): unknown, treat as pruned, ask again next time
        const Err& e = slot.err();
        if (e.is_a("RpcUnavailable") || e.is_a("RpcError") || e.is_a("TypeError") || e.is_a("ValueError"))
            co_return BigInt(UNKNOWN_HISTORY);
        co_return std::move(slot).err();
    }
    first_slot_.set(url, slot.value());
    co_return slot.take();
}

Co<TxAny> SolanaRpc::get_transaction_any(Json sig, std::optional<BigInt> min_slot, std::string commitment) {
    bool answered = false, authoritative = false;
    std::vector<std::string> live = client_->live_urls();
    for (const std::string& url : live) {
        SCFG_CO_TRY(std::optional<Attempt> attempt,
                    co_await client_->try_at(url, "getTransaction", tx_params(sig, commitment, std::nullopt)));
        if (!attempt) continue;
        Json res = attempt->result;
        if (attempt->error) {
            const Err& error = *attempt->error;
            if (lower_version(error)) co_return co_await get_transaction_any(sig, min_slot, commitment);
            std::optional<int64_t> version = named_version(error, tx_version);
            if (!version) continue;
            Result<Json> retried = co_await client_->call_at(url, "getTransaction", tx_params(sig, commitment, version));
            if (!retried.ok()) {
                if (retried.err().is_a("RpcUnavailable") || retried.err().is_a("RpcError")) continue;
                co_return std::move(retried).err();
            }
            res = retried.take();
        }
        answered = true;
        if (!res.is_null()) co_return TxAny{std::move(res), false};
        if (min_slot) {
            SCFG_CO_TRY(BigInt first, co_await first_available(url));
            if (first <= *min_slot) authoritative = true;
        }
    }
    if (!answered) {
        SCFG_CO_TRY(std::string shown, py::str(sig));
        co_return Err::make("RpcUnavailable", "no endpoint answered getTransaction for " + shown);
    }
    co_return TxAny{Json(), authoritative};
}

Co<Json> SolanaRpc::get_signature_statuses(Json sigs) {
    SCFG_CO_TRY(std::vector<Json> list, py::iterate(sigs));
    Json params = Json::list({Json(std::move(list)), Json::dict({{"searchTransactionHistory", true}})});
    SCFG_CO_TRY(Json res, co_await call("getSignatureStatuses", std::move(params)));
    co_return py::get(res, "value");
}

Co<bool> SolanaRpc::account_exists(Json address) {
    Json params = Json::list({address, Json::dict({{"encoding", "base64"}, {"commitment", "confirmed"}})});
    SCFG_CO_TRY(Json res, co_await call("getAccountInfo", std::move(params)));
    SCFG_CO_TRY(Json value, py::get(res, "value"));
    co_return !value.is_null();
}

Co<Json> SolanaRpc::get_latest_blockhash() {
    Json params = Json::list({Json::dict({{"commitment", "confirmed"}})});
    SCFG_CO_TRY(Json res, co_await call("getLatestBlockhash", std::move(params)));
    SCFG_CO_TRY(Json value, py::get(res, "value"));
    co_return py::get(value, "blockhash");
}

Co<BigInt> SolanaRpc::get_token_balance(Json token_account) {
    Json params = Json::list({token_account, Json::dict({{"encoding", "jsonParsed"}, {"commitment", "confirmed"}})});
    SCFG_CO_TRY(Json res, co_await call("getAccountInfo", std::move(params)));
    SCFG_CO_TRY(Json value, py::get(res, "value"));
    if (value.is_null()) co_return BigInt(0);
    for (std::string_view key : {"data", "parsed", "info", "tokenAmount", "amount"}) {
        SCFG_CO_TRY(value, py::get(value, key));
    }
    co_return ints::int_from(value);
}

Co<BigInt> SolanaRpc::get_balance(Json address) {
    Json params = Json::list({address, Json::dict({{"commitment", "confirmed"}})});
    SCFG_CO_TRY(Json res, co_await call("getBalance", std::move(params)));
    SCFG_CO_TRY(Json value, py::get(res, "value"));
    co_return ints::int_from(value);
}

Co<BigInt> SolanaRpc::get_priority_fee(Json accounts) {
    SCFG_CO_TRY(std::vector<Json> list, py::iterate(accounts));
    Json params = Json::list({Json(std::move(list))});
    SCFG_CO_TRY(Json rows, co_await call("getRecentPrioritizationFees", std::move(params)));
    std::vector<BigInt> fees;
    if (py::truthy(rows)) {
        SCFG_CO_TRY(std::vector<Json> items, py::iterate(rows));
        for (const Json& f : items) {
            SCFG_CO_TRY(Json fee, py::get(f, "prioritizationFee"));
            SCFG_CO_TRY(BigInt n, ints::int_from(fee));
            fees.push_back(std::move(n));
        }
    }
    std::sort(fees.begin(), fees.end());
    co_return fees.empty() ? BigInt(0) : fees[fees.size() / 2];
}

}  // namespace scfg::solana::rpc
