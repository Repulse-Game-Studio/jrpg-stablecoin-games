// No Python twin: tests of scfg::evm and scfg::transfers beyond tier1/evm_address.json: Python's error classes and
// messages for malformed logs (what scfg_core.evm raises, python3 -c ...; 3.9's wording where 3.12's differs), the
// int(x, 16) the log index and block number go through (unbounded, as Python's int), and the accepted str.lower()
// divergence on non-ASCII text.
#include "support/Test.h"

#include "Scfg/BigInt.h"
#include "Scfg/Bytes.h"
#include "Scfg/Evm.h"
#include "Scfg/Json.h"
#include "Scfg/Keccak.h"
#include "Scfg/Py.h"
#include "Scfg/Transfers.h"

#include <chrono>
#include <optional>
#include <string>

using scfg::BigInt;
using scfg::Json;
using scfg::Result;
namespace evm = scfg::evm;
namespace py = scfg::py;

static const char* const kA = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed";
static const char* const kB = "0xfB6916095ca1df60bB79Ce92cE3Ea74c37c5d359";

static std::string shown(const Result<std::string>& r) { return r.ok() ? r.value() : r.err().kind + ": " + r.err().message; }

// tier1.py's _tlog: a valid USDC Transfer log, with kw's keys replaced (a null Json value deletes the key).
static Json tlog(std::initializer_list<std::pair<std::string, std::optional<Json>>> kw = {}) {
    Json log = Json::dict({
        {"address", "0xa0b86991c6218b36c1d19d4a2e9eb0ce3606eb48"},
        {"topics", Json::list({std::string(evm::TRANSFER_TOPIC), evm::address_topic(kA).value(),
                               evm::address_topic(kB).value()})},
        {"data", "0x00000000000000000000000000000000000000000000000000000000004c2430"},
        {"transactionHash", "0x" + std::string(64, 'A')},
        {"logIndex", "0x7"},
        {"blockNumber", "0x12d687"},
        {"blockHash", "0x" + std::string(64, 'C')},
        {"removed", false},
    });
    for (const auto& [k, v] : kw) {
        if (v) {
            log.as_dict().set(k, *v);
        } else {
            log.as_dict().erase(k);
        }
    }
    return log;
}

// "None", "<log_index> <block_number> <tx_hash> <block_hash>", or "<Kind>: <message>".
static std::string parsed(const Json& log) {
    Result<std::optional<evm::TransferLog>> r = evm::parse_transfer_log(log);
    if (!r.ok()) return r.err().kind + ": " + r.err().message;
    if (!r.value()) return "None";
    const evm::TransferLog& t = *r.value();
    // a hostile index of a million digits is not converted to decimal here (that is quadratic)
    auto text = [](const BigInt& n) { return n.fits_int64() || n.abs() < (BigInt(1) << 256) ? n.to_decimal() : "<huge>"; };
    return text(t.log_index) + " " + text(t.block_number) + " " + t.tx_hash + " " + t.block_hash;
}

static Json topics_with(size_t at, Json v) {
    Json topics = tlog().as_dict().get("topics");
    Json copy = scfg::deep_copy(topics);
    copy.as_list()[at] = std::move(v);
    return copy;
}

SCFG_TEST(EvmTests, TransferTopicIsTheEventSignatureHash) {
    SCFG_CHECK_EQ(t, evm::TRANSFER_TOPIC,
                  "0x" + scfg::bytes::to_hex(scfg::keccak::keccak256(std::string_view("Transfer(address,address,uint256)"))));
}

SCFG_TEST(EvmTests, AddressErrorsAreValueErrors) {
    SCFG_CHECK_EQ(t, shown(evm::to_checksum_address(5)), "ValueError: not a 20-byte hex address: 5");
    SCFG_CHECK_EQ(t, shown(evm::to_checksum_address(Json())), "ValueError: not a 20-byte hex address: None");
    SCFG_CHECK_EQ(t, shown(evm::to_checksum_address(Json::list({1}))), "ValueError: not a 20-byte hex address: [1]");
    SCFG_CHECK_EQ(t, shown(evm::address_topic("0x1234")), "ValueError: not a 20-byte hex address: '0x1234'");
    SCFG_CHECK_EQ(t, shown(evm::normalize_address(std::string(kA) + "\n")),
                  "ValueError: not a 20-byte hex address: '0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed\\n'");
    std::string e42;
    for (int i = 0; i < 42; ++i) e42 += "\xC3\xA9";  // 42 code points, 84 bytes
    SCFG_CHECK_EQ(t, shown(evm::to_checksum_address(e42)), "ValueError: not a 20-byte hex address: '" + e42 + "'");
    // the str form of the address, not its repr
    SCFG_CHECK_EQ(t, shown(evm::normalize_address("0x5aaeb6053F3E94C9b9A09f33669435E7Ef1BeAed")),
                  "ValueError: bad EIP-55 checksum: 0x5aaeb6053F3E94C9b9A09f33669435E7Ef1BeAed");
    SCFG_CHECK(t, !evm::is_checksum_address(5));
    SCFG_CHECK(t, !evm::is_checksum_address(Json()));
    SCFG_CHECK(t, !evm::is_checksum_address(""));
    SCFG_CHECK(t, evm::is_checksum_address(kA));
    SCFG_CHECK_EQ(t, shown(evm::to_checksum_address("0X" + std::string(kA).substr(2))),
                  "ValueError: not a 20-byte hex address: '0X5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed'");
}

SCFG_TEST(EvmTests, ParsesAValidLog) {
    SCFG_CHECK_EQ(t, parsed(tlog()), "7 1234567 0x" + std::string(64, 'a') + " 0x" + std::string(64, 'c'));
    scfg::Result<std::optional<evm::TransferLog>> r = evm::parse_transfer_log(tlog());
    if (!SCFG_CHECK(t, r.ok() && r.value().has_value())) return;
    const evm::TransferLog& log = *r.value();
    SCFG_CHECK_EQ(t, log.token, "0xA0b86991c6218b36c1d19D4a2e9Eb0cE3606eB48");
    SCFG_CHECK_EQ(t, log.sender, kA);
    SCFG_CHECK_EQ(t, log.recipient, kB);
    SCFG_CHECK(t, log.value == BigInt(4990000));
    scfg::transfers::Transfer tr = log.to_transfer();
    SCFG_CHECK(t, tr == (scfg::transfers::Transfer{log.token, Json(log.sender), log.recipient, BigInt(4990000),
                                                    Json(log.tx_hash), Json(7), BigInt(1234567)}));
    SCFG_CHECK(t, tr.sender.is_str() && tr.sender.as_str() == kA);
    SCFG_CHECK(t, !(tr == scfg::transfers::Transfer{log.token, Json(), log.recipient, BigInt(4990000),
                                                      Json(log.tx_hash), Json(7), BigInt(1234567)}));
    // the dataclass's ==: Python's == on the carried values (7 == 7.0 == ..., "7" is not 7)
    SCFG_CHECK(t, tr == (scfg::transfers::Transfer{log.token, Json(log.sender), log.recipient, BigInt(4990000),
                                                    Json(log.tx_hash), Json(7.0), BigInt(1234567)}));
    SCFG_CHECK(t, !(tr == scfg::transfers::Transfer{log.token, Json(log.sender), log.recipient, BigInt(4990000),
                                                      Json(log.tx_hash), Json("7"), BigInt(1234567)}));
    SCFG_CHECK(t, *r.value() == *evm::parse_transfer_log(tlog()).value());
    // topic0 is compared after str.lower(): an all-uppercase one (0X prefix included) is still the Transfer topic
    std::string upper = "0XDDF252AD1BE2C89B69C2B068FC378DAA952BA7F163C4A11628F55A4DF523B3EF";
    SCFG_CHECK_EQ(t, parsed(tlog({{"topics", topics_with(0, upper)}})), parsed(tlog()));
}

SCFG_TEST(EvmTests, LogIndexAndBlockNumberAreIntBase16) {
    // int(x, 16): surrounding whitespace, a sign, a 0x prefix, single underscores, any script's decimal digits
    SCFG_CHECK_EQ(t, parsed(tlog({{"logIndex", " 0x_7 "}, {"blockNumber", "\xD9\xA3"}})),
                  "7 3 0x" + std::string(64, 'a') + " 0x" + std::string(64, 'c'));
    SCFG_CHECK_EQ(t, parsed(tlog({{"logIndex", "-0x1"}})).substr(0, 11), "-1 1234567 ");
    SCFG_CHECK_EQ(t, parsed(tlog({{"logIndex", "0x" + std::string(1000, '0') + "7"}})).substr(0, 10), "7 1234567 ");
    SCFG_CHECK_EQ(t, parsed(tlog({{"logIndex", "0x7fffffffffffffff"}})).substr(0, 20), "9223372036854775807 ");
    SCFG_CHECK_EQ(t, parsed(tlog({{"logIndex", "-0x8000000000000000"}})).substr(0, 21), "-9223372036854775808 ");
    SCFG_CHECK_EQ(t, parsed(tlog({{"logIndex", "0x"}})), "ValueError: invalid literal for int() with base 16: '0x'");
    SCFG_CHECK_EQ(t, parsed(tlog({{"logIndex", 7}})), "TypeError: int() can't convert non-string with explicit base");
    SCFG_CHECK_EQ(t, parsed(tlog({{"blockNumber", Json()}})), "TypeError: int() can't convert non-string with explicit base");
    SCFG_CHECK_EQ(t, parsed(tlog({{"logIndex", std::nullopt}})), "KeyError: 'logIndex'");
}

SCFG_TEST(EvmTests, AnIndexOrBlockBeyondInt64IsKeptExactly) {
    // Python's int(x, 16) is unbounded, and so is the TransferLog here: a stored receipt with such an index is
    // compared with it exactly (reverify), and keyed exactly (receipt_key).
    SCFG_CHECK_EQ(t, parsed(tlog({{"logIndex", "0x8000000000000000"}})).substr(0, 28), "9223372036854775808 1234567 ");
    SCFG_CHECK_EQ(t, parsed(tlog({{"blockNumber", "-0x8000000000000001"}})).substr(0, 23), "7 -9223372036854775809 ");
    SCFG_CHECK_EQ(t, parsed(tlog({{"logIndex", "0x10000000000000000"}})).substr(0, 21), "18446744073709551616 ");
    SCFG_CHECK_EQ(t, parsed(tlog({{"logIndex", "0x8000000000000000"}, {"blockNumber", "zz"}})),
                  "ValueError: invalid literal for int() with base 16: 'zz'");
    SCFG_CHECK_EQ(t, parsed(tlog({{"logIndex", "0x" + std::string(100, 'f')}, {"blockHash", std::nullopt}})),
                  "KeyError: 'blockHash'");
    // hostile lengths stay linear (base 16 needs no decimal conversion)
    for (const std::string& huge : {"0x" + std::string(400000, 'f'), std::string(400000, '1') + "_",
                                    "0x" + std::string(400000, '0') + "1"}) {
        auto start = std::chrono::steady_clock::now();
        std::string got = parsed(tlog({{"logIndex", huge}}));
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        t.check(got.rfind("<huge> ", 0) == 0 || got.rfind("ValueError: invalid literal", 0) == 0 || got.rfind("1 ", 0) == 0,
                got.substr(0, 80));
        t.check(ms < 1000, "parse_transfer_log took " + std::to_string(ms) + " ms");
    }
}

SCFG_TEST(EvmTests, MalformedLogsRaiseWhatPythonRaises) {
    SCFG_CHECK_EQ(t, parsed(Json::list({1})), "AttributeError: 'list' object has no attribute 'get'");
    SCFG_CHECK_EQ(t, parsed(Json()), "AttributeError: 'NoneType' object has no attribute 'get'");
    SCFG_CHECK_EQ(t, parsed(tlog({{"topics", 5}})), "TypeError: object of type 'int' has no len()");
    SCFG_CHECK_EQ(t, parsed(tlog({{"topics", true}})), "TypeError: object of type 'bool' has no len()");
    SCFG_CHECK_EQ(t, parsed(tlog({{"topics", 5}, {"removed", 1}})), "None");  // removed is checked before len()
    SCFG_CHECK_EQ(t, parsed(tlog({{"topics", 0}})), "None");                  // falsy: no topics
    SCFG_CHECK_EQ(t, parsed(tlog({{"topics", "abc"}})), "None");              // 'a'.lower() is not the topic
    SCFG_CHECK_EQ(t, parsed(tlog({{"topics", Json::dict({{"a", 1}, {"b", 2}, {"c", 3}})}})), "KeyError: 0");
    SCFG_CHECK_EQ(t, parsed(tlog({{"topics", Json::list({5, 1, 2})}})),
                  "AttributeError: 'int' object has no attribute 'lower'");
    SCFG_CHECK_EQ(t, parsed(tlog({{"topics", topics_with(1, 5)}})), "TypeError: 'int' object is not subscriptable");
    SCFG_CHECK_EQ(t, parsed(tlog({{"topics", topics_with(1, Json())}})),
                  "TypeError: 'NoneType' object is not subscriptable");
    SCFG_CHECK_EQ(t, parsed(tlog({{"topics", topics_with(1, Json::list())}})),
                  "TypeError: can only concatenate str (not \"list\") to str");
    SCFG_CHECK_EQ(t, parsed(tlog({{"topics", topics_with(1, Json::dict())}})), "TypeError: unhashable type: 'slice'");
    SCFG_CHECK_EQ(t, parsed(tlog({{"topics", topics_with(2, "0x12")}})),
                  "ValueError: not a 20-byte hex address: '0x0x12'");
    // topic[-40:] counts code points
    std::string e_then_a = "\xC3\xA9" + std::string(39, 'a');
    SCFG_CHECK_EQ(t, parsed(tlog({{"topics", topics_with(1, "0x00" + e_then_a)}})),
                  "ValueError: not a 20-byte hex address: '0x" + e_then_a + "'");
    SCFG_CHECK_EQ(t, parsed(tlog({{"transactionHash", 5}})), "AttributeError: 'int' object has no attribute 'lower'");
    SCFG_CHECK_EQ(t, parsed(tlog({{"blockHash", Json()}})),
                  "AttributeError: 'NoneType' object has no attribute 'lower'");
    SCFG_CHECK_EQ(t, parsed(tlog({{"address", std::nullopt}})), "KeyError: 'address'");
    SCFG_CHECK_EQ(t, parsed(tlog({{"address", "0x1234"}})), "ValueError: not a 20-byte hex address: '0x1234'");
}

SCFG_TEST(EvmTests, HashesAreLoweredForAsciiOnly) {
    // Accepted divergence: str.lower() is applied as py::lower_for_ascii (A-Z, and the Kelvin sign and U+0130, whose
    // lowercase holds an ASCII letter), so a hash's ASCII form and any comparison with ASCII text are Python's;
    // other non-ASCII letters keep their case (Python gives '0x\xe9kk' here).
    std::string got = parsed(tlog({{"transactionHash", "0x\xC3\x89K\xE2\x84\xAA"}}));
    SCFG_CHECK_EQ(t, got.substr(0, got.find(" 0xcc")), "7 1234567 0x\xC3\x89kk");
}
