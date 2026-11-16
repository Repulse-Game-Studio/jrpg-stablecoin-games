// Twin of scfg_core/chains.py: Token's normalisation, Chain's methods and the built-in registries.
#include "Scfg/Chains.h"

#include "Scfg/Err.h"
#include "Scfg/Py.h"
#include "Scfg/SolanaBase58.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace scfg::chains::detail {

// Chain(key=..., chain_id=..., name=..., rpc=..., confirmations=..., max_log_range=..., explorer_tx=...,
// native_symbol=..., fee_hint=..., tokens=...), the other fields at their defaults.
Chain chain(std::string key, int64_t chain_id, std::string name, std::vector<std::string> rpc, int64_t confirmations,
            int64_t max_log_range, std::string explorer_tx, std::string native_symbol, std::string fee_hint,
            OrderedMap<Token> tokens) {
    Chain c;
    c.key = std::move(key);
    c.chain_id = BigInt(chain_id);
    c.name = std::move(name);
    c.rpc = std::move(rpc);
    c.confirmations = BigInt(confirmations);
    c.max_log_range = BigInt(max_log_range);
    c.explorer_tx = std::move(explorer_tx);
    c.native_symbol = std::move(native_symbol);
    c.fee_hint = std::move(fee_hint);
    c.tokens = std::move(tokens);
    return c;
}

// Token(address) of the built-in tables: a typo there is a programming error, and value() aborts on it (ChainsTests
// and tier1/chains.json build every entry).
Token token(std::string_view address) { return Token::make(address).value(); }

// Token(ARC_USDC, log_address=ARC_SYSTEM_EMITTER, log_decimals=18).
Token arc_usdc() { return Token::make(ARC_USDC, 6, ARC_SYSTEM_EMITTER, 18).value(); }

OrderedMap<Chain> build_mainnet() {
    OrderedMap<Chain> m;
    m.set("ethereum", chain("ethereum", 1, "Ethereum", {"https://ethereum-rpc.publicnode.com", "https://eth.drpc.org"},
                            12, 125, "https://etherscan.io/tx/{}", "ETH", "high",
                            {{"USDC", token("0xA0b86991c6218b36c1d19D4a2e9Eb0cE3606eB48")},
                             {"USDT", token("0xdAC17F958D2ee523a2206206994597C13D831ec7")}}));
    // publicnode only serves very recent Arbitrum blocks without a token: last resort.
    m.set("arbitrum", chain("arbitrum", 42161, "Arbitrum",
                            {"https://arb1.arbitrum.io/rpc", "https://arbitrum.drpc.org", "https://1rpc.io/arb",
                             "https://arbitrum-one-rpc.publicnode.com"},
                            20, 2000, "https://arbiscan.io/tx/{}", "ETH", "low",
                            {{"USDC", token("0xaf88d065e77c8cC2239327C5EDb3A432268e5831")},
                             {"USDT", token("0xFd086bC7CD5C481DCC9C85ebE478A1C0b69FCbb9")}}));
    m.set("polygon", chain("polygon", 137, "Polygon",
                           {"https://polygon-bor-rpc.publicnode.com", "https://polygon.drpc.org",
                            "https://1rpc.io/matic"},
                           32, 500, "https://polygonscan.com/tx/{}", "POL", "lowest",
                           {{"USDC", token("0x3c499c542cEF5E3811e1192ce70d8cC03d5c3359")},
                            {"USDT", token("0xc2132D05D31c914a87C6611C10748AEb04B58e8F")}}));
    m.set("base", chain("base", 8453, "Base",
                        {"https://mainnet.base.org", "https://base-rpc.publicnode.com", "https://base.drpc.org",
                         "https://1rpc.io/base"},
                        10, 125, "https://basescan.org/tx/{}", "ETH", "low",
                        {{"USDC", token("0x833589fCD6eDb6E08f4c7C32D4f71b54bdA02913")}}));
    // dRPC caps getLogs at a few dozen blocks, so it is never first, and never last either: the last endpoint is the
    // one handed to a wallet that adds the network.
    Chain arc = chain("arc", 5042, "Arc",
                      {"https://rpc.mainnet.arc.io", "https://rpc.quicknode.mainnet.arc.io",
                       "https://rpc.drpc.mainnet.arc.io", "https://rpc.blockdaemon.mainnet.arc.io"},
                      1, 9'999,  // BFT finality: one block is final
                      "https://explorer.arc.io/tx/{}", "USDC", "lowest", {{"USDC", arc_usdc()}});
    arc.native_token = "USDC";
    arc.min_fee_wei = ARC_MIN_FEE_WEI;
    m.set("arc", std::move(arc));
    // publicnode answers fast but keeps ~2 days of history; mainnet-beta keeps all of it.
    Chain solana = chain("solana", 0, "Solana",
                         {"https://solana-rpc.publicnode.com", "https://api.mainnet-beta.solana.com"}, 32, 0,
                         "https://solscan.io/tx/{}", "SOL", "lowest",
                         {{"USDC", token("EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v")},
                          {"USDT", token("Es9vMFrzaCERmJfrF4H2FYD4KCoNkY11McCe8BenwNYB")}});
    solana.family = "solana";
    solana.wallet_chain = "solana:mainnet";
    m.set("solana", std::move(solana));
    return m;
}

OrderedMap<Chain> build_testnet() {
    OrderedMap<Chain> m;
    auto add = [&m](Chain c) {
        c.testnet = true;
        std::string key = c.key;
        m.set(std::move(key), std::move(c));
    };
    add(chain("ethereum", 11155111, "Sepolia",
              {"https://ethereum-sepolia-rpc.publicnode.com", "https://1rpc.io/sepolia"}, 3, 2000,
              "https://sepolia.etherscan.io/tx/{}", "ETH", "high",
              {{"USDC", token("0x1c7D4B196Cb0C7B01d743Fbc6116a902379C7238")}}));
    add(chain("arbitrum", 421614, "Arbitrum Sepolia",
              {"https://sepolia-rollup.arbitrum.io/rpc", "https://arbitrum-sepolia-rpc.publicnode.com"}, 3, 2000,
              "https://sepolia.arbiscan.io/tx/{}", "ETH", "low",
              {{"USDC", token("0x75faf114eafb1BDbe2F0316DF893fd58CE46AA4d")}}));
    add(chain("polygon", 80002, "Polygon Amoy", {"https://polygon-amoy-bor-rpc.publicnode.com"}, 3, 2000,
              "https://amoy.polygonscan.com/tx/{}", "POL", "lowest",
              {{"USDC", token("0x41E94Eb019C0762f9Bfcf9Fb1E58725BfB0e7582")}}));
    add(chain("base", 84532, "Base Sepolia", {"https://base-sepolia-rpc.publicnode.com", "https://sepolia.base.org"},
              3, 2000, "https://sepolia.basescan.org/tx/{}", "ETH", "low",
              {{"USDC", token("0x036CbD53842c5426634e7929541eC2318f3dCF7e")}}));
    // Blockdaemon's testnet endpoint answers gzip unasked (urllib can't read it): left out.
    Chain arc = chain("arc", 5042002, "Arc Testnet",
                      {"https://rpc.testnet.arc.io", "https://rpc.drpc.testnet.arc.io",
                       "https://rpc.quicknode.testnet.arc.io"},
                      1, 9'999, "https://explorer.testnet.arc.io/tx/{}", "USDC", "lowest", {{"USDC", arc_usdc()}});
    arc.native_token = "USDC";
    arc.min_fee_wei = ARC_MIN_FEE_WEI;
    add(std::move(arc));
    Chain solana = chain("solana", 0, "Solana Devnet", {"https://api.devnet.solana.com"}, 32, 0,
                         "https://solscan.io/tx/{}?cluster=devnet", "SOL", "lowest",
                         {{"USDC", token("4zMMC9srt5Ri5X14GAgXhaHii3GnPAEERYPJgZJDncDU")}});
    solana.family = "solana";
    solana.wallet_chain = "solana:devnet";
    add(std::move(solana));
    return m;
}

}  // namespace scfg::chains::detail

namespace scfg::chains {

Result<Token> Token::make(std::string_view address, int64_t decimals, std::string_view log_address,
                          int64_t log_decimals) {
    // EVM addresses are always EIP-55 checksummed, so comparisons with parsed logs never depend on case. Solana
    // mints are base58 and compared exactly.
    Token t;
    if (address.substr(0, 2) == "0x") {
        SCFG_TRY(t.address, evm::to_checksum_address(Json(address)));
    } else {
        SCFG_TRY(t.address, solana::base58::normalize_pubkey(Json(address)));
    }
    t.decimals = decimals;
    t.log_decimals = log_decimals;
    if (!log_address.empty()) {
        SCFG_TRY(t.log_address, evm::to_checksum_address(Json(log_address)));
        if (log_decimals < decimals) return Err::value_error("log_decimals must be at least decimals");
    }
    return t;
}

Chain Chain::with(std::optional<std::vector<std::string>> new_rpc, std::optional<BigInt> new_confirmations,
                  std::optional<OrderedMap<Token>> new_tokens) const {
    Chain c = *this;
    if (new_rpc) c.rpc = std::move(*new_rpc);
    if (new_confirmations) c.confirmations = std::move(*new_confirmations);
    if (new_tokens) c.tokens = std::move(*new_tokens);
    return c;
}

Result<std::string> Chain::explorer_url(std::string_view tx_hash) const { return py::format1(explorer_tx, tx_hash); }

Result<std::optional<evm::TransferLog>> Chain::parse_transfer(const Json& log) const {
    SCFG_TRY(std::optional<evm::TransferLog> t, evm::parse_transfer_log(log));
    if (!t) return std::optional<evm::TransferLog>();
    const Token* token = nullptr;
    for (const auto& [symbol, tk] : tokens) {
        if (tk.log_source() == t->token) {
            token = &tk;
            break;
        }
    }
    if (token == nullptr) return std::optional<evm::TransferLog>();
    if (token->log_address.empty()) return t;
    // t.value // 10 ** (log_decimals - decimals); Token::make keeps the exponent >= 0.
    if (token->log_decimals < token->decimals) return Err::value_error("log_decimals must be at least decimals");
    BigInt scale = BigInt(10).pow(static_cast<uint64_t>(token->log_decimals - token->decimals));
    SCFG_TRY(BigInt value, t->value.floordiv(scale));
    if (value <= BigInt(0)) return std::optional<evm::TransferLog>();
    t->token = token->address;  // replace(t, token=token.address, value=value)
    t->value = std::move(value);
    return t;
}

OrderedMap<Chain>& MAINNET() {
    static OrderedMap<Chain> registry = detail::build_mainnet();
    return registry;
}

OrderedMap<Chain>& TESTNET() {
    static OrderedMap<Chain> registry = detail::build_testnet();
    return registry;
}

OrderedMap<Chain>& get_chains(bool testnet) { return testnet ? TESTNET() : MAINNET(); }

}  // namespace scfg::chains
