// Twin of scfg_core/backends/solana.py (see BackendsSolana.h).
#include "Scfg/BackendsSolana.h"

#include "Scfg/Amounts.h"
#include "Scfg/Bytes.h"
#include "Scfg/Err.h"
#include "Scfg/Ints.h"
#include "Scfg/Py.h"
#include "Scfg/PyFloat.h"
#include "Scfg/Receipts.h"
#include "Scfg/Restore.h"
#include "Scfg/Reverify.h"
#include "Scfg/SolanaBase58.h"
#include "Scfg/SolanaPda.h"
#include "Scfg/SolanaTx.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <utility>

namespace scfg::backends::solana::detail {

namespace srpc = ::scfg::solana::rpc;

// A SolanaRpc and the RpcClient it wraps (by reference), kept alive together by one shared_ptr.
struct OwnedSolanaRpc {
    ::scfg::rpc::RpcClient client;
    srpc::SolanaRpc rpc;
    explicit OwnedSolanaRpc(::scfg::rpc::RpcClient c) : client(std::move(c)), rpc(client) {}
    OwnedSolanaRpc(const OwnedSolanaRpc&) = delete;
    OwnedSolanaRpc& operator=(const OwnedSolanaRpc&) = delete;
};

// ---- Python's view of node replies ----

// d.get(key): a dict's value or None; anything else has no .get.
Result<Json> dict_get(const Json& d, std::string_view key) {
    if (!d.is_dict()) return py::no_attribute(d, "get");
    return d.as_dict().get(key);
}

// v or fallback
Json or_else(const Json& v, Json fallback) { return py::truthy(v) ? v : fallback; }

// v[0]: a list's first item, a str's first character, the KeyError of a dict (JSON keys are strs), else the
// TypeError of a value that is not subscriptable.
Result<Json> first(const Json& v) {
    if (v.is_list()) {
        if (v.as_list().empty()) return Err::index_error("list index out of range");
        return v.as_list()[0];
    }
    if (v.is_str()) {
        if (v.as_str().empty()) return Err::index_error("string index out of range");
        size_t i = 0;
        return Json(py::utf8_encode(py::utf8_decode(v.as_str(), i)));
    }
    if (v.is_dict()) return Err::key_error("0");  // repr(0)
    return Err::type_error("'" + std::string(v.type_name()) + "' object is not subscriptable");
}

// reversed(v): a list's items, a dict's keys, a str's characters, last first; anything else is not reversible.
Result<std::vector<Json>> reversed(const Json& v) {
    if (!v.is_list() && !v.is_dict() && !v.is_str()) {
        return Err::type_error("'" + std::string(v.type_name()) + "' object is not reversible");
    }
    SCFG_TRY(std::vector<Json> items, py::iterate(v));
    std::reverse(items.begin(), items.end());
    return items;
}

// ---- dict keys (accountIndex, owner) as Python hashes them ----

// The key a hashable value is under in a dict: equal values (1, 1.0 and True; 0, -0.0 and False) share one, as
// Python's == and hash() make them. json.loads gives every NaN the same float object, so they are one key too. A
// list or a dict is unhashable.
Result<std::string> key_of(const Json& v) {
    if (v.is_list() || v.is_dict()) return Err::type_error("unhashable type: '" + std::string(v.type_name()) + "'");
    if (v.is_null()) return std::string("z");
    if (v.is_str()) return "s" + v.as_str();
    if (v.is_float()) {
        const double f = v.as_float();
        if (std::isnan(f)) return std::string("n");
        if (std::isinf(f) || f != std::trunc(f)) return "f" + pyfloat::repr(f);
    }
    SCFG_TRY(BigInt n, py::py_int(v));  // an int, a bool or an integral float: its exact value
    return "i" + py::hex(n);             // in hex: linear in the size of n, and no digit limit applies
}

// Python's dict over such keys, in insertion order: the first key object inserted stays (rows[k] = v and
// d[k] = d.get(k, 0) + x keep it).
template <class V>
struct KeyedRows {
    struct Row {
        std::string k;  // key_of(key)
        Json key;
        V value;
    };
    std::vector<Row> rows;
    std::map<std::string, size_t> at;

    const Row* find(const std::string& k) const {
        auto it = at.find(k);
        return it == at.end() ? nullptr : &rows[it->second];
    }
    void set(const std::string& k, const Json& key, V value) {
        auto it = at.find(k);
        if (it != at.end()) {
            rows[it->second].value = std::move(value);
            return;
        }
        at.emplace(k, rows.size());
        rows.push_back(Row{k, key, std::move(value)});
    }
};

struct Balance {
    Json owner;
    BigInt amount;
};

// The table of one of meta's balance lists for mint: accountIndex -> (owner, amount).
Result<KeyedRows<Balance>> table(const Json& meta, std::string_view name, std::string_view mint) {
    KeyedRows<Balance> rows;
    SCFG_TRY(Json listed, dict_get(meta, name));
    if (!py::truthy(listed)) return rows;  // `meta.get(name) or []`
    SCFG_TRY(std::vector<Json> each, py::iterate(listed));
    for (const Json& b : each) {
        SCFG_TRY(Json m, dict_get(b, "mint"));
        if (!py::eq(m, Json(mint))) continue;
        // rows[b["accountIndex"]] = (b.get("owner"), int_from(b["uiTokenAmount"]["amount"])): Python evaluates the
        // right-hand side before the subscript's key.
        Json owner = b.as_dict().get("owner");
        SCFG_TRY(Json ui, py::get(b, "uiTokenAmount"));
        SCFG_TRY(Json amount, py::get(ui, "amount"));
        SCFG_TRY(BigInt units, ints::int_from(amount));
        SCFG_TRY(Json index, py::get(b, "accountIndex"));
        SCFG_TRY(std::string k, key_of(index));
        rows.set(k, index, Balance{std::move(owner), std::move(units)});
    }
    return rows;
}

// One distinct accountIndex of the two tables.
struct IndexKey {
    std::string k;
    Json key;
};

// a < b for two accountIndexes, as Python compares them (the caller has checked they are comparable), a NaN last.
bool index_less(const Json& a, const Json& b) {
    if (a.is_str()) return py::compare_code_points(a.as_str(), b.as_str()) < 0;
    return py::number_order(*py::number(a), *py::number(b)) < 0;
}

// sorted(set(pre) | set(post)). Python orders numbers among themselves and strs among themselves; any other mix of two
// or more keys is a TypeError at the comparison that meets it, which every sort reaches. The keys are distinct, so
// without a NaN they are totally ordered and any sort gives Python's order. A NaN goes last (port limit: where
// CPython puts it depends on the set's hash order).
Status sort_indexes(std::vector<IndexKey>& keys) {
    if (keys.size() < 2) return ok();
    // 1 for a str, 2 for a number, 0 for anything else (None, the only other hashable JSON value)
    auto kind = [](const Json& v) { return v.is_str() ? 1 : py::number(v).has_value() ? 2 : 0; };
    const int head = kind(keys[0].key);
    for (size_t i = 1; i < keys.size(); ++i) {
        if (head == 0 || kind(keys[i].key) != head) {
            return Err::type_error("'<' not supported between instances of '" + std::string(keys[i].key.type_name()) +
                                   "' and '" + std::string(keys[0].key.type_name()) + "'");
        }
    }
    std::sort(keys.begin(), keys.end(), [](const IndexKey& a, const IndexKey& b) { return index_less(a.key, b.key); });
    return ok();
}

// _fee_payer: accountKeys[0] (a str, or a {pubkey} entry), or None for no keys.
Result<Json> fee_payer(const Json& tx) {
    SCFG_TRY(Json transaction, dict_get(tx, "transaction"));
    SCFG_TRY(Json message, dict_get(or_else(transaction, Json::dict()), "message"));
    SCFG_TRY(Json keys, dict_get(or_else(message, Json::dict()), "accountKeys"));
    if (!py::truthy(keys)) return Json();
    SCFG_TRY(Json head, first(keys));
    if (head.is_str()) return head;
    return dict_get(head, "pubkey");
}

// ---- one advance(): its arguments, the record and the outcome so far, shared by the step's coroutines ----

struct Step {
    Json pending;
    Json receipts;
    std::string sid;
    std::shared_ptr<srpc::SolanaRpc> rpc;
    chains::Chain chain;
    std::string mint;
    std::string wallet;
    OrderedMap<models::Item> items;
    Json now;
    Json rec;  // a dict: chain.tokens[rec["token"]] was read
    watcher::StepOutcome out;
};

Json match_dict(const transfers::Transfer& t) {
    return Json::dict({{"tx_hash", t.tx_hash},
                       {"log_index", t.log_index},
                       {"block", Json(t.block_number)},
                       {"value", Json(t.value)},
                       {"payer", t.sender},
                       {"recipient", Json(t.recipient)}});
}

// chain.tokens[rec["token"]].address
Result<std::string> mint_of(const chains::Chain& chain, const Json& rec) {
    SCFG_TRY(chains::Token token, chains::token_of(chain, rec));
    return token.address;
}

// Python _qualifies: a transfer at or after the session's start slot that no receipt holds yet, read only as far as
// Python's `and` gets.
Result<bool> qualifies(const std::optional<transfers::Transfer>& t, const Step& s) {
    if (!t) return false;
    SCFG_TRY(Json start, py::get(s.rec, "start_block"));
    SCFG_TRY(bool after, py::compare(Json(t->block_number), py::Cmp::Ge, start));
    if (!after) return false;
    SCFG_TRY(std::string key, receipts::receipt_key(Json(s.chain.key), t->tx_hash, t->log_index));
    return !s.receipts.as_dict().contains(key);
}

// Python _verdict: remember the match; "underpaid" below the least the session must pay, else "matched".
Result<std::string> verdict(const transfers::Transfer& t, Step& s) {
    s.rec.as_dict().set("match", match_dict(t));
    SCFG_TRY(Dec least, watcher::min_amount(s.rec, s.items));
    return std::string(amounts::units_to_decimal(t.value) < least ? "underpaid" : "matched");
}

// Python _drop_hint: the hint's verdict is final; remember which hint it is about, then forget the hint.
void drop_hint(Step& s, std::string error) {
    s.out.error = std::move(error);
    s.out.hint_tx = s.rec.as_dict().get("hint_tx");
    sessions::clear_hint(s.rec.as_dict());
}

// Python _verify_hint: check the signature reported by the browser page or pasted by the player. "matched",
// "underpaid", or nullopt (keep waiting, or the hint was discarded).
Co<std::optional<std::string>> verify_hint(std::shared_ptr<Step> s) {
    using Verdict = std::optional<std::string>;
    SCFG_CO_TRY(Json hint_at, py::get(s->rec, "hint_at"));
    SCFG_CO_TRY(Json waited, py::sub(s->now, hint_at));
    SCFG_CO_TRY(bool expired, py::compare(waited, py::Cmp::Ge, Json(SOLANA_HINT_TIMEOUT_S)));
    SCFG_CO_TRY(Json hint, py::get(s->rec, "hint_tx"));
    Result<Json> fetched = co_await s->rpc->get_transaction(hint);
    Json tx;
    if (fetched.ok()) {
        tx = fetched.take();
    } else if (!fetched.err().is_a("RpcError") || !expired) {
        // an endpoint that keeps refusing must not keep a hint forever: an RpcError only counts once it expired
        co_return std::move(fetched).err();
    }
    if (tx.is_null()) {  // not visible yet, or dropped: the player may pay again after a while
        if (expired) drop_hint(*s, "tx_not_landed");
        co_return Verdict();
    }
    SCFG_CO_TRY(Json meta, dict_get(tx, "meta"));
    SCFG_CO_TRY(Json failed, dict_get(or_else(meta, Json::dict()), "err"));
    if (!failed.is_null()) {
        drop_hint(*s, "tx_reverted");
        co_return Verdict();
    }
    SCFG_CO_TRY(Json sig, py::get(s->rec, "hint_tx"));
    SCFG_CO_TRY(std::optional<transfers::Transfer> t, transfer_to(tx, sig, s->mint, s->wallet));
    SCFG_CO_TRY(bool ok_transfer, qualifies(t, *s));
    if (!ok_transfer) {
        drop_hint(*s, "tx_not_a_payment");
        co_return Verdict();
    }
    SCFG_CO_TRY(std::string v, verdict(*t, *s));
    co_return Verdict(std::move(v));
}

// Python _by_reference: the reference ties a transaction to this session, so any amount matches.
Co<std::optional<std::string>> by_reference(std::shared_ptr<Step> s) {
    using Verdict = std::optional<std::string>;
    SCFG_CO_TRY(Json reference, py::get(s->rec, "reference"));
    srpc::SignatureOptions options;
    options.limit = SIG_PAGE;
    SCFG_CO_TRY(Json infos, co_await s->rpc->get_signatures_for_address(reference, options));
    SCFG_CO_TRY(std::vector<Json> newest_last, reversed(infos));
    for (const Json& info : newest_last) {
        SCFG_CO_TRY(Json failed, dict_get(info, "err"));
        if (!failed.is_null()) continue;
        SCFG_CO_TRY(Json sig, py::get(info, "signature"));
        SCFG_CO_TRY(Json tx, co_await s->rpc->get_transaction(sig));
        std::optional<transfers::Transfer> t;
        if (py::truthy(tx)) {
            SCFG_CO_TRY(t, transfer_to(tx, sig, s->mint, s->wallet));
        }
        SCFG_CO_TRY(bool ok_transfer, qualifies(t, *s));
        if (ok_transfer) {
            SCFG_CO_TRY(std::string v, verdict(*t, *s));
            co_return Verdict(std::move(v));
        }
    }
    co_return Verdict();
}

// Python _walk's (found, reached).
struct Walked {
    std::vector<Json> found;
    bool reached = false;
};

// Python _walk: pages of signatures on address between until (exclusive, older bound) and before (exclusive, newer
// bound), newest first, and whether the walk reached until; if not, the pages cover only the newest
// MAX_SIG_PAGES * SIG_PAGE of them. A signature from before min_slot also ends the walk (and is dropped): with no
// cursor, or one this endpoint doesn't know, the walk would otherwise go through the account's whole history.
Co<Walked> walk(std::shared_ptr<Step> s, Json address, Json until, Json before, Json min_slot) {
    Walked w;
    for (int64_t pages = 0; pages < MAX_SIG_PAGES; ++pages) {
        srpc::SignatureOptions options;
        options.until = until;
        options.before = before;
        options.limit = SIG_PAGE;
        SCFG_CO_TRY(Json page, co_await s->rpc->get_signatures_for_address(address, options));
        SCFG_CO_TRY(std::vector<Json> infos, py::iterate(page));
        for (size_t n = 0; n < infos.size(); ++n) {
            SCFG_CO_TRY(Json slot, dict_get(infos[n], "slot"));
            if (slot.is_null()) continue;
            SCFG_CO_TRY(BigInt number, ints::int_from(slot));
            SCFG_CO_TRY(bool older, py::compare(Json(number), py::Cmp::Lt, min_slot));
            if (older) {
                w.found.insert(w.found.end(), infos.begin(), infos.begin() + static_cast<std::ptrdiff_t>(n));
                w.reached = true;
                co_return w;
            }
        }
        w.found.insert(w.found.end(), infos.begin(), infos.end());
        if (infos.size() < static_cast<size_t>(SIG_PAGE)) {
            w.reached = true;
            co_return w;
        }
        SCFG_CO_TRY(before, py::get(infos.back(), "signature"));
    }
    // The last page was full too: reached `until` only if nothing older is left.
    srpc::SignatureOptions options;
    options.until = until;
    options.before = before;
    options.limit = 1;
    SCFG_CO_TRY(Json more, co_await s->rpc->get_signatures_for_address(address, options));
    w.reached = !py::truthy(more);
    co_return w;
}

// Python _scan: exchange and manual transfers carry no reference, so match the exact amount.
//
// Signatures are only listed newest first, so a backlog larger than one walk is crossed in steps: each walk that
// cannot reach scan_cursor moves scan_before back and examines nothing; once a walk reaches the cursor its window is
// processed oldest first, then scan_before is cleared to look at the newer signatures again. A busy token account
// only delays detection, it never hides a payment. Signatures from before the session's start slot are never
// examined.
Co<bool> scan(std::shared_ptr<Step> s) {
    SCFG_CO_TRY(Json dest, py::get(s->rec, "dest"));
    SCFG_CO_TRY(Json cursor, py::get(s->rec, "scan_cursor"));
    Json before = s->rec.as_dict().get("scan_before");
    SCFG_CO_TRY(Json start, py::get(s->rec, "start_block"));
    SCFG_CO_TRY(Walked w, co_await walk(s, dest, cursor, before, start));
    if (!w.reached) {
        if (w.found.empty()) co_return Err::index_error("list index out of range");  // found[-1]
        SCFG_CO_TRY(Json sig, py::get(w.found.back(), "signature"));
        s->rec.as_dict().set("scan_before", sig);
        co_return false;
    }
    int64_t budget = SCAN_TX_BUDGET;
    for (auto it = w.found.rbegin(); it != w.found.rend(); ++it) {
        const Json& info = *it;
        SCFG_CO_TRY(Json failed, dict_get(info, "err"));
        if (!failed.is_null()) {
            SCFG_CO_TRY(Json sig, py::get(info, "signature"));
            s->rec.as_dict().set("scan_cursor", sig);
            continue;
        }
        if (budget == 0) co_return false;
        budget -= 1;
        SCFG_CO_TRY(Json sig, py::get(info, "signature"));
        SCFG_CO_TRY(Json tx, co_await s->rpc->get_transaction(sig));
        if (tx.is_null()) co_return false;  // listed but not served yet: retry from here next step
        s->rec.as_dict().set("scan_cursor", sig);
        SCFG_CO_TRY(std::optional<transfers::Transfer> t, transfer_to(tx, sig, s->mint, s->wallet));
        SCFG_CO_TRY(bool ok_transfer, qualifies(t, *s));
        if (!ok_transfer) continue;
        SCFG_CO_TRY(Json units, py::get(s->rec, "units"));  // read only for a transfer that qualifies
        if (py::eq(Json(t->value), units)) {
            s->rec.as_dict().set("match", match_dict(*t));
            co_return true;
        }
    }
    s->rec.as_dict().set("scan_before", Json());  // window done: look at the newer signatures again
    co_return false;
}

// The body of advance()'s try: returns where Python returns `out`; an Err where it raises.
Co<void> steps(std::shared_ptr<Step> s) {
    Json::Dict& rec = s->rec.as_dict();
    SCFG_CO_TRY(Json state, py::get(s->rec, "state"));
    if (py::eq(state, Json("awaiting"))) {
        s->out.status = "waiting";
        SCFG_CO_TRY(bool past, sessions::is_past_horizon(s->rec, s->now));
        if (past) {
            sessions::remove_session(s->pending.as_dict(), s->sid);
            s->out.status = "closed";
            co_return ok();
        }
        std::optional<std::string> outcome;
        SCFG_CO_TRY(Json hint, py::get(s->rec, "hint_tx"));
        if (py::truthy(hint)) {
            SCFG_CO_TRY(outcome, co_await verify_hint(s));
        }
        if (!outcome) {
            // The reference also runs while a hint is pending: the hinted transaction may have been dropped and the
            // player paid again.
            SCFG_CO_TRY(outcome, co_await by_reference(s));
            if (outcome) {
                sessions::clear_hint(rec);
                s->out.error.reset();
                s->out.hint_tx = Json();
            } else {
                SCFG_CO_TRY(Json again, py::get(s->rec, "hint_tx"));  // a verdict may have dropped the hint
                if (!py::truthy(again)) {
                    SCFG_CO_TRY(bool found, co_await scan(s));
                    if (found) outcome = "matched";
                }
            }
        }
        if (outcome == "underpaid") {
            sessions::remove_session(s->pending.as_dict(), s->sid);
            s->out.status = "underpaid";
            SCFG_CO_TRY(Json m, py::get(s->rec, "match"));
            SCFG_CO_TRY(models::PaymentResult r, watcher::result_from_match(s->rec, "underpaid", s->chain.key, m));
            s->out.result = std::move(r);
            co_return ok();
        }
        if (outcome != "matched") co_return ok();
        rec.set("state", "confirming");
    }

    s->out.status = "confirming";
    SCFG_CO_TRY(Json m, py::get(s->rec, "match"));
    SCFG_CO_TRY(Json tx_hash, py::get(m, "tx_hash"));
    Json sigs = Json::list();
    sigs.as_list().push_back(tx_hash);
    SCFG_CO_TRY(Json statuses, co_await s->rpc->get_signature_statuses(sigs));
    SCFG_CO_TRY(Json status, first(statuses));
    if (status.is_null()) {
        // Unknown to this endpoint: a dropped fork or a lagging endpoint. Keep the signature as a hint so it is
        // verified again rather than lost.
        rec.set("state", "awaiting");
        rec.set("match", Json());
        rec.set("hint_tx", tx_hash);
        rec.set("hint_at", s->now);
        s->out.status = "reorged";
        co_return ok();
    }
    SCFG_CO_TRY(Json confirmation, dict_get(status, "confirmationStatus"));
    if (!py::eq(confirmation, Json("finalized"))) {
        SCFG_CO_TRY(Json count, dict_get(status, "confirmations"));
        SCFG_CO_TRY(BigInt n, ints::int_from(or_else(count, Json(0))));
        s->out.confirmations = std::move(n);
        co_return ok();
    }
    s->out.confirmations = BigInt(FINAL_CONFIRMATIONS);

    // Transfer(mint, m["payer"], m["recipient"], m["value"], m["tx_hash"], m["log_index"], m["block"]) and
    // make_receipt(chain=, transfer=, kind=rec["kind"], item=rec["item"], token=rec["token"], source="payment"),
    // read in Python's order. The receipt keeps the stored values as they are.
    SCFG_CO_TRY(Json payer, py::get(m, "payer"));
    SCFG_CO_TRY(Json recipient, py::get(m, "recipient"));
    SCFG_CO_TRY(Json value, py::get(m, "value"));
    SCFG_CO_TRY(Json signature, py::get(m, "tx_hash"));
    SCFG_CO_TRY(Json log_index, py::get(m, "log_index"));
    SCFG_CO_TRY(Json block, py::get(m, "block"));
    SCFG_CO_TRY(Json kind, py::get(s->rec, "kind"));
    SCFG_CO_TRY(Json item, py::get(s->rec, "item"));
    SCFG_CO_TRY(Json token_name, py::get(s->rec, "token"));
    if (!py::eq(kind, Json("unlock")) && !py::eq(kind, Json("tip"))) {
        SCFG_CO_TRY(std::string shown, py::repr(kind));
        co_return Err::value_error("invalid kind " + shown);
    }
    SCFG_CO_TRY(Dec amount, watcher::units_of(value));
    Json receipt = Json::dict({{"chain", Json(s->chain.key)},
                               {"tx_hash", signature},
                               {"log_index", log_index},
                               {"block", block},
                               {"kind", kind},
                               {"item", item},
                               {"amount", Json(amount.str())},
                               {"token", token_name},
                               {"payer", payer},
                               {"recipient", recipient},
                               {"source", "payment"}});
    SCFG_CO_TRY(bool added, receipts::add_receipt(s->receipts.as_dict(), receipt));
    (void)added;
    sessions::remove_session(s->pending.as_dict(), s->sid);
    s->out.status = "paid";
    SCFG_CO_TRY(models::PaymentResult r, watcher::result_from_match(s->rec, "paid", s->chain.key, m));
    s->out.result = std::move(r);
    co_return ok();
}

// A stored key Python hands to compile_message as it is: decode_fixed(key, 32)'s ValueError for anything that is not
// a 32-byte base58 str, raised where compile_message decodes it.
Status check_key(const Json& key) {
    SCFG_TRY_VOID(::scfg::solana::base58::decode_fixed(key, 32));
    if (!key.is_str()) ::scfg::detail::programming_error("decode_fixed accepted a key that is not a str");
    return ok();
}

}  // namespace scfg::backends::solana::detail

namespace scfg::backends::solana {

Result<std::optional<transfers::Transfer>> transfer_to(const Json& tx, const Json& sig, std::string_view mint,
                                                       std::string_view owner) {
    using Maybe = std::optional<transfers::Transfer>;
    SCFG_TRY(Json meta_value, detail::dict_get(tx, "meta"));
    const Json meta = detail::or_else(meta_value, Json::dict());
    SCFG_TRY(Json failed, detail::dict_get(meta, "err"));
    if (!failed.is_null()) return Maybe();
    SCFG_TRY(detail::KeyedRows<detail::Balance> pre, detail::table(meta, "preTokenBalances", mint));
    SCFG_TRY(detail::KeyedRows<detail::Balance> post, detail::table(meta, "postTokenBalances", mint));

    std::vector<detail::IndexKey> indexes;  // set(pre) | set(post): pre's keys first, each equal key once
    for (const auto& row : pre.rows) indexes.push_back(detail::IndexKey{row.k, row.key});
    for (const auto& row : post.rows) {
        if (pre.find(row.k) == nullptr) indexes.push_back(detail::IndexKey{row.k, row.key});
    }
    SCFG_TRY_VOID(detail::sort_indexes(indexes));

    BigInt gained = 0;
    Json index;  // None until an account of owner's gained (an accountIndex of None stays None)
    detail::KeyedRows<BigInt> drops;
    const Json owner_json(owner);
    for (const detail::IndexKey& idx : indexes) {
        const auto* after = post.find(idx.k);
        const auto* before = pre.find(idx.k);
        const Json& who = (after != nullptr ? after : before)->value.owner;
        const BigInt delta = (after != nullptr ? after->value.amount : BigInt(0)) -
                             (before != nullptr ? before->value.amount : BigInt(0));
        if (py::eq(who, owner_json)) {
            gained = gained + delta;
            if (delta > 0 && index.is_null()) index = idx.key;
        } else if (delta < 0) {
            SCFG_TRY(std::string k, detail::key_of(who));  // drops.get(who, 0): an unhashable owner is a TypeError
            const auto* sum = drops.find(k);
            drops.set(k, who, (sum != nullptr ? sum->value : BigInt(0)) + delta);
        }
    }
    if (gained <= 0 || index.is_null()) return Maybe();
    Json payer;
    if (!drops.rows.empty()) {
        const auto* best = &drops.rows[0];  // min(drops, key=drops.get): the first of the largest drops
        for (const auto& d : drops.rows) {
            if (d.value < best->value) best = &d;
        }
        payer = best->key;
    } else {
        SCFG_TRY(payer, detail::fee_payer(tx));
    }
    SCFG_TRY(Json slot_value, py::get(tx, "slot"));
    SCFG_TRY(BigInt slot, ints::int_from(slot_value));
    return Maybe(transfers::Transfer{std::string(mint), std::move(payer), std::string(owner), std::move(gained), sig,
                                     std::move(index), std::move(slot)});
}

Result<base::Rpc> SolanaBackend::make_rpc(const chains::Chain& chain, coio::Transport& transport) const {
    SCFG_TRY(::scfg::rpc::RpcClient client, ::scfg::rpc::RpcClient::make(chain.rpc, transport));
    auto owned = std::make_shared<detail::OwnedSolanaRpc>(std::move(client));
    return base::Rpc(std::shared_ptr<detail::srpc::SolanaRpc>(owned, &owned->rpc));
}

Result<std::string> SolanaBackend::normalize_tx(const Json& text) const {
    return ::scfg::solana::base58::normalize_signature(text);
}

Co<Json> SolanaBackend::start(base::Rpc rpc, chains::Chain chain, std::optional<std::string> wallet,
                              std::string token, std::reference_wrapper<Entropy> entropy) const {
    SCFG_CO_TRY(std::shared_ptr<detail::srpc::SolanaRpc> client, base::solana_client(rpc));
    const chains::Token* t = chain.tokens.find(token);
    if (t == nullptr) co_return Err::key_error(py::repr_str(token));
    const Json owner = wallet ? Json(*wallet) : Json();
    SCFG_CO_TRY(std::string dest, ::scfg::solana::pda::associated_token_address(owner, Json(t->address)));
    SCFG_CO_TRY(BigInt slot, co_await client->get_slot());
    detail::srpc::SignatureOptions options;
    options.limit = 1;
    SCFG_CO_TRY(Json newest, co_await client->get_signatures_for_address(Json(dest), options));
    const BigInt start_block = std::max(BigInt(0), slot - BigInt(START_SLOT_MARGIN));
    const std::vector<uint8_t> reference = entropy.get().token_bytes(32);
    Json cursor;
    if (py::truthy(newest)) {
        SCFG_CO_TRY(Json head, detail::first(newest));
        SCFG_CO_TRY(cursor, py::get(head, "signature"));
    }
    co_return Json::dict({{"start_block", Json(start_block)},
                          {"reference", Json(::scfg::solana::base58::b58encode(reference))},
                          {"dest", Json(dest)},
                          {"scan_cursor", cursor},
                          {"scan_before", Json()}});
}

Co<watcher::StepOutcome> SolanaBackend::advance(Json pending, Json receipts, std::string sid, base::Rpc rpc,
                                                chains::Chain chain, std::string wallet,
                                                OrderedMap<models::Item> items, Json now) const {
    SCFG_CO_TRY(std::shared_ptr<detail::srpc::SolanaRpc> client, base::solana_client(rpc));
    if (!pending.is_dict() || !receipts.is_dict()) co_return Err::type_error("the pending and receipt stores must be dicts");
    watcher::StepOutcome out;
    out.session_id = sid;
    out.status = "missing";
    out.required = BigInt(FINAL_CONFIRMATIONS);
    Json rec = pending.as_dict().get(sid);
    if (rec.is_null()) co_return out;
    SCFG_CO_TRY(std::string mint, detail::mint_of(chain, rec));
    auto s = std::make_shared<detail::Step>(detail::Step{std::move(pending), std::move(receipts), std::move(sid),
                                                         std::move(client), std::move(chain), std::move(mint),
                                                         std::move(wallet), std::move(items), std::move(now),
                                                         std::move(rec), std::move(out)});
    Status done = co_await detail::steps(s);
    if (!done.ok()) {
        if (!::scfg::rpc::network_failure(done.err())) co_return std::move(done).err();
        // A verdict on the hint from earlier in this step wins: the hint is already cleared, and later steps are
        // plain "waiting", so it would never be reported.
        if (!s->out.error) s->out.error = "network_unavailable";
    }
    co_return s->out;
}

Co<std::string> SolanaBackend::reverify(Json receipt, base::Rpc rpc, chains::Chain chain,
                                        std::vector<std::string> wallets) const {
    SCFG_CO_TRY(std::shared_ptr<detail::srpc::SolanaRpc> client, base::solana_client(rpc));
    SCFG_CO_TRY(Json block, py::get(receipt, "block"));
    SCFG_CO_TRY(BigInt slot, ints::int_from(block));
    SCFG_CO_TRY(Json owner, detail::dict_get(receipt, "recipient"));
    const bool accepted = owner.is_str() && std::find(wallets.begin(), wallets.end(), owner.as_str()) != wallets.end();
    if (!accepted) co_return std::string("drop");
    // try: ... except (RpcUnavailable, RpcError): return "keep"
    Result<BigInt> current = co_await client->get_slot("finalized");
    if (!current.ok()) {
        if (::scfg::rpc::network_failure(current.err())) co_return std::string("keep");
        co_return std::move(current).err();
    }
    if (slot > current.value()) {
        const bool impossible = slot > current.value() + BigInt(::scfg::reverify::FUTURE_TOLERANCE_BLOCKS);
        co_return std::string(impossible ? "drop" : "keep");
    }
    SCFG_CO_TRY(Json signature, py::get(receipt, "tx_hash"));
    Result<detail::srpc::TxAny> any = co_await client->get_transaction_any(signature, slot);
    if (!any.ok()) {
        if (::scfg::rpc::network_failure(any.err())) co_return std::string("keep");
        co_return std::move(any).err();
    }
    const detail::srpc::TxAny found = any.take();
    if (found.tx.is_null()) co_return std::string(found.authoritative ? "drop" : "keep");
    // try: transfer_to(...) except (KeyError, TypeError, ValueError, AttributeError): a payload this build can't
    // read, so the receipt can't be judged. Anything else (an OverflowError from an infinite amount) leaves, as in
    // Python, for reverify_receipts to contain.
    Result<std::optional<transfers::Transfer>> read = [&]() -> Result<std::optional<transfers::Transfer>> {
        SCFG_TRY(Json sig, py::get(receipt, "tx_hash"));
        SCFG_TRY(std::string mint, detail::mint_of(chain, receipt));
        return transfer_to(found.tx, sig, mint, owner.as_str());
    }();
    if (!read.ok()) {
        const Err& e = read.err();
        if (e.is_a("KeyError") || e.is_a("TypeError") || e.is_a("ValueError") || e.is_a("AttributeError")) {
            co_return std::string("keep");
        }
        co_return std::move(read).err();
    }
    const std::optional<transfers::Transfer> t = read.take();
    bool genuine = t.has_value() && t->block_number == slot;
    if (genuine) {
        SCFG_CO_TRY(Json stored_index, py::get(receipt, "log_index"));
        SCFG_CO_TRY(BigInt index, ints::int_from(stored_index));
        genuine = py::eq(t->log_index, Json(index));
    }
    if (genuine) {
        SCFG_CO_TRY(Json stored, py::get(receipt, "amount"));
        SCFG_CO_TRY(Dec amount, amounts::stored_amount(stored));
        SCFG_CO_TRY(BigInt units, amounts::decimal_to_units(amount));
        genuine = t->value == units;
    }
    co_return std::string(genuine ? "keep" : "drop");
}

Co<std::optional<std::vector<restore::RestoreCandidate>>> SolanaBackend::lookup(std::string tx, config::Config config,
                                                                                chains::Chain chain, base::Rpc rpc,
                                                                                Json receipts,
                                                                                base::ErrorSink on_error) const {
    using Candidates = std::vector<restore::RestoreCandidate>;
    SCFG_CO_TRY(std::shared_ptr<detail::srpc::SolanaRpc> client, base::solana_client(rpc));
    SCFG_CO_TRY(detail::srpc::TxAny any, co_await client->get_transaction_any(Json(tx)));
    const Json found = any.tx;
    if (found.is_null()) co_return std::optional<Candidates>();
    SCFG_CO_TRY(Json meta, detail::dict_get(found, "meta"));
    SCFG_CO_TRY(Json failed, detail::dict_get(detail::or_else(meta, Json::dict()), "err"));
    if (!failed.is_null()) co_return Err::restore_error("tx_reverted", "tx_reverted");
    Candidates out;
    const std::vector<std::string> owners = config.accepted_wallets_for("solana");
    for (const auto& [name, token] : chain.tokens) {
        for (const std::string& owner : owners) {
            SCFG_CO_TRY(std::optional<transfers::Transfer> t, transfer_to(found, Json(tx), token.address, owner));
            if (!t) continue;
            SCFG_CO_TRY(std::optional<restore::Classified> classified,
                        restore::classify_or_skip(config, t->value, on_error));
            if (!classified) continue;
            SCFG_CO_TRY(std::string key, receipts::receipt_key(Json(chain.key), t->tx_hash, t->log_index));
            restore::RestoreCandidate c;
            c.chain = chain.key;
            c.token = name;
            c.transfer = std::move(*t);  // carried as transfer_to read it (a str index, an int payer, a huge slot)
            c.kind = std::move(classified->kind);
            c.item = std::move(classified->item);
            c.choices = std::move(classified->choices);
            c.already_restored = receipts.is_dict() && receipts.as_dict().contains(key);
            out.push_back(std::move(c));
        }
    }
    if (out.empty()) co_return Err::restore_error("tx_not_a_payment", "tx_not_a_payment");
    co_return std::optional<Candidates>(std::move(out));
}

Co<OrderedMap<bool>> SolanaBackend::check_ready(base::LazyRpc rpc, chains::Chain chain,
                                                std::optional<std::string> wallet) const {
    OrderedMap<bool> ready;
    const Json owner = wallet ? Json(*wallet) : Json();
    for (const auto& [name, token] : chain.tokens) {
        SCFG_CO_TRY(base::Rpc built, rpc());  // `rpc.account_exists`: _LazyRpc builds the client first
        SCFG_CO_TRY(std::shared_ptr<detail::srpc::SolanaRpc> client, base::solana_client(built));
        SCFG_CO_TRY(std::string address, ::scfg::solana::pda::associated_token_address(owner, Json(token.address)));
        SCFG_CO_TRY(bool exists, co_await client->account_exists(Json(address)));
        ready.set(name, exists);
    }
    co_return ready;
}

Co<Json> SolanaBackend::build_payment(Json rec, base::Rpc rpc, chains::Chain chain, Json account) const {
    namespace stx = ::scfg::solana::tx;
    SCFG_CO_TRY(std::shared_ptr<detail::srpc::SolanaRpc> client, base::solana_client(rpc));
    SCFG_CO_TRY(std::string payer, ::scfg::solana::base58::normalize_pubkey(account));
    SCFG_CO_TRY(std::string mint, detail::mint_of(chain, rec));
    SCFG_CO_TRY(std::string source, ::scfg::solana::pda::associated_token_address(Json(payer), Json(mint)));
    SCFG_CO_TRY(BigInt token_balance, co_await client->get_token_balance(Json(source)));
    SCFG_CO_TRY(BigInt sol_balance, co_await client->get_balance(Json(payer)));
    SCFG_CO_TRY(Json blockhash, co_await client->get_latest_blockhash());
    SCFG_CO_TRY(Json fee_dest, py::get(rec, "dest"));
    Json accounts = Json::list();
    accounts.as_list().push_back(fee_dest);
    SCFG_CO_TRY(BigInt fee, co_await client->get_priority_fee(accounts));
    const BigInt price = std::min(BigInt(MAX_PRIORITY_FEE), std::max(BigInt(MIN_PRIORITY_FEE), fee));

    std::vector<stx::Instruction> instructions;
    SCFG_CO_TRY(stx::Instruction limit, stx::set_compute_unit_limit(BigInt(COMPUTE_UNITS)));
    SCFG_CO_TRY(stx::Instruction unit_price, stx::set_compute_unit_price(price));
    // transfer_checked(source=, mint=, destination=rec["dest"], owner=, amount=rec["units"], decimals=6,
    // extra=(rec["reference"],)): its arguments in Python's order, then amount.to_bytes (an int or a bool has it).
    SCFG_CO_TRY(Json dest, py::get(rec, "dest"));
    SCFG_CO_TRY(Json units, py::get(rec, "units"));
    SCFG_CO_TRY(Json reference, py::get(rec, "reference"));
    if (!units.is_int() && !units.is_bool()) co_return py::no_attribute(units, "to_bytes");
    SCFG_CO_TRY(BigInt amount, py::index(units));
    const std::string dest_text = dest.is_str() ? dest.as_str() : std::string();
    const std::string reference_text = reference.is_str() ? reference.as_str() : std::string();
    SCFG_CO_TRY(stx::Instruction transfer,
                stx::transfer_checked(source, mint, dest_text, payer, amount, 6, {reference_text}));
    // compile_message decodes every key in its order: the destination and the reference come before the blockhash.
    SCFG_CO_TRY_VOID(detail::check_key(dest));
    SCFG_CO_TRY_VOID(detail::check_key(reference));
    if (!blockhash.is_str()) SCFG_CO_TRY_VOID(detail::check_key(blockhash));
    instructions.push_back(std::move(limit));
    instructions.push_back(std::move(unit_price));
    instructions.push_back(std::move(transfer));
    SCFG_CO_TRY(std::vector<uint8_t> message, stx::compile_message(payer, instructions, blockhash.as_str()));
    SCFG_CO_TRY(std::vector<uint8_t> unsigned_tx, stx::unsigned_transaction(message));
    SCFG_CO_TRY(std::string token_text, py::int_str(token_balance));
    SCFG_CO_TRY(std::string sol_text, py::int_str(sol_balance));
    co_return Json::dict({{"transaction", Json(bytes::b64encode(unsigned_tx))},
                          {"token_balance", Json(token_text)},
                          {"sol_balance", Json(sol_text)}});
}

}  // namespace scfg::backends::solana
