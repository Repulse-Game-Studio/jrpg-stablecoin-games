// Copyright 2026 Repulse
// No Python twin: tests of scfg::chains, scfg::config and scfg::payments beyond the tier1 configuration vectors:
// what the C# port recorded as divergences and this port does as Python does (2**70 confirmations, the
// AttributeError of a non-dict methods/rpc_urls/confirmations, a tip's non-str title and item, {!a} as ascii(), a
// Chain that owns its rpc list), Python's iteration and hashing of loosely typed arguments, the keyword-argument
// TypeErrors, the mutable registries, Token's normalization, parse_transfer's Arc floor, text_on and the accepted
// explorer_tx divergence. Every expectation is scfg_core's own output (python3 with plugin/game/python-packages).
#include "support/Test.h"

#include "Scfg/BigInt.h"
#include "Scfg/Chains.h"
#include "Scfg/Config.h"
#include "Scfg/Dec.h"
#include "Scfg/Evm.h"
#include "Scfg/Json.h"
#include "Scfg/Models.h"
#include "Scfg/Payments.h"
#include "Scfg/Py.h"

#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using scfg::BigInt;
using scfg::Json;
using scfg::Result;
namespace chains = scfg::chains;
namespace config = scfg::config;
namespace payments = scfg::payments;
namespace py = scfg::py;

static const char* const EVM = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed";
static const char* const SOL = "9WzDXwBbmkg8ZTbNMqUxvQRAyrZzDsGYdLVL9zYtAWWM";

// "ok", or "<Kind>[(<code>)]: <message>".
template <class T>
static std::string outcome(const Result<T>& r) {
    if (r.ok()) return "ok";
    const scfg::Err& e = r.err();
    return e.kind + (e.code.empty() ? "" : "(" + e.code + ")") + ": " + e.message;
}

// {"wallet": EVM, key: value}.
static Json evm_kw(std::string key, Json value) {
    Json kw = Json::dict({{"wallet", EVM}});
    kw.as_dict().set(std::move(key), std::move(value));
    return kw;
}

static std::string cfg(std::string key, Json value) { return outcome(config::build_config(evm_kw(std::move(key), value))); }

// tier1_config.py's CHAIN, with key replaced.
static Json chain_kw(std::string key = "", Json value = Json()) {
    Json kw = Json::dict({{"key", "avalanche"},
                          {"name", "Avalanche"},
                          {"chain_id", 43114},
                          {"rpc", "https://api.avax.network/ext/bc/C/rpc"},
                          {"tokens", Json::dict({{"USDC", "0xb97ef9ef8734c71904d8002f8b6bc66dd9c48a6e"}})},
                          {"confirmations", 5},
                          {"explorer_tx", "https://snowtrace.io/tx/{}"}});
    if (!key.empty()) kw.as_dict().set(std::move(key), std::move(value));
    return kw;
}

static std::string strings(const std::vector<std::string>& xs) {
    std::string out;
    for (const std::string& x : xs) out += (out.empty() ? "" : " ") + x;
    return out;
}

// A Transfer log of `value` from `address` (topics and hashes as tier1.py's _tlog).
static Json transfer_log(std::string address, const BigInt& value) {
    std::string hex = value.to_hex();
    return Json::dict({{"address", std::move(address)},
                       {"topics", Json::list({std::string(scfg::evm::TRANSFER_TOPIC),
                                              scfg::evm::address_topic(EVM).value(),
                                              scfg::evm::address_topic("0xfB6916095ca1df60bB79Ce92cE3Ea74c37c5d359").value()})},
                       {"data", "0x" + std::string(64 - hex.size(), '0') + hex},
                       {"transactionHash", "0x" + std::string(64, 'a')},
                       {"logIndex", "0x1"},
                       {"blockNumber", "0x10"},
                       {"blockHash", "0x" + std::string(64, 'c')}});
}

// ---- chains ----

SCFG_TEST(ChainsTests, RegistriesAreBuiltInPythonsOrder) {
    SCFG_CHECK_EQ(t, strings(chains::MAINNET().keys()), "ethereum arbitrum polygon base arc solana");
    SCFG_CHECK_EQ(t, strings(chains::TESTNET().keys()), "ethereum arbitrum polygon base arc solana");
    SCFG_CHECK(t, &chains::get_chains(false) == &chains::MAINNET());
    SCFG_CHECK(t, &chains::get_chains(true) == &chains::TESTNET());
    const chains::Chain& arc = *chains::MAINNET().find("arc");
    const chains::Token& usdc = *arc.tokens.find("USDC");
    SCFG_CHECK_EQ(t, usdc.address, std::string(chains::ARC_USDC));
    SCFG_CHECK_EQ(t, usdc.log_address, scfg::evm::to_checksum_address(std::string(chains::ARC_SYSTEM_EMITTER)).value());
    SCFG_CHECK_EQ(t, usdc.log_source(), usdc.log_address);
    SCFG_CHECK_EQ(t, usdc.log_decimals, 18);
    SCFG_CHECK_EQ(t, arc.min_fee_wei, chains::ARC_MIN_FEE_WEI);
    SCFG_CHECK_EQ(t, chains::TESTNET().find("solana")->wallet_chain, "solana:devnet");
}

SCFG_TEST(ChainsTests, TheRegistriesAreMutableForTheDriver) {
    // monkeypatch.setitem(MAINNET, "arbitrum", replace(MAINNET["arbitrum"], rpc=(url,), confirmations=3)).
    chains::Chain saved = *chains::MAINNET().find("arbitrum");
    chains::MAINNET().set("arbitrum", saved.with(std::vector<std::string>{"http://127.0.0.1:8545"}, BigInt(3)));
    Result<config::Config> c = config::build_config(Json::dict({{"wallet", EVM}, {"chains", Json::list({"arbitrum"})}}));
    SCFG_CHECK_EQ(t, outcome(c), "ok");
    if (c.ok()) {
        Result<scfg::OrderedMap<chains::Chain>> enabled = config::enabled_chains(c.value());
        SCFG_CHECK_EQ(t, strings(enabled.value().find("arbitrum")->rpc), "http://127.0.0.1:8545");
        SCFG_CHECK(t, enabled.value().find("arbitrum")->confirmations == BigInt(3));
    }
    chains::MAINNET().set("arbitrum", saved);
    SCFG_CHECK_EQ(t, strings(chains::MAINNET().keys()), "ethereum arbitrum polygon base arc solana");
    SCFG_CHECK(t, chains::MAINNET().find("arbitrum")->confirmations == BigInt(20));
}

SCFG_TEST(ChainsTests, TokenNormalisesAsPostInit) {
    SCFG_CHECK_EQ(t, chains::Token::make("0xa0b86991c6218b36c1d19d4a2e9eb0ce3606eb48").value().address,
                  "0xA0b86991c6218b36c1d19D4a2e9Eb0cE3606eB48");
    SCFG_CHECK_EQ(t, chains::Token::make("  EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v ").value().address,
                  "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v");
    // "0X" is not "0x": the text goes to normalize_pubkey.
    SCFG_CHECK_EQ(t, outcome(chains::Token::make("0X3600000000000000000000000000000000000000")),
                  "ValueError: invalid base58 character '0'");
    SCFG_CHECK_EQ(t, outcome(chains::Token::make(chains::ARC_USDC, 6, "0x" + std::string(40, '1'), 5)),
                  "ValueError: log_decimals must be at least decimals");
    chains::Token plain = chains::Token::make(chains::ARC_USDC).value();
    SCFG_CHECK_EQ(t, plain.log_source(), plain.address);
}

SCFG_TEST(ChainsTests, ParseTransferReadsOnlyTheLogSource) {
    const chains::Chain& arc = *chains::MAINNET().find("arc");
    std::string emitter = py::lower_ascii(chains::ARC_SYSTEM_EMITTER);
    // 4993742 * 10**12 + 999 wei of the 18-decimal stream floors to 4993742 units, under the 0x3600 address.
    BigInt wei = BigInt(4993742) * BigInt(10).pow(12) + BigInt(999);
    auto parsed = arc.parse_transfer(transfer_log(emitter, wei)).value();
    SCFG_CHECK(t, parsed.has_value());
    if (parsed) {
        SCFG_CHECK_EQ(t, parsed->token, std::string(chains::ARC_USDC));
        SCFG_CHECK_EQ(t, parsed->value.to_decimal(), "4993742");
    }
    // Below one unit floors to 0: not a payment. The 6-decimal interface's own log is never read.
    SCFG_CHECK(t, !arc.parse_transfer(transfer_log(emitter, BigInt(10).pow(12) - BigInt(1))).value().has_value());
    SCFG_CHECK(t, !arc.parse_transfer(transfer_log(std::string(chains::ARC_USDC), BigInt(4993742))).value().has_value());
    const chains::Chain& eth = *chains::MAINNET().find("ethereum");
    auto usdc = eth.parse_transfer(transfer_log("0xa0b86991c6218b36c1d19d4a2e9eb0ce3606eb48", BigInt(5))).value();
    SCFG_CHECK(t, usdc.has_value() && usdc->token == "0xA0b86991c6218b36c1d19D4a2e9Eb0cE3606eB48" &&
                      usdc->value == BigInt(5));
    SCFG_CHECK(t, !eth.parse_transfer(transfer_log("0x" + std::string(40, '1'), BigInt(5))).value().has_value());
    SCFG_CHECK_EQ(t, outcome(eth.parse_transfer(Json::list())), "AttributeError: 'list' object has no attribute 'get'");
}

SCFG_TEST(ChainsTests, AChainOwnsItsRpcListAndExplorerUsesAscii) {
    Json rpc = Json::list({"https://a.example.org"});
    chains::Chain c = config::build_chain(chain_kw("rpc", rpc)).value();
    rpc.as_list().emplace_back("https://b.example.org");  // the caller's list changes; the chain's does not
    SCFG_CHECK_EQ(t, strings(c.rpc), "https://a.example.org");
    chains::Chain copy = c;
    copy.rpc.push_back("https://c.example.org");
    SCFG_CHECK_EQ(t, strings(c.rpc), "https://a.example.org");
    // {!a} is ascii(), not repr(): only reachable beside an escaped {{}}, since a second auto field is index 1.
    chains::Chain a = config::build_chain(chain_kw("explorer_tx", "https://x/{{}}{!a}")).value();
    SCFG_CHECK_EQ(t, a.explorer_url("\xC3\xA9").value(), "https://x/{}'\\xe9'");
    SCFG_CHECK_EQ(t, outcome(config::build_chain(chain_kw("explorer_tx", "https://x/{}?{!a}"))),
                  "ConfigError(chain.bad_explorer_tx_braces): chain 'avalanche': explorer_tx may contain no braces "
                  "besides {}");
    SCFG_CHECK_EQ(t, chains::MAINNET().find("base")->explorer_url("0xab").value(), "https://basescan.org/tx/0xab");
}

// ---- build_chain ----

SCFG_TEST(ConfigTests, BuildChainIntsAreUnbounded) {
    BigInt big = BigInt(1) << 70;
    chains::Chain c = config::build_chain(chain_kw("chain_id", Json(big))).value();
    SCFG_CHECK_EQ(t, c.chain_id.to_decimal(), "1180591620717411303424");
    SCFG_CHECK_EQ(t, config::build_chain(chain_kw("max_log_range", Json(big))).value().max_log_range.to_decimal(),
                  "1180591620717411303424");
    SCFG_CHECK_EQ(t, config::build_chain(chain_kw("confirmations", Json(big))).value().confirmations.to_decimal(),
                  "1180591620717411303424");
    SCFG_CHECK_EQ(t, outcome(config::build_chain(chain_kw("max_log_range", 9))),
                  "ConfigError(int.below_minimum): chain 'avalanche': max_log_range must be an int >= 10");
    SCFG_CHECK_EQ(t, outcome(config::build_chain(chain_kw("chain_id", 1.0))),
                  "ConfigError(int.below_minimum): chain 'avalanche': chain_id must be an int >= 1");
}

SCFG_TEST(ConfigTests, KeywordArgumentsBindAsPython) {
    SCFG_CHECK_EQ(t, outcome(config::build_chain(chain_kw("colour", 1))),
                  "TypeError: build_chain() got an unexpected keyword argument 'colour'");
    SCFG_CHECK_EQ(t, outcome(config::build_chain(Json::dict({{"key", "k"}}))),
                  "TypeError: build_chain() missing 6 required keyword-only arguments: 'name', 'chain_id', 'rpc', "
                  "'tokens', 'confirmations', and 'explorer_tx'");
    Json two = chain_kw();
    two.as_dict().erase("name");
    two.as_dict().erase("rpc");
    SCFG_CHECK_EQ(t, outcome(config::build_chain(two)),
                  "TypeError: build_chain() missing 2 required keyword-only arguments: 'name' and 'rpc'");
    Json no_key = chain_kw();
    no_key.as_dict().erase("key");
    SCFG_CHECK_EQ(t, outcome(config::build_chain(no_key)),
                  "TypeError: build_chain() missing 1 required positional argument: 'key'");
    SCFG_CHECK_EQ(t, cfg("colour", 1), "TypeError: build_config() got an unexpected keyword argument 'colour'");
    SCFG_CHECK_EQ(t, outcome(config::build_config(Json::list())),
                  "TypeError: build_config() argument after ** must be a mapping, not list");
    config::Config c = config::build_config(Json::dict({{"wallet", EVM}})).value();
    SCFG_CHECK_EQ(t, outcome(config::add_item(c, "b", Json::dict({{"code", 7}}))),
                  "TypeError: add_item() missing 3 required keyword-only arguments: 'price', 'token', and 'title'");
    std::vector<config::CustomChain> none;
    SCFG_CHECK_EQ(t, outcome(config::build_config(evm_kw("custom_chains", Json::list()), none)),
                  "TypeError: build_config() got multiple values for keyword argument 'custom_chains'");
}

SCFG_TEST(ConfigTests, ExplorerTemplatesPythonFormatsAreAPortLimit) {
    // Python accepts a format spec beside an escaped {{}} ('https://x/{}    \xe9') and raises AttributeError for
    // {.x}; py::format1 refuses both (Config.h's port limit).
    SCFG_CHECK_EQ(t, outcome(config::build_chain(chain_kw("explorer_tx", "https://x/{{}}{:>5}"))),
                  "ConfigError(chain.bad_explorer_tx_braces): chain 'avalanche': explorer_tx may contain no braces "
                  "besides {}");
    SCFG_CHECK_EQ(t, outcome(config::build_chain(chain_kw("explorer_tx", "https://x/{{}}{.x}"))),
                  "ConfigError(chain.bad_explorer_tx_braces): chain 'avalanche': explorer_tx may contain no braces "
                  "besides {}");
}

// ---- build_config ----

SCFG_TEST(ConfigTests, HugeConfirmationsAreAcceptedAsInPython) {
    BigInt big = BigInt(1) << 70;
    Result<config::Config> c = config::build_config(evm_kw("confirmations", Json::dict({{"base", Json(big)}})));
    SCFG_CHECK_EQ(t, outcome(c), "ok");
    if (!c.ok()) return;
    SCFG_CHECK(t, *c.value().confirmations.find("base") == big);
    SCFG_CHECK(t, config::enabled_chains(c.value()).value().find("base")->confirmations == big);
    // And no_usable_token still decides when nothing is usable.
    Json kw = evm_kw("confirmations", Json::dict({{"base", Json(big)}}));
    kw.as_dict().set("chains", Json::list({"base"}));
    kw.as_dict().set("tokens", Json::list({"USDT"}));
    SCFG_CHECK_EQ(t, outcome(config::build_config(kw)),
                  "ConfigError(config.no_usable_token): no enabled chain supports any enabled token");
}

SCFG_TEST(ConfigTests, NonDictOptionsAreAttributeErrors) {
    for (const char* key : {"methods", "rpc_urls", "confirmations"}) {
        SCFG_CHECK_EQ(t, cfg(key, Json::list({1})), "AttributeError: 'list' object has no attribute 'items'");
        SCFG_CHECK_EQ(t, cfg(key, "x"), "AttributeError: 'str' object has no attribute 'items'");
        SCFG_CHECK_EQ(t, cfg(key, 5), "AttributeError: 'int' object has no attribute 'items'");
        SCFG_CHECK_EQ(t, cfg(key, true), "AttributeError: 'bool' object has no attribute 'items'");
        // `x or {}`: a falsy non-dict is no options at all.
        SCFG_CHECK_EQ(t, cfg(key, Json::list()), "ok");
        SCFG_CHECK_EQ(t, cfg(key, 0), "ok");
        SCFG_CHECK_EQ(t, cfg(key, ""), "ok");
    }
    SCFG_CHECK_EQ(t, cfg("methods", Json::dict({{"qr", Json()}})),
                  "AttributeError: 'NoneType' object has no attribute 'items'");
    SCFG_CHECK_EQ(t, cfg("methods", Json::dict({{"qr", Json::list()}})),
                  "AttributeError: 'list' object has no attribute 'items'");
    SCFG_CHECK_EQ(t, cfg("methods", Json::dict({{"qr", Json::dict()}})), "ok");
}

SCFG_TEST(ConfigTests, ArgumentsIterateAndHashAsPython) {
    SCFG_CHECK_EQ(t, cfg("chains", "base"), "ConfigError(chains.unknown): unknown chain: 'b'");
    SCFG_CHECK_EQ(t, cfg("chains", Json::list({Json::list({"base"})})), "TypeError: unhashable type: 'list'");
    SCFG_CHECK_EQ(t, cfg("chains", Json::list({Json::dict()})), "TypeError: unhashable type: 'dict'");
    SCFG_CHECK_EQ(t, cfg("chains", 5), "TypeError: 'int' object is not iterable");
    SCFG_CHECK_EQ(t, cfg("chains", Json::list({1.5})), "ConfigError(chains.unknown): unknown chain: 1.5");
    SCFG_CHECK_EQ(t, cfg("chains", Json::list({Json()})), "ConfigError(chains.unknown): unknown chain: None");
    SCFG_CHECK_EQ(t, strings(config::build_config(evm_kw("chains", Json::dict({{"base", 1}, {"arc", 2}}))).value().chains),
                  "base arc");
    SCFG_CHECK_EQ(t, cfg("tokens", "USDC"), "ConfigError(tokens.unknown): unknown token: 'U'");
    SCFG_CHECK_EQ(t, strings(config::build_config(evm_kw("tokens", Json::dict({{"USDT", 1}}))).value().tokens), "USDT");
    SCFG_CHECK_EQ(t, cfg("custom_chains", 5), "TypeError: 'int' object is not iterable");
    SCFG_CHECK_EQ(t, cfg("custom_chains", "ab"),
                  "ConfigError(chain.not_from_add_chain): custom chains must be created with scfg.add_chain()");
    SCFG_CHECK_EQ(t, cfg("previous_wallets", 5), "TypeError: 'int' object is not iterable");
    SCFG_CHECK_EQ(t, cfg("previous_wallets", "ab"),
                  "ConfigError(solana_address.invalid): previous_wallets: not a Solana address: 'a'");
    SCFG_CHECK_EQ(t, cfg("methods", Json::dict({{"qr", Json::dict({{"chains", "base"}})}})),
                  "ConfigError(methods.qr_disabled_chain): methods.qr.chains contains disabled chain 'b'");
    SCFG_CHECK_EQ(t, cfg("methods", Json::dict({{"qr", Json::dict({{"chains", 5}})}})),
                  "TypeError: 'int' object is not iterable");
    SCFG_CHECK_EQ(t, cfg("methods", Json::dict({{"qr", Json::dict({{"chains", Json::list()}})}})), "ok");
}

SCFG_TEST(ConfigTests, NumbersAndUrls) {
    auto timeout = [](Json v) { return cfg("methods", Json::dict({{"browser", Json::dict({{"timeout_min", v}})}})); };
    SCFG_CHECK_EQ(t, timeout(std::numeric_limits<double>::infinity()), "ok");
    SCFG_CHECK_EQ(t, timeout(0.5), "ok");
    SCFG_CHECK_EQ(t, timeout(std::nan("")),
                  "ConfigError(methods.bad_timeout): methods.browser.timeout_min must be a number of minutes above 0");
    SCFG_CHECK_EQ(t, cfg("confirmations", Json::dict({{"base", 3.0}})),
                  "ConfigError(confirmations.invalid): confirmations must be an int >= 1");
    SCFG_CHECK_EQ(t, cfg("confirmations", Json::dict({{"base", -3}})),
                  "ConfigError(confirmations.invalid): confirmations must be an int >= 1");
    auto url = [](const char* u) { return cfg("rpc_urls", Json::dict({{"base", u}})); };
    SCFG_CHECK_EQ(t, url("http://localhost:\xD9\xA3/"), "ok");  // \d is any script's digit (Arabic-Indic three)
    SCFG_CHECK_EQ(t, url("http://localhost:12345"), "ok");
    SCFG_CHECK_EQ(t, url("http://localhost:80/x"), "ok");
    SCFG_CHECK_EQ(t, url("http://127.0.0.1"), "ok");
    SCFG_CHECK_EQ(t, url("https://"), "ok");
    SCFG_CHECK_EQ(t, url("http://localhost:123456"),
                  "ConfigError(rpc.not_https): rpc url must start with https://: 'http://localhost:123456'");
    SCFG_CHECK_EQ(t, url("http://localhost:"),
                  "ConfigError(rpc.not_https): rpc url must start with https://: 'http://localhost:'");
    SCFG_CHECK_EQ(t, url("http://localhostx"),
                  "ConfigError(rpc.not_https): rpc url must start with https://: 'http://localhostx'");
    SCFG_CHECK_EQ(t, cfg("wallet", std::string(" ") + EVM),
                  std::string("ConfigError(address.invalid): wallet: not a 20-byte hex address: ' ") + EVM + "'");
    SCFG_CHECK(t, config::build_config(evm_kw("testnet", "yes")).value().testnet);
}

SCFG_TEST(ConfigTests, WalletsAndLookups) {
    Json kw = Json::dict({{"wallet", EVM},
                          {"solana_wallet", SOL},
                          {"previous_wallets", Json::list({std::string(" ") + py::lower_ascii(EVM) + " ",
                                                           "0x" + std::string(40, '1'), SOL})}});
    config::Config c = config::build_config(kw).value();
    SCFG_CHECK_EQ(t, strings(c.previous_wallets), "0x" + std::string(40, '1'));
    SCFG_CHECK(t, c.previous_solana_wallets.empty());
    SCFG_CHECK_EQ(t, strings(c.accepted_wallets()), std::string(EVM) + " 0x" + std::string(40, '1'));
    SCFG_CHECK_EQ(t, strings(c.accepted_wallets_for("solana")), SOL);
    SCFG_CHECK_EQ(t, strings(c.accepted_wallets_for("evm")), strings(c.accepted_wallets()));
    SCFG_CHECK(t, c.wallet_for(*chains::MAINNET().find("solana")) == std::optional<std::string>(SOL));
    SCFG_CHECK(t, c.wallet_for(*chains::MAINNET().find("base")) == std::optional<std::string>(EVM));
    SCFG_CHECK_EQ(t, strings(c.chains), "ethereum solana arc base arbitrum polygon");
    SCFG_CHECK_EQ(t, strings(c.registry().keys()), "ethereum arbitrum polygon base arc solana");
    SCFG_CHECK(t, c.item_by_code(7) == nullptr);
    SCFG_CHECK_EQ(t, outcome(config::add_item(c, "a", Json::dict({{"code", 7}, {"price", "1"}, {"token", "USDC"},
                                                                   {"title", "A"}}))),
                  "ok");
    SCFG_CHECK(t, c.item_by_code(7) != nullptr && c.item_by_code(7)->item_id == "a");
    SCFG_CHECK_EQ(t, strings(config::chains_for_token(c, "USDT").value()), "ethereum solana arbitrum polygon");
}

SCFG_TEST(ConfigTests, EnabledChainsRaisesKeyErrorForAChainTheRegistryLost) {
    config::Config c = config::build_config(Json::dict({{"wallet", EVM}})).value();
    chains::Chain saved = *chains::MAINNET().find("base");
    chains::MAINNET().erase("base");
    SCFG_CHECK_EQ(t, outcome(config::enabled_chains(c)), "KeyError: 'base'");
    // Put it back in Python's order: a re-added key goes last, so arc and solana go after it again.
    chains::Chain arc = *chains::MAINNET().find("arc");
    chains::Chain sol = *chains::MAINNET().find("solana");
    chains::MAINNET().erase("arc");
    chains::MAINNET().erase("solana");
    chains::MAINNET().set("base", saved);
    chains::MAINNET().set("arc", arc);
    chains::MAINNET().set("solana", sol);
    SCFG_CHECK_EQ(t, strings(chains::MAINNET().keys()), "ethereum arbitrum polygon base arc solana");
    SCFG_CHECK_EQ(t, outcome(config::enabled_chains(c)), "ok");
}

SCFG_TEST(ConfigTests, LogoAccentAndTextOn) {
    SCFG_CHECK_EQ(t, config::logo_content_type("a.PNG").value(), "image/png");
    SCFG_CHECK_EQ(t, config::logo_content_type("x/y.JpEg").value(), "image/jpeg");
    SCFG_CHECK_EQ(t, config::logo_content_type("a.webp.png").value(), "image/png");
    SCFG_CHECK_EQ(t, outcome(config::logo_content_type("a.\xE2\x84\xAA")),  // KELVIN SIGN lowers to k
                  "ConfigError(logo.bad_type): logo must be a .png, .jpg or .webp image: 'a.\xE2\x84\xAA'");
    SCFG_CHECK_EQ(t, outcome(config::logo_content_type("a")),
                  "ConfigError(logo.bad_type): logo must be a .png, .jpg or .webp image: 'a'");
    SCFG_CHECK_EQ(t, config::build_config(evm_kw("accent", "#ABC")).value().accent, "#aabbcc");
    for (const char* color : {"#0e7c74", "#000000", "#777777", "#757575", "#17202e", "#-f-f-f"}) {
        SCFG_CHECK_EQ(t, config::text_on(color).value(), "#ffffff");
    }
    SCFG_CHECK_EQ(t, config::text_on("#ffffff").value(), "#17202e");
    SCFG_CHECK_EQ(t, outcome(config::text_on("#zz0000")), "ValueError: invalid literal for int() with base 16: 'zz'");
    SCFG_CHECK_EQ(t, outcome(config::text_on("# 1+2_")), "ValueError: invalid literal for int() with base 16: '_'");
    SCFG_CHECK_EQ(t, outcome(config::text_on("#1")), "ValueError: invalid literal for int() with base 16: ''");
    Json methods = config::DEFAULT_METHODS();
    methods.as_dict().set("qr", 5);  // a fresh dict each call: the next one is untouched
    SCFG_CHECK_EQ(t, py::repr(config::DEFAULT_METHODS()).value(),
                  "{'browser': {'enabled': True, 'timeout_min': 30}, 'qr': {'enabled': True, 'wallet_button': True, "
                  "'chains': None}, 'manual': {'enabled': True, 'allow_tx_paste': True}}");
}

// ---- add_item ----

SCFG_TEST(ConfigTests, AddItemChecksInPythonsOrder) {
    config::Config c = config::build_config(Json::dict({{"wallet", EVM}})).value();
    auto add = [&c](const char* id, Json code, Json price, Json token, Json title, Json min_price = Json()) {
        Json kw = Json::dict({{"code", code}, {"price", price}, {"token", token}, {"title", title}});
        if (!min_price.is_null()) kw.as_dict().set("min_price", min_price);
        return outcome(config::add_item(c, id, kw));
    };
    SCFG_CHECK_EQ(t, add("a", 7, "1", "USDC", "A"), "ok");
    SCFG_CHECK_EQ(t, add("b", 8, "1", "USDC", Json::list({"x"})),
                  "ConfigError(item.invalid_title): title must be a non-empty string of at most 80 characters");
    SCFG_CHECK_EQ(t, add("b", 8, "1", Json::list({"USDC"}), "x"),
                  "ConfigError(token.unavailable): token ['USDC'] is not available on any enabled chain");
    SCFG_CHECK_EQ(t, add("b", Json(BigInt(1) << 70), "1", "USDC", "x"),
                  "ConfigError(item.invalid_code): item code must be an int in 1..99");
    SCFG_CHECK_EQ(t, add("b", 8, 1.5, "USDC", "x"),
                  "ConfigError(item.invalid_price): item 'b': amounts must be str, int or Decimal (never float)");
    SCFG_CHECK_EQ(t, add("b", 8, "2", "USDC", "x", "abc"),
                  "ConfigError(item.invalid_price): item 'b': not a number: 'abc'");
    SCFG_CHECK_EQ(t, add("b", 7, "2", "USDC", "x"), "ConfigError(item.duplicate_code): duplicate item code 7");
    SCFG_CHECK_EQ(t, add("a", 8, "2", "USDC", "x"), "ConfigError(item.duplicate_id): duplicate item id 'a'");
    SCFG_CHECK_EQ(t, c.items.size(), 1u);
}

// ---- prepare_payment ----

SCFG_TEST(PaymentsTests, ConstantsAreCents) {
    SCFG_CHECK_EQ(t, payments::MIN_TIP().str(), "0.50");
    std::string presets;
    for (const scfg::Dec& d : payments::TIP_PRESETS()) presets += d.str() + " ";
    SCFG_CHECK_EQ(t, presets, "1.00 3.00 5.00 10.00 ");
}

SCFG_TEST(PaymentsTests, ATipCarriesWhateverTitleAndItemItWasGiven) {
    config::Config c =
        config::build_config(Json::dict({{"wallet", EVM}, {"solana_wallet", SOL}})).value();
    SCFG_CHECK_EQ(t, outcome(config::add_item(c, "a", Json::dict({{"code", 7}, {"price", "1"}, {"token", "USDC"},
                                                                   {"title", "A"}}))),
                  "ok");
    auto pay = [&c](Json kind, Json kw) { return payments::prepare_payment(c, kind, kw); };
    auto spec = pay("tip", Json::dict({{"amount", "1"}, {"token", "USDC"}, {"title", 5}, {"item", Json::list({1})}}));
    SCFG_CHECK_EQ(t, outcome(spec), "ok");
    if (spec.ok()) {
        SCFG_CHECK_EQ(t, py::repr(spec.value().title).value(), "5");
        SCFG_CHECK_EQ(t, py::repr(spec.value().item).value(), "[1]");
        SCFG_CHECK_EQ(t, spec.value().price.str(), "1.00");
        SCFG_CHECK_EQ(t, spec.value().code, 0);
        SCFG_CHECK_EQ(t, strings(spec.value().chains), "ethereum solana arc base arbitrum polygon");
    }
    auto falsy = pay("tip", Json::dict({{"amount", "1"}, {"token", "USDC"}, {"title", 0}}));
    SCFG_CHECK(t, falsy.ok() && py::repr(falsy.value().title).value() == "'Tip'" && falsy.value().item.is_null());
    auto unlock = pay("unlock", Json::dict({{"item", "a"}, {"title", Json::list({1})}, {"token", 0}}));
    SCFG_CHECK(t, unlock.ok() && py::repr(unlock.value().title).value() == "[1]" && unlock.value().token == "USDC" &&
                      unlock.value().code == 7);
    SCFG_CHECK_EQ(t, outcome(pay("unlock", Json::dict({{"item", Json::list({1})}}))), "TypeError: unhashable type: 'list'");
    SCFG_CHECK_EQ(t, outcome(pay("unlock", Json::dict({{"item", Json::dict()}}))), "TypeError: unhashable type: 'dict'");
    SCFG_CHECK_EQ(t, outcome(pay("unlock", Json::dict({{"item", 5}}))),
                  "ConfigError(item.unknown): unknown item 5; declare it with scfg.add_item()");
    SCFG_CHECK_EQ(t, outcome(pay("unlock", Json::dict())),
                  "ConfigError(item.unknown): unknown item None; declare it with scfg.add_item()");
    SCFG_CHECK_EQ(t, outcome(pay("tip", Json::dict({{"amount", "1"}, {"token", 0}}))),
                  "ConfigError(token.not_enabled): token 0 is not enabled");
    SCFG_CHECK_EQ(t, outcome(pay(Json::list({"tip"}), Json::dict({{"amount", "1"}, {"token", "USDC"}}))),
                  "ConfigError(payment.bad_kind): kind must be 'unlock' or 'tip', not ['tip']");
    SCFG_CHECK_EQ(t, outcome(pay("unlock", Json::dict({{"item", "a"}, {"amount", "0.5"}}))),
                  "ConfigError(payment.below_min_price): amount 0.50 is below the item's min_price 1.00");
    SCFG_CHECK_EQ(t, outcome(pay("tip", Json::dict({{"amount", 1.5}, {"token", "USDC"}}))),
                  "TypeError: amounts must be str, int or Decimal (never float)");
    SCFG_CHECK_EQ(t, outcome(pay("tip", Json::dict({{"amount", "1"}, {"token", "USDC"}, {"colour", 1}}))),
                  "TypeError: prepare_payment() got an unexpected keyword argument 'colour'");
}

// test_config.py's test_onramp_url_needs_a_host: a host must follow https://.
SCFG_TEST(ConfigTests, test_onramp_url_needs_a_host) {
    for (const char* url : {"https://", "https:///buy", "https://{amount}", "https://?a=1", "https://#top", "https://}"}) {
        const std::string got = cfg("onramp", Json::dict({{"url", url}}));
        SCFG_CHECK(t, got.starts_with("ConfigError(onramp.bad_url)"));
    }
    Result<config::Config> ok = config::build_config(evm_kw("onramp", Json::dict({{"url", "https://x"}})));
    SCFG_CHECK(t, ok.ok() && ok.value().onramp.url == "https://x");
}
