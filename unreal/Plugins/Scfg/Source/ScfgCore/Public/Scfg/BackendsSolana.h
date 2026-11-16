// Twin of scfg_core/backends/solana.py (and of unity/.../Core/Backends/Solana.cs): Solana behind the Backend
// interface. Payments are found by their Solana Pay reference, by a reported signature, or by scanning the
// developer's token account for the exact encoded amount. A payment counts once the cluster marks its transaction
// finalized.
//
// What a transaction paid is read from meta.pre/postTokenBalances, never from its instructions, so it works whatever
// program or wallet moved the tokens.
//
// Node replies and stored records are read the way Python reads them: d[k] is py::get (its KeyError or TypeError),
// d.get(k) on a value that is not a dict is the AttributeError of Python's missing .get, `x or y` is py::truthy, and
// a slot or start_block is compared with Python's exact number rules (py::compare). What Python only carries along (a
// stored hint, reference or cursor sent to the node as it is; a balance's owner; its accountIndex; a slot of any
// size) is carried as the Json it is: a malformed reply gets Python's verdict, not a port's narrower one. A malformed
// reply or record is a non-RPC Err, which the Engine contains per session.
#pragma once

#include "Scfg/BackendsBase.h"
#include "Scfg/BigInt.h"
#include "Scfg/Json.h"
#include "Scfg/Result.h"
#include "Scfg/Sessions.h"
#include "Scfg/Transfers.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace scfg::backends::solana {

inline constexpr int64_t FINAL_CONFIRMATIONS = 32;  // Solana's "finalized" corresponds to 32 confirmed blocks
inline constexpr int64_t SCAN_TX_BUDGET = 5;        // getTransaction calls per step for the token-account scan
inline constexpr int64_t SIG_PAGE = 100;            // signatures asked for per getSignaturesForAddress page
inline constexpr int64_t MAX_SIG_PAGES = 10;        // pages one scan walk reads before it moves scan_before back
inline constexpr int64_t START_SLOT_MARGIN = 150;
inline constexpr int64_t COMPUTE_UNITS = 30'000;
inline constexpr int64_t MIN_PRIORITY_FEE = 1'000;      // micro-lamports per compute unit
inline constexpr int64_t MAX_PRIORITY_FEE = 1'000'000;  // x 30,000 CU = at most 30,000 lamports (0.00003 SOL)
// A wallet returns the signature before the transaction lands, and a transaction whose blockhash expires (~60-90 s)
// never lands. A hint unseen for this long was dropped. (Python defines it here; Sessions.h holds the port's one.)
using sessions::SOLANA_HINT_TIMEOUT_S;

// Python transfer_to(tx, sig=, mint=, owner=): what owner received of mint in tx, from its token-balance changes, or
// nullopt (a failed transaction, nothing gained, or no account of owner's that gained). The node's values are
// carried as they are (transfers::Transfer): sig as given, the accountIndex as sent, any owner as the payer, the slot
// unbounded. The payer is the owner that lost the most of the mint (the first of them, in accountIndex order), else
// the fee payer (accountKeys[0], a str or a {pubkey} entry, whatever its pubkey holds), else None. Raises what Python raises, in its order: AttributeError for a value with no .get,
// KeyError for a missing accountIndex, uiTokenAmount or amount (or slot), TypeError for an unhashable accountIndex
// or owner and for accountIndexes Python cannot sort (a str and an int, or None beside anything), int_from's errors
// for an amount or slot. Port limit: a NaN accountIndex is ordered last, where CPython's place for it depends on
// its set's hash order.
Result<std::optional<transfers::Transfer>> transfer_to(const Json& tx, const Json& sig, std::string_view mint,
                                                       std::string_view owner);

class SolanaBackend final : public base::Backend {
public:
    std::string_view family() const override { return "solana"; }
    // SolanaRpc(RpcClient(chain.rpc)): the client's default log range; the SolanaRpc owns its client.
    Result<base::Rpc> make_rpc(const chains::Chain& chain, coio::Transport& transport) const override;
    // solana::base58::normalize_signature.
    Result<std::string> normalize_tx(const Json& text) const override;
    // {"start_block": max(0, slot - START_SLOT_MARGIN), "reference": a fresh 32-byte key (entropy.token_bytes(32)),
    // "dest": the wallet's token account for the token, "scan_cursor": its newest signature or None,
    // "scan_before": None}.
    Co<Json> start(base::Rpc rpc, chains::Chain chain, std::optional<std::string> wallet, std::string token,
                   std::reference_wrapper<Entropy> entropy) const override;
    // One step of pending[sid]: a hint, then the reference, then (with no hint) the token-account scan while
    // awaiting; getSignatureStatuses while confirming. RpcUnavailable and RpcError are the outcome's
    // "network_unavailable" (unless a hint verdict of this step already set its error); any other Err is returned.
    Co<watcher::StepOutcome> advance(Json pending, Json receipts, std::string sid, base::Rpc rpc, chains::Chain chain,
                                     std::string wallet, OrderedMap<models::Item> items, Json now) const override;
    // "keep" or "drop" for a stored receipt (its fields already validated by reverify_receipts). Network trouble
    // keeps it, and so does a transaction this build cannot read (KeyError, TypeError, ValueError, AttributeError).
    Co<std::string> reverify(Json receipt, base::Rpc rpc, chains::Chain chain,
                             std::vector<std::string> wallets) const override;
    // The transfers to each accepted Solana wallet, of each of the chain's tokens, in tx (asked of each endpoint in
    // turn), or nullopt when no endpoint has it. RestoreError tx_reverted / tx_not_a_payment. A transfer
    // restore::classify cannot price is skipped and reported to on_error.
    Co<std::optional<std::vector<restore::RestoreCandidate>>> lookup(std::string tx, config::Config config,
                                                                     chains::Chain chain, base::Rpc rpc,
                                                                     Json receipts,
                                                                     base::ErrorSink on_error) const override;
    // Token name -> the wallet's token account for it exists (wallets refuse to pay one that does not). rpc is
    // called for each token before its address is derived, as Python's _LazyRpc builds the client on the attribute
    // lookup. A getAccountInfo result without "value" is a KeyError, which the Engine reports.
    Co<OrderedMap<bool>> check_ready(base::LazyRpc rpc, chains::Chain chain,
                                     std::optional<std::string> wallet) const override;
    // The unsigned transfer from account's token account to the session's, with the session's reference appended,
    // for a browser wallet to sign: {"transaction": base64 of the unsigned transaction, "token_balance",
    // "sol_balance": decimal strings of base units}. The priority fee is the median recent fee for the destination,
    // clamped to [MIN_PRIORITY_FEE, MAX_PRIORITY_FEE].
    Co<Json> build_payment(Json rec, base::Rpc rpc, chains::Chain chain, Json account) const override;
};

}  // namespace scfg::backends::solana
