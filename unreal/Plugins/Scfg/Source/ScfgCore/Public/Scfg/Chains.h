// Twin of scfg_core/chains.py (and of unity/.../Core/Chains.cs, rpgmaker/src/core/chains.ts).
// Registry of supported chains and stablecoin contracts.
//
// Token addresses come from Circle's and Tether's official contract lists. Solana mints come from those same lists
// and are compared exactly (base58), never case-folded. Tokens are always identified by contract address, never by
// symbol, so a look-alike token called "USDC" can never be mistaken for the real one. Testnet entries use the same
// keys so a game can switch with testnet=true.
//
// Developers can add further EVM chains of their own (config::build_chain, exposed as scfg.add_chain); those never
// enter these registries, they live on the Config.
//
// RPC lists and max_log_range values were measured against the live public endpoints (tools/check_rpcs.py). The
// first endpoint should be the most generous.
//
// Log sources: on most chains a token's Transfer events come from its own contract. On Arc, USDC is the native gas
// coin: a plain native send logs a Transfer only at a system emitter address (18 decimals), while an ERC-20
// transfer() logs there *and* at the 0x3600 interface (6 decimals). Such a token names the emitter as its
// log_address, and Chain::parse_transfer reads only that stream, floored to 6-decimal units, so every payment is
// seen exactly once however the player sent it.
//
// MAINNET() and TESTNET() are the mutable registries themselves (Python's module-level dicts): the integration
// driver points entries at local nodes, as tests/integration's monkeypatch.setitem does. They are built on first use
// (function-local statics), never during static initialisation.
#pragma once

#include "Scfg/BigInt.h"
#include "Scfg/Evm.h"
#include "Scfg/Export.h"
#include "Scfg/Json.h"
#include "Scfg/OrderedMap.h"
#include "Scfg/Result.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scfg::chains {

inline constexpr std::string_view ARC_USDC = "0x3600000000000000000000000000000000000000";
inline constexpr std::string_view ARC_SYSTEM_EMITTER = "0xffffFFFfFFffffffffffffffFfFFFfffFFFfFFfE";
inline constexpr int64_t ARC_MIN_FEE_WEI = 20'000'000'000;  // Arc's mempool silently drops anything below 20 gwei

// Python's frozen Token dataclass. Build one with make(), which runs __post_init__'s normalisation; the fields are
// public (as the dataclass's are readable) but a Token assembled by hand skips that check.
struct SCFGCORE_API Token {
    std::string address;  // EIP-55 checksummed for an EVM token; the canonical base58 mint for Solana
    int64_t decimals = 6;
    // Where this token's Transfer events are logged, when that is not its own contract (Arc's system emitter), and
    // the decimals of the values logged there.
    std::string log_address;
    int64_t log_decimals = 0;

    // Token(address, decimals, log_address, log_decimals): a "0x" address is checksummed (any case), anything else
    // is a Solana mint (normalize_pubkey, which strips); a non-empty log_address is checksummed and needs
    // log_decimals >= decimals. Every failure is a ValueError, as Python's.
    static Result<Token> make(std::string_view address, int64_t decimals = 6, std::string_view log_address = "",
                              int64_t log_decimals = 0);

    // The contract address whose Transfer events record this token's payments.
    const std::string& log_source() const { return log_address.empty() ? address : log_address; }
};

// Python's frozen Chain dataclass, a value: copying it copies its rpc list and tokens (Python's are a tuple and a
// dict nobody mutates). chain_id, confirmations and max_log_range are a developer's ints (build_chain,
// build_config's confirmations), unbounded as Python's: 2**70 confirmations is accepted, as in Python, and simply
// never reached.
struct SCFGCORE_API Chain {
    std::string key;
    BigInt chain_id;
    std::string name;
    std::vector<std::string> rpc;
    BigInt confirmations;
    BigInt max_log_range;
    std::string explorer_tx;
    std::string native_symbol;
    std::string fee_hint;
    OrderedMap<Token> tokens;  // symbol -> token, in declaration order
    bool testnet = false;
    std::string family = "evm";  // "evm" or "solana"
    std::string wallet_chain;
    std::string native_token;  // the token in `tokens` that is also the gas coin (Arc: "USDC")
    int64_t min_fee_wei = 0;   // maxFeePerGas floor the checkout page enforces (0: none)

    // dataclasses.replace(chain, rpc=..., confirmations=..., tokens=...): an absent argument keeps the field.
    Chain with(std::optional<std::vector<std::string>> new_rpc = std::nullopt,
               std::optional<BigInt> new_confirmations = std::nullopt,
               std::optional<OrderedMap<Token>> new_tokens = std::nullopt) const;

    // explorer_tx.format(tx_hash): IndexError, KeyError or ValueError for a template build_chain would refuse.
    Result<std::string> explorer_url(std::string_view tx_hash) const;

    // A raw log as a payment in one of this chain's tokens, or nullopt. Only logs from a token's log source count.
    // Values logged with more decimals than the token has are floored to its units; a log that floors to 0 is not a
    // payment. The result always carries the token's canonical address. A malformed log fails as
    // evm::parse_transfer_log does.
    Result<std::optional<evm::TransferLog>> parse_transfer(const Json& log) const;
};

// The built-in registries, in Python's order (ethereum, arbitrum, polygon, base, arc, solana).
SCFGCORE_API OrderedMap<Chain>& MAINNET();
SCFGCORE_API OrderedMap<Chain>& TESTNET();

inline constexpr std::array<std::string_view, 5> DISPLAY_ORDER = {"arbitrum", "base", "arc", "polygon", "ethereum"};

// TESTNET() if testnet else MAINNET(): the registry itself, not a copy.
SCFGCORE_API OrderedMap<Chain>& get_chains(bool testnet);

// chain.tokens[rec["token"]] for a stored record: rec["token"]'s KeyError or TypeError (py::get), the TypeError of an
// unhashable value, or the KeyError of a token the chain does not carry (py::key_error_for).
SCFGCORE_API Result<Token> token_of(const Chain& chain, const Json& rec);

}  // namespace scfg::chains
