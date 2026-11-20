// Copyright 2026 Repulse
// Twin of worker.py's Worker.submit as the controllers see it (Submitter.h): the core's scfg::worker::Submitter on
// FScfgWorker. Each command goes to the worker's command of the same name, with its std::function OnDone wrapped for
// TUniqueFunction (an empty one becomes a no-op: a fire-and-forget command, whose Err the worker still reports).
//
// LIFETIME. The controllers share the adapter (a std::shared_ptr) and may outlive the worker: a CheckoutController's
// destructor calls close(), which may submit discard_session synchronously. So every command first checks the
// worker's lifetime token (FScfgWorker::GetLifetime()): once the worker is gone the command is dropped and its done
// is never called, which Submitter.h allows ("It may also drop done without calling it"). Nothing is called back
// from inside the call, so a command submitted from a destructor or during any teardown order (adapter, worker,
// controllers) touches nothing that is gone and never re-enters the caller.
//
// While the worker is alive its own rules apply: a stopped worker cancels the command (CancelledError, on its next
// frame), and a callback runs from the worker's ticker, never inside the call.
#pragma once

#include "CoreMinimal.h"

#include "ScfgWorker.h"

#include "Scfg/Json.h"
#include "Scfg/Restore.h"
#include "Scfg/Submitter.h"

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

class SCFG_API FScfgSubmitter final : public scfg::worker::Submitter
{
public:
	explicit FScfgSubmitter(FScfgWorker& InWorker);

	virtual void start_session(scfg::Json kind, scfg::Json chain, scfg::Json opts,
		scfg::worker::OnDone<scfg::Json> done, scfg::worker::AbandonFlag abandoned) override;
	virtual void build_solana_payment(
		scfg::Json sid, scfg::Json account, scfg::worker::OnDone<scfg::Json> done) override;
	virtual void report_hint(scfg::Json sid, scfg::Json tx_hash, scfg::worker::OnDone<void> done) override;
	virtual void set_ui_status(scfg::Json sid, scfg::Json status, scfg::worker::OnDone<void> done) override;
	virtual void discard_session(scfg::Json sid, scfg::worker::OnDone<void> done) override;
	virtual void restore_lookup(
		scfg::Json tx_hash, scfg::worker::OnDone<std::vector<scfg::restore::RestoreCandidate>> done) override;
	virtual void restore_apply(std::vector<scfg::restore::RestoreCandidate> candidates,
		std::optional<std::map<int64_t, scfg::Json>> choices, scfg::worker::OnDone<scfg::Json> done) override;

	// The worker, while it exists; null once it is gone.
	FScfgWorker* Live() const;
	// Commands dropped because the worker was gone (the tests count them).
	int32 NumDropped() const { return Dropped; }

private:
	template <class T>
	static FScfgWorker::TOnDone<T> Wrap(scfg::worker::OnDone<T> Done);

	FScfgWorker* Worker;
	TWeakPtr<bool> Lifetime;
	int32 Dropped = 0;
};
