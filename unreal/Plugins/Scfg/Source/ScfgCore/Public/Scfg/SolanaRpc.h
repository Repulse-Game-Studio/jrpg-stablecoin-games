// Copyright 2026 Repulse
// Twin of scfg_core/solana/rpc.py (and of unity/.../Core/Solana/Rpc.cs): Solana JSON-RPC on top of the shared
// failover client, ::scfg::rpc::RpcClient (the EVM one; inside scfg::solana, `rpc` alone names this namespace).
//
// History-sensitive lookups (restore, launch re-check) ask endpoints one by one, because public endpoints keep
// very different amounts of history: publicnode about two days, api.mainnet-beta everything. A missing
// transaction only counts as "doesn't exist" when an endpoint whose history reaches back to the slot in question
// says so.
//
// Results stay JSON values as the node sent them; numbers read with int_from (a slot, a balance, a fee) are BigInt,
// as Python's ints are. Every network method is a Co<...>; arguments that Python sends as given (a stored
// signature, an address) are Json.
#pragma once

#include "Scfg/BigInt.h"
#include "Scfg/Co.h"
#include "Scfg/Err.h"
#include "Scfg/Export.h"
#include "Scfg/Json.h"
#include "Scfg/OrderedMap.h"
#include "Scfg/Rpc.h"

#include <cstdint>
#include <optional>
#include <string>

namespace scfg::solana::rpc {

// The highest transaction version this build can read (only meta is used).
inline constexpr int64_t TX_VERSION = 1;
// _first_available's answer when an endpoint's history is unknown: treated as pruned.
inline constexpr int64_t UNKNOWN_HISTORY = int64_t{1} << 62;
inline constexpr int64_t UNSUPPORTED_VERSION = -32015;

// Python SolanaRpc._named_version for a client at tx_version: the newer version a -32015 "Transaction version (N)
// is not supported" names, else nullopt. The match is re.search(r"version \((\d+)\)", message, re.IGNORECASE):
// the leftmost one only, its letters matched as Python 3.9 folds them (ſ matches s; İ and ı match i), \d any
// Unicode 13 decimal digit. A number of more than 18 significant digits is no match (a request cannot
// carry it as an int), so no long run is converted. Linear in the message.
SCFGCORE_API std::optional<int64_t> named_version(const Err& error, int64_t tx_version);

// get_signatures_for_address's keyword arguments: a falsy until/before is left out.
struct SignatureOptions {
    Json until = Json();
    Json before = Json();
    int64_t limit = 100;
    std::string commitment = "confirmed";
};

// get_transaction_any's (tx, authoritative).
struct TxAny {
    Json tx;
    bool authoritative = false;
};

// Python solana.rpc.SolanaRpc. Holds the client by reference: the caller keeps it alive (and does not move this
// object or the client while a Co of theirs runs).
class SCFGCORE_API SolanaRpc {
public:
    explicit SolanaRpc(::scfg::rpc::RpcClient& client) : client_(&client) {}

    ::scfg::rpc::RpcClient& client() const { return *client_; }
    // The transaction version asked for; lowered once per -32602 from a node that does not know it.
    int64_t tx_version = TX_VERSION;

    Co<Json> call(std::string method, Json params);
    Co<BigInt> get_slot(std::string commitment = "confirmed");
    // The result, or [] for a falsy one (`... or []`).
    Co<Json> get_signatures_for_address(Json address, SignatureOptions options = {});
    // A -32602 naming the version lowers tx_version and asks again; a -32015 naming a newer version is asked once
    // with it; any other error is raised.
    Co<Json> get_transaction(Json sig, std::string commitment = "confirmed");
    // The transaction from the first live endpoint that has it, and whether a None answer is authoritative (an
    // endpoint whose history reaches min_slot said so). RpcUnavailable when no endpoint answered.
    Co<TxAny> get_transaction_any(Json sig, std::optional<BigInt> min_slot = std::nullopt,
                                  std::string commitment = "finalized");
    // res["value"] for list(sigs).
    Co<Json> get_signature_statuses(Json sigs);
    Co<bool> account_exists(Json address);
    // res["value"]["blockhash"], as the node sent it.
    Co<Json> get_latest_blockhash();
    Co<BigInt> get_token_balance(Json token_account);
    Co<BigInt> get_balance(Json address);
    // The median (upper middle) recent prioritization fee for list(accounts), 0 for none.
    Co<BigInt> get_priority_fee(Json accounts);

private:
    Json tx_params(const Json& sig, const std::string& commitment, std::optional<int64_t> version) const;
    bool lower_version(const Err& error);
    // Python _first_available: an endpoint's first available slot, cached; UNKNOWN_HISTORY when it cannot be
    // read (asked again next time).
    Co<BigInt> first_available(std::string url);

    ::scfg::rpc::RpcClient* client_;
    OrderedMap<BigInt> first_slot_;
};

}  // namespace scfg::solana::rpc
