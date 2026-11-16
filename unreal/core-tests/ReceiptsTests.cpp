// No Python twin: tests of scfg::receipts beyond the tier1/receipts.json vectors: the receipt_key forms (EVM hashes
// lowercased, Solana signatures and "0X" kept), a stored log_index of any size formatted exactly (the C# port raised
// OverflowException beyond int64), duplicate add_receipt, the malformed-store classes, and compute_ownership's
// code-point order, "0" total and skipped entries. Every expectation is scfg_core's own output (python3 with
// plugin/game/python-packages).
#include "support/Test.h"

#include "Scfg/BigInt.h"
#include "Scfg/Dec.h"
#include "Scfg/Evm.h"
#include "Scfg/Json.h"
#include "Scfg/Models.h"
#include "Scfg/OrderedMap.h"
#include "Scfg/Receipts.h"
#include "Scfg/Transfers.h"

#include <optional>
#include <string>
#include <vector>

using scfg::BigInt;
using scfg::Dec;
using scfg::Json;
using scfg::Result;
namespace receipts = scfg::receipts;

static const char* const kHashUpper = "0xABABABABABABABABABABABABABABABABABABABABABABABABABABABABABABABAB";
static const char* const kHashLower = "0xabababababababababababababababababababababababababababababababab";
static const char* const kSig = "1GMkH3brNXiNNs1tiFZHu4yZSRrzJwxi5wB9bHFtMinfCXNnR1adh8Vo8NTheK4evneedH4qmvjeqcBBNAefgS";
static const char* const kSigLower = "1gmkh3brnxinns1tifzhu4yzsrrzjwxi5wb9bhftminfcxnnr1adh8vo8nthek4evneedh4qmvjeqcbbnaefgs";

template <class T>
static std::string outcome(const Result<T>& r) {
    if (r.ok()) return "ok";
    return r.err().kind + ": " + r.err().message;
}

template <class T>
static std::string kind_of(const Result<T>& r) {
    return r.ok() ? "ok" : r.err().kind;
}

static std::string key(Json chain, Json tx, Json index) {
    Result<std::string> k = receipts::receipt_key(chain, tx, index);
    return k.ok() ? k.value() : outcome(k);
}

static Dec dec(const char* s) { return *Dec::parse(s).value(); }

static scfg::models::Item item(const char* id, const char* min_price) {
    return scfg::models::Item{id, 2, dec("4.99"), "USDC", "Title", dec(min_price)};
}

static Json receipt(const char* chain, const char* tx, Json log_index, const char* amount) {
    return Json::dict({{"chain", chain}, {"tx_hash", tx}, {"log_index", log_index}, {"block", 1}, {"kind", "unlock"},
                       {"item", "chapter2"}, {"amount", amount}, {"token", "USDC"}, {"payer", Json()},
                       {"recipient", "R"}, {"source", "payment"}});
}

SCFG_TEST(ReceiptsTests, ReceiptKeyLowersOnlyEvmHashes) {
    SCFG_CHECK_EQ(t, receipts::receipt_key("ethereum", kHashUpper, 7), std::string("ethereum:") + kHashLower + ":7");
    SCFG_CHECK_EQ(t, receipts::receipt_key("ethereum", "0XAB", 0), "ethereum:0XAB:0");
    // Solana signatures are base58: case is meaning, so two spellings are two receipts.
    SCFG_CHECK_EQ(t, receipts::receipt_key("solana", kSig, 3), std::string("solana:") + kSig + ":3");
    SCFG_CHECK_EQ(t, receipts::receipt_key("solana", kSigLower, 3), std::string("solana:") + kSigLower + ":3");
    SCFG_CHECK(t, receipts::receipt_key("solana", kSig, 3) != receipts::receipt_key("solana", kSigLower, 3));
    SCFG_CHECK_EQ(t, receipts::receipt_key("base", "", 0), "base::0");
    SCFG_CHECK_EQ(t, receipts::receipt_key("c", "0x1", INT64_MIN), "c:0x1:-9223372036854775808");
    // The Json form agrees with the typed one.
    SCFG_CHECK_EQ(t, key("solana", kSig, 3), receipts::receipt_key("solana", kSig, 3));
    SCFG_CHECK_EQ(t, key("ethereum", kHashUpper, 7), receipts::receipt_key("ethereum", kHashUpper, 7));
}

SCFG_TEST(ReceiptsTests, StoredFieldsAreFormattedWithStr) {
    // Python's f-string: an int of any size exactly (the C# port raised OverflowException beyond int64).
    SCFG_CHECK_EQ(t, key("c", "0xAB", Json(BigInt(1) << 70)), "c:0xab:1180591620717411303424");
    SCFG_CHECK_EQ(t, key("c", "0xAB", Json(BigInt(1) << 63)), "c:0xab:9223372036854775808");
    SCFG_CHECK_EQ(t, key("c", "0xAB", "7"), "c:0xab:7");
    SCFG_CHECK_EQ(t, key("c", "0xAB", 7.0), "c:0xab:7.0");
    SCFG_CHECK_EQ(t, key("c", "0xAB", true), "c:0xab:True");
    SCFG_CHECK_EQ(t, key(Json(), "0xAB", Json()), "None:0xab:None");
    SCFG_CHECK_EQ(t, key("c", 5, 1), "AttributeError: 'int' object has no attribute 'startswith'");
    SCFG_CHECK_EQ(t, key("c", Json(), 1), "AttributeError: 'NoneType' object has no attribute 'startswith'");
}

SCFG_TEST(ReceiptsTests, MakeReceiptFields) {
    scfg::transfers::Transfer tr{"EPj", Json(), "7xK", BigInt(4990000), Json("5Sig"), Json(3), BigInt(99)};
    Result<Json> r = receipts::make_receipt("solana", tr, "unlock", "chapter2", "USDC", "payment");
    SCFG_CHECK_EQ(t, outcome(r), "ok");
    if (!r.ok()) return;
    SCFG_CHECK_EQ(t, scfg::json::dumps(r.value()),
                  "{\"chain\": \"solana\", \"tx_hash\": \"5Sig\", \"log_index\": 3, \"block\": 99, \"kind\": \"unlock\", "
                  "\"item\": \"chapter2\", \"amount\": \"4.990000\", \"token\": \"USDC\", \"payer\": null, "
                  "\"recipient\": \"7xK\", \"source\": \"payment\"}");
    // A TransferLog gives the same receipt as its Transfer.
    scfg::evm::TransferLog log{"T", "S", "R", BigInt(1230017), kHashLower, BigInt(7), BigInt(1234567), kHashLower};
    Result<Json> a = receipts::make_receipt("ethereum", log, "tip", Json(), "USDC", "restore");
    Result<Json> b = receipts::make_receipt("ethereum", log.to_transfer(), "tip", Json(), "USDC", "restore");
    SCFG_CHECK(t, a.ok() && b.ok() && scfg::json::dumps(a.value()) == scfg::json::dumps(b.value()));
    if (a.ok()) SCFG_CHECK_EQ(t, a.value().as_dict().get("amount").as_str(), "1.230017");
    // kind is checked before source.
    SCFG_CHECK_EQ(t, outcome(receipts::make_receipt("c", tr, "refund", Json(), "USDC", "payment")),
                  "ValueError: invalid kind 'refund'");
    SCFG_CHECK_EQ(t, outcome(receipts::make_receipt("c", tr, Json(), Json(), "USDC", "payment")),
                  "ValueError: invalid kind None");
    SCFG_CHECK_EQ(t, outcome(receipts::make_receipt("c", tr, "unlock", Json(), "USDC", Json())),
                  "ValueError: invalid source None");
    SCFG_CHECK_EQ(t, outcome(receipts::make_receipt("c", tr, "x", Json(), "USDC", "y")), "ValueError: invalid kind 'x'");
}

SCFG_TEST(ReceiptsTests, AddReceiptKeepsTheFirstOfAKey) {
    Json::Dict store;
    Json first = receipt("ethereum", kHashLower, 7, "4.990000");
    SCFG_CHECK_EQ(t, outcome(receipts::add_receipt(store, first)), "ok");
    std::string k = std::string("ethereum:") + kHashLower + ":7";
    SCFG_CHECK(t, store.size() == 1 && store.contains(k));
    // The same transfer again, its hash in capitals and another amount: not added, the first stays.
    Result<bool> again = receipts::add_receipt(store, receipt("ethereum", kHashUpper, 7, "9.000000"));
    SCFG_CHECK(t, again.ok() && !again.value());
    SCFG_CHECK_EQ(t, store.size(), 1);
    SCFG_CHECK(t, store.find(k) != nullptr && store.find(k)->same_object(first));
    // Another log index of the same transaction is another receipt.
    Result<bool> other = receipts::add_receipt(store, receipt("ethereum", kHashLower, 8, "4.990000"));
    SCFG_CHECK(t, other.ok() && other.value());
    // Solana signatures differing only in case are two receipts.
    SCFG_CHECK(t, receipts::add_receipt(store, receipt("solana", kSig, 3, "1")).value());
    SCFG_CHECK(t, receipts::add_receipt(store, receipt("solana", kSigLower, 3, "1")).value());
    SCFG_CHECK(t, !receipts::add_receipt(store, receipt("solana", kSig, 3, "2")).value());
    SCFG_CHECK_EQ(t, store.size(), 4);
    // A stored log_index beyond int64 keys exactly, as Python's.
    Json huge = receipt("ethereum", kHashLower, Json(BigInt(1) << 64), "1");
    SCFG_CHECK(t, receipts::add_receipt(store, huge).value());
    SCFG_CHECK(t, store.contains(std::string("ethereum:") + kHashLower + ":18446744073709551616"));
    // ...up to str()'s limit: more than 4300 digits is CPython 3.9's ValueError, decided before any conversion
    const std::string limit = "ValueError: Exceeds the limit (4300) for integer string conversion; use "
                              "sys.set_int_max_str_digits() to increase the limit";
    const BigInt digits4300 = BigInt::from_decimal(std::string(4300, '9')).take();
    Result<std::string> at = receipts::receipt_key("c", "0x1", digits4300);
    SCFG_CHECK(t, at.ok() && at.value() == "c:0x1:" + std::string(4300, '9'));
    Result<std::string> beyond = receipts::receipt_key("c", "0x1", digits4300 + BigInt(1));
    SCFG_CHECK_EQ(t, outcome(beyond), limit);
    SCFG_CHECK_EQ(t, outcome(receipts::receipt_key("c", "0x1", BigInt(1) << 16000)), limit);
    SCFG_CHECK_EQ(t, outcome(receipts::receipt_key(Json("c"), Json("0x1"), Json(BigInt(1) << 16000))), limit);
    SCFG_CHECK_EQ(t, outcome(receipts::add_receipt(store, receipt("ethereum", kHashLower, Json(digits4300 + BigInt(1)), "1"))),
                  limit);
}

SCFG_TEST(ReceiptsTests, AddReceiptOfAMalformedReceipt) {
    Json::Dict store;
    SCFG_CHECK_EQ(t, outcome(receipts::add_receipt(store, Json::list({1}))),
                  "TypeError: list indices must be integers or slices, not str");
    SCFG_CHECK_EQ(t, kind_of(receipts::add_receipt(store, "abc")), "TypeError");
    SCFG_CHECK_EQ(t, outcome(receipts::add_receipt(store, Json())), "TypeError: 'NoneType' object is not subscriptable");
    SCFG_CHECK_EQ(t, outcome(receipts::add_receipt(store, 5)), "TypeError: 'int' object is not subscriptable");
    SCFG_CHECK_EQ(t, outcome(receipts::add_receipt(store, Json::dict({{"chain", "c"}, {"tx_hash", "0x1"}}))),
                  "KeyError: 'log_index'");
    SCFG_CHECK_EQ(t, outcome(receipts::add_receipt(store, Json::dict({{"chain", "c"}, {"log_index", 1}}))),
                  "KeyError: 'tx_hash'");
    SCFG_CHECK_EQ(t, outcome(receipts::add_receipt(store, Json::dict({{"tx_hash", "0x1"}, {"log_index", 1}}))),
                  "KeyError: 'chain'");
    SCFG_CHECK_EQ(t, kind_of(receipts::add_receipt(store, Json::dict({{"chain", "c"}, {"tx_hash", 1}, {"log_index", 1}}))),
                  "AttributeError");
    SCFG_CHECK(t, store.empty());
}

SCFG_TEST(ReceiptsTests, RemoveReceipt) {
    Json::Dict store;
    SCFG_CHECK(t, receipts::add_receipt(store, receipt("ethereum", kHashLower, 7, "1")).value());
    receipts::remove_receipt(store, "ethereum:nothing:1");
    SCFG_CHECK_EQ(t, store.size(), 1);
    receipts::remove_receipt(store, std::string("ethereum:") + kHashLower + ":7");
    SCFG_CHECK(t, store.empty());
    receipts::remove_receipt(store, "");
    SCFG_CHECK(t, store.empty());
}

static std::string owned_text(const receipts::Ownership& o) {
    std::string s;
    for (const std::string& id : o.owned) s += (s.empty() ? "" : ",") + id;
    return s + " | " + o.tip_total.str();
}

static std::string ownership(const Json& store, const scfg::OrderedMap<scfg::models::Item>& items) {
    Result<receipts::Ownership> o = receipts::compute_ownership(store.as_dict(), items);
    return o.ok() ? owned_text(o.value()) : outcome(o);
}

SCFG_TEST(ReceiptsTests, OwnedIsSortedByCodePoint) {
    // UTF-16 order would put U+1F600 (D83D DE00) before U+FF5A.
    scfg::OrderedMap<scfg::models::Item> items;
    for (const char* id : {"z", "\xEF\xBD\x9A", "\xF0\x9F\x98\x80", "a", "\xC3\xA9"}) items.set(id, item(id, "1"));
    Json store = Json::dict();
    int n = 0;
    for (const char* id : {"z", "\xF0\x9F\x98\x80", "\xEF\xBD\x9A", "a", "\xC3\xA9", "a"}) {
        store.as_dict().set(std::to_string(n++), Json::dict({{"kind", "unlock"}, {"amount", "1"}, {"item", id}}));
    }
    SCFG_CHECK_EQ(t, ownership(store, items), "a,z,\xC3\xA9,\xEF\xBD\x9A,\xF0\x9F\x98\x80 | 0");
}

SCFG_TEST(ReceiptsTests, TipTotalIsTheDecimalSum) {
    scfg::OrderedMap<scfg::models::Item> items;
    SCFG_CHECK_EQ(t, ownership(Json::dict(), items), " | 0");
    SCFG_CHECK_EQ(t, ownership(Json::dict({{"a", Json::dict({{"kind", "tip"}, {"amount", "1.5"}})},
                                           {"b", Json::dict({{"kind", "tip"}, {"amount", "0"}})}}),
                               items),
                  " | 1.5");
    SCFG_CHECK_EQ(t, ownership(Json::dict({{"a", Json::dict({{"kind", "tip"}, {"amount", "0.500000"}})},
                                           {"b", Json::dict({{"kind", "tip"}, {"amount", "3"}})}}),
                               items),
                  " | 3.500000");
    // 28 digits of precision, as Python's default context.
    SCFG_CHECK_EQ(t, ownership(Json::dict({{"a", Json::dict({{"kind", "tip"}, {"amount", "1E-40"}})},
                                           {"b", Json::dict({{"kind", "tip"}, {"amount", "1E+40"}})}}),
                               items),
                  " | 1.000000000000000000000000000E+40");
}

SCFG_TEST(ReceiptsTests, MalformedEntriesAreSkipped) {
    scfg::OrderedMap<scfg::models::Item> items;
    items.set("z", item("z", "1"));
    Json store = Json::dict({
        // An unhashable item matters only to an unlock: this tip still counts.
        {"a", Json::dict({{"kind", "tip"}, {"amount", "1.5"}, {"item", Json::list({"x"})}})},
        {"b", Json::dict({{"kind", "unlock"}, {"amount", "9"}, {"item", Json::list({"z"})}})},
        {"c", Json::dict({{"kind", "tip"}, {"amount", "0"}})},
        {"d", Json::list({1})},
        {"e", "s"},
        {"f", Json()},
        {"g", Json::dict({{"kind", "tip"}, {"amount", "2.25"}, {"item", Json::dict({{"k", 1}})}})},
        {"h", Json::dict({{"kind", "unlock"}, {"amount", "0.99"}, {"item", "z"}})},  // under min_price
        {"i", Json::dict({{"kind", "unlock"}, {"amount", "abc"}, {"item", "z"}})},
        {"j", Json::dict({{"kind", "unlock"}, {"item", "z"}})},
        {"k", Json::dict({{"amount", "5"}, {"item", "z"}})},
        {"l", Json::dict({{"kind", "unlock"}, {"amount", "5"}, {"item", 1}})},
        {"m", Json::dict({{"kind", "tip"}, {"amount", Json::list({0, Json::list({1}), 0})}})},
    });
    SCFG_CHECK_EQ(t, ownership(store, items), " | 3.75");
    // The item is owned once its amount reaches min_price; the item id comes from the Item.
    store.as_dict().set("n", Json::dict({{"kind", "unlock"}, {"amount", "1.000000"}, {"item", "z"}}));
    SCFG_CHECK_EQ(t, ownership(store, items), "z | 3.75");
}
