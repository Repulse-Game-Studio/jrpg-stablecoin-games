// Twin of scfg_core/watcher.py (see Watcher.h).
#include "Scfg/Watcher.h"

#include "Scfg/Amounts.h"
#include "Scfg/Err.h"
#include "Scfg/Evm.h"
#include "Scfg/Py.h"
#include "Scfg/Receipts.h"
#include "Scfg/Sessions.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace scfg::watcher::detail {

// One advance(): its arguments, the session record and the outcome built so far, shared by the step's coroutines
// (Python's locals and `out`; a coroutine takes values, so they share it through a pointer).
struct Step {
    Json pending;
    Json receipts;
    std::string sid;
    std::shared_ptr<rpc::RpcClient> rpc;
    chains::Chain chain;
    std::string wallet;
    OrderedMap<models::Item> items;
    Json now;
    Json rec;
    StepOutcome out;
};

// chain.tokens[rec["token"]].
Result<chains::Token> token_of(const chains::Chain& chain, const Json& rec) {
    SCFG_TRY(Json key, py::get(rec, "token"));
    SCFG_TRY(std::optional<std::string> name, py::str_key(key));
    if (name) {
        if (const chains::Token* t = chain.tokens.find(*name)) return *t;
    }
    return Err::key_error(py::repr(key));
}

Json match_dict(const evm::TransferLog& t) {
    return Json::dict({{"tx_hash", Json(t.tx_hash)},
                       {"log_index", Json(t.log_index)},
                       {"block", Json(t.block_number)},
                       {"block_hash", Json(t.block_hash)},
                       {"value", Json(t.value)},
                       {"payer", Json(t.sender)},
                       {"recipient", Json(t.recipient)}});
}

// Python _qualifies: a transfer of the session's token to the wallet, at or after the session's start block, that
// no receipt holds yet. The record is read only as far as Python's `and` gets.
Result<bool> qualifies(const std::optional<evm::TransferLog>& t, const Step& s) {
    if (!t) return false;
    SCFG_TRY(chains::Token token, token_of(s.chain, s.rec));
    if (t->token != token.address || t->recipient != s.wallet) return false;
    SCFG_TRY(Json start, py::get(s.rec, "start_block"));
    SCFG_TRY(bool after, py::compare(Json(t->block_number), py::Cmp::Ge, start));
    if (!after) return false;
    // a log index of more than 4300 digits is str()'s ValueError: the step fails, and the Engine contains it
    SCFG_TRY(std::string key, receipts::receipt_key(s.chain.key, t->tx_hash, t->log_index));
    return !s.receipts.as_dict().contains(key);
}

// The hint's verdict is final: remember which hint it is about, then forget the hint.
void drop_hint(Step& s, std::string error) {
    s.out.hint_tx = s.rec.as_dict().get("hint_tx");
    sessions::clear_hint(s.rec.as_dict());
    s.out.error = std::move(error);
}

// Python _verify_hint: check the tx hash reported by the browser page or pasted by the player. "matched",
// "underpaid", or nullopt (keep waiting, or the hint was discarded).
Co<std::optional<std::string>> verify_hint(std::shared_ptr<Step> s) {
    using Verdict = std::optional<std::string>;
    SCFG_CO_TRY(Json hint, py::get(s->rec, "hint_tx"));
    SCFG_CO_TRY(Json receipt, co_await s->rpc->get_transaction_receipt(hint));
    if (receipt.is_null()) {  // not mined yet (or unknown): give it up to 30 minutes
        SCFG_CO_TRY(Json hint_at, py::get(s->rec, "hint_at"));
        SCFG_CO_TRY(Json waited, py::sub(s->now, hint_at));
        SCFG_CO_TRY(bool timed_out, py::compare(waited, py::Cmp::Ge, Json(sessions::HINT_TIMEOUT_S)));
        if (timed_out) sessions::clear_hint(s->rec.as_dict());
        co_return Verdict();
    }
    if (!receipt.is_dict()) {
        co_return Err::make("AttributeError", "'" + std::string(receipt.type_name()) + "' object has no attribute 'get'");
    }
    if (py::eq(receipt.as_dict().get("status"), Json("0x0"))) {
        drop_hint(*s, "tx_reverted");
        co_return Verdict();
    }
    std::vector<evm::TransferLog> candidates;
    Json logs = receipt.as_dict().get("logs");
    if (py::truthy(logs)) {  // receipt.get("logs") or []
        SCFG_CO_TRY(std::vector<Json> each, py::iterate(logs));
        for (const Json& lg : each) {
            SCFG_CO_TRY(std::optional<evm::TransferLog> t, s->chain.parse_transfer(lg));
            SCFG_CO_TRY(bool ok_transfer, qualifies(t, *s));
            if (ok_transfer && t->value > 0) candidates.push_back(*t);
        }
    }
    if (candidates.empty()) {
        drop_hint(*s, "tx_not_a_payment");
        co_return Verdict();
    }
    const evm::TransferLog* best = &candidates[0];  // max(): the first of the largest values
    for (const evm::TransferLog& t : candidates) {
        if (t.value > best->value) best = &t;
    }
    s->rec.as_dict().set("match", match_dict(*best));
    SCFG_CO_TRY(Dec least, min_amount(s->rec, s->items));
    if (amounts::units_to_decimal(best->value) < least) co_return Verdict("underpaid");
    co_return Verdict("matched");
}

// Python _scan: look for a transfer of exactly the session's encoded amount since the cursor.
Co<bool> scan(std::shared_ptr<Step> s, BigInt head) {
    SCFG_CO_TRY(Json cursor, py::get(s->rec, "cursor"));
    SCFG_CO_TRY(bool behind, py::compare(cursor, py::Cmp::Gt, Json(head)));
    if (behind) co_return false;  // this endpoint is behind the one that set start_block
    const BigInt span = s->rpc->log_range();
    const BigInt step = span.is_zero() ? BigInt(SCAN_STEP_BLOCKS)
                                       : std::min(BigInt(SCAN_STEP_BLOCKS), span * BigInt(MAX_SCAN_CALLS));
    SCFG_CO_TRY(chains::Token token, token_of(s->chain, s->rec));
    SCFG_CO_TRY(std::string topic, evm::address_topic(Json(s->wallet)));
    // The cursor compared with an int, so it is a number. A float reaches get_logs, whose hex() refuses it, except
    // a NaN: min(head, nan) is head and get_logs' `while start <= to_block` never runs.
    BigInt to = head;
    std::vector<Json> logs;
    if (cursor.is_float()) {
        if (!std::isnan(cursor.as_float())) co_return Err::type_error("'float' object cannot be interpreted as an integer");
    } else {
        SCFG_CO_TRY(BigInt from, py::index(cursor));
        to = std::min(head, from + step - BigInt(1));
        Json topics = Json::list({Json(evm::TRANSFER_TOPIC), Json(), Json(topic)});
        SCFG_CO_TRY(logs, co_await s->rpc->get_logs(token.log_source(), topics, from, to));
    }
    std::vector<evm::TransferLog> found;
    for (const Json& lg : logs) {
        SCFG_CO_TRY(std::optional<evm::TransferLog> t, s->chain.parse_transfer(lg));
        SCFG_CO_TRY(bool ok_transfer, qualifies(t, *s));
        if (!ok_transfer) continue;
        SCFG_CO_TRY(Json units, py::get(s->rec, "units"));  // read only for a transfer that qualifies
        if (py::eq(Json(t->value), units)) found.push_back(*t);
    }
    std::stable_sort(found.begin(), found.end(), [](const evm::TransferLog& a, const evm::TransferLog& b) {
        return a.block_number != b.block_number ? a.block_number < b.block_number : a.log_index < b.log_index;
    });
    s->rec.as_dict().set("cursor", Json(to + BigInt(1)));
    if (found.empty()) co_return false;
    s->rec.as_dict().set("match", match_dict(found[0]));
    co_return true;
}

// The body of advance()'s try: returns where Python returns `out`; an Err where it raises.
Co<void> steps(std::shared_ptr<Step> s) {
    std::optional<BigInt> head;
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
            SCFG_CO_TRY(Json again, py::get(s->rec, "hint_tx"));  // the verdict may have dropped the hint
            if (!py::truthy(again)) {
                SCFG_CO_TRY(BigInt h, co_await s->rpc->block_number());
                head = h;
                SCFG_CO_TRY(bool found, co_await scan(s, h));
                if (found) outcome = "matched";
            }
        }
        if (outcome == "underpaid") {
            sessions::remove_session(s->pending.as_dict(), s->sid);
            s->out.status = "underpaid";
            SCFG_CO_TRY(Json m, py::get(s->rec, "match"));
            SCFG_CO_TRY(models::PaymentResult r, result_from_match(s->rec, "underpaid", s->chain.key, m));
            s->out.result = std::move(r);
            co_return ok();
        }
        if (outcome != "matched") co_return ok();
        s->rec.as_dict().set("state", "confirming");
    }

    s->out.status = "confirming";
    SCFG_CO_TRY(Json m, py::get(s->rec, "match"));
    if (!head) {
        SCFG_CO_TRY(BigInt h, co_await s->rpc->block_number());
        head = h;
    }
    SCFG_CO_TRY(Json block, py::get(m, "block"));
    SCFG_CO_TRY(bool lagging, py::compare(Json(*head), py::Cmp::Lt, block));
    if (lagging) co_return ok();  // lagging endpoint, not a reorg
    SCFG_CO_TRY(BigInt number, py::index(block));  // get_block_hash's hex(number)
    SCFG_CO_TRY(std::optional<std::string> current, co_await s->rpc->get_block_hash(number));
    bool same = false;
    if (current) {
        SCFG_CO_TRY(Json recorded, py::get(m, "block_hash"));
        same = py::eq(Json(py::lower_for_ascii(*current)), recorded);
    }
    if (!same) {
        s->rec.as_dict().set("state", "awaiting");
        s->rec.as_dict().set("match", Json());
        SCFG_CO_TRY(Json cursor, py::get(s->rec, "cursor"));
        SCFG_CO_TRY(bool earlier, py::compare(block, py::Cmp::Lt, cursor));  // min(cursor, block)
        s->rec.as_dict().set("cursor", earlier ? block : cursor);
        s->out.status = "reorged";
        co_return ok();
    }
    s->out.confirmations = *head - number + BigInt(1);
    if (s->out.confirmations < s->chain.confirmations) co_return ok();

    // TransferLog(token, m["payer"], m["recipient"], m["value"], m["tx_hash"], m["log_index"], m["block"],
    // m["block_hash"]) and make_receipt(chain=, transfer=, kind=rec["kind"], item=rec["item"], token=rec["token"],
    // source="payment"), read in Python's order. The receipt keeps the stored values as they are.
    SCFG_CO_TRY(chains::Token token, token_of(s->chain, s->rec));
    (void)token;
    SCFG_CO_TRY(Json payer, py::get(m, "payer"));
    SCFG_CO_TRY(Json recipient, py::get(m, "recipient"));
    SCFG_CO_TRY(Json value, py::get(m, "value"));
    SCFG_CO_TRY(Json tx_hash, py::get(m, "tx_hash"));
    SCFG_CO_TRY(Json log_index, py::get(m, "log_index"));
    SCFG_CO_TRY(Json block_hash, py::get(m, "block_hash"));
    (void)block_hash;
    SCFG_CO_TRY(Json kind, py::get(s->rec, "kind"));
    SCFG_CO_TRY(Json item, py::get(s->rec, "item"));
    SCFG_CO_TRY(Json token_name, py::get(s->rec, "token"));
    if (!py::eq(kind, Json("unlock")) && !py::eq(kind, Json("tip"))) co_return Err::value_error("invalid kind " + py::repr(kind));
    SCFG_CO_TRY(Dec amount, units_of(value));
    Json receipt = Json::dict({{"chain", Json(s->chain.key)},
                               {"tx_hash", tx_hash},
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
    SCFG_CO_TRY(models::PaymentResult r, result_from_match(s->rec, "paid", s->chain.key, m));
    s->out.result = std::move(r);
    co_return ok();
}

}  // namespace scfg::watcher::detail

namespace scfg::watcher {

// A NaN or an infinity has no digits to move: Python's exponent arithmetic on its 'n' or 'F' exponent is a TypeError.
Result<Dec> units_of(const Json& v) {
    if (v.is_int() || v.is_bool()) {
        SCFG_TRY(BigInt n, py::index(v));
        return amounts::units_to_decimal(n);
    }
    SCFG_TRY(std::optional<Dec> d, Dec::from_json(v));
    if (!d) return Err::type_error("unsupported operand type(s) for -: 'str' and 'int'");
    return d->move_point(-amounts::TOKEN_DECIMALS);
}

Result<Dec> min_amount(const Json& rec, const OrderedMap<models::Item>& items) {
    SCFG_TRY(Json kind, py::get(rec, "kind"));
    if (py::eq(kind, Json("unlock"))) {
        SCFG_TRY(Json key, py::get(rec, "item"));
        SCFG_TRY(std::optional<std::string> name, py::str_key(key));  // items.get(rec["item"])
        if (name) {
            if (const models::Item* item = items.find(*name)) return item->min_price;
        }
        return sessions::session_price(rec);
    }
    return Dec(BigInt(1), -amounts::TOKEN_DECIMALS, false);
}

Result<models::PaymentResult> result_from_match(const Json& rec, std::string status, std::string chain_key,
                                                const Json& m) {
    models::PaymentResult r;
    r.status = std::move(status);
    SCFG_TRY(r.kind, py::get(rec, "kind"));
    SCFG_TRY(r.item, py::get(rec, "item"));
    SCFG_TRY(Json value, py::get(m, "value"));
    SCFG_TRY(Dec amount, units_of(value));
    r.amount = amount;
    SCFG_TRY(r.token, py::get(rec, "token"));
    r.chain = Json(std::move(chain_key));
    SCFG_TRY(r.tx_hash, py::get(m, "tx_hash"));
    SCFG_TRY(r.payer, py::get(m, "payer"));
    SCFG_TRY(r.session_id, py::get(rec, "id"));
    return r;
}

Co<StepOutcome> advance(Json pending, Json receipts, std::string sid, std::shared_ptr<rpc::RpcClient> rpc,
                        chains::Chain chain, std::string wallet, OrderedMap<models::Item> items, Json now) {
    if (!pending.is_dict() || !receipts.is_dict()) co_return Err::type_error("the pending and receipt stores must be dicts");
    if (!rpc) ::scfg::detail::programming_error("watcher::advance without an RpcClient");
    StepOutcome out;
    out.session_id = sid;
    out.status = "missing";
    out.required = chain.confirmations;
    Json rec = pending.as_dict().get(sid);
    if (rec.is_null()) co_return out;
    auto s = std::make_shared<detail::Step>(detail::Step{std::move(pending), std::move(receipts), std::move(sid),
                                                         std::move(rpc), std::move(chain), std::move(wallet),
                                                         std::move(items), std::move(now), std::move(rec),
                                                         std::move(out)});
    Status done = co_await detail::steps(s);
    if (!done.ok()) {
        if (!done.err().is_a("RpcUnavailable") && !done.err().is_a("RpcError")) co_return std::move(done).err();
        s->out.error = "network_unavailable";
    }
    co_return s->out;
}

}  // namespace scfg::watcher
