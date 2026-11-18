// Copyright 2026 Repulse
// Twin of worker.py's submit() as the controllers use it (see ScfgSubmitter.h).
#include "ScfgSubmitter.h"

#include <utility>

FScfgSubmitter::FScfgSubmitter(FScfgWorker& InWorker) : Worker(&InWorker), Lifetime(InWorker.GetLifetime()) {}

FScfgWorker* FScfgSubmitter::Live() const
{
	const TSharedPtr<bool> Alive = Lifetime.Pin();
	return Alive.IsValid() && *Alive ? Worker : nullptr;
}

template <class T>
FScfgWorker::TOnDone<T> FScfgSubmitter::Wrap(scfg::worker::OnDone<T> Done)
{
	return [Done = std::move(Done)](scfg::Result<T> R)
	{
		if (Done)
		{
			Done(std::move(R));
		}
	};
}

void FScfgSubmitter::start_session(scfg::Json kind, scfg::Json chain, scfg::Json opts,
	scfg::worker::OnDone<scfg::Json> done, scfg::worker::AbandonFlag abandoned)
{
	if (FScfgWorker* W = Live())
	{
		W->StartSession(
			std::move(kind), std::move(chain), std::move(opts), Wrap(std::move(done)), std::move(abandoned));
		return;
	}
	++Dropped;
}

void FScfgSubmitter::build_solana_payment(scfg::Json sid, scfg::Json account, scfg::worker::OnDone<scfg::Json> done)
{
	if (FScfgWorker* W = Live())
	{
		W->BuildSolanaPayment(std::move(sid), std::move(account), Wrap(std::move(done)));
		return;
	}
	++Dropped;
}

void FScfgSubmitter::report_hint(scfg::Json sid, scfg::Json tx_hash, scfg::worker::OnDone<void> done)
{
	if (FScfgWorker* W = Live())
	{
		W->ReportHint(std::move(sid), std::move(tx_hash), Wrap(std::move(done)));
		return;
	}
	++Dropped;
}

void FScfgSubmitter::set_ui_status(scfg::Json sid, scfg::Json status, scfg::worker::OnDone<void> done)
{
	if (FScfgWorker* W = Live())
	{
		W->SetUiStatus(std::move(sid), std::move(status), Wrap(std::move(done)));
		return;
	}
	++Dropped;
}

void FScfgSubmitter::discard_session(scfg::Json sid, scfg::worker::OnDone<void> done)
{
	if (FScfgWorker* W = Live())
	{
		W->Discard(std::move(sid), Wrap(std::move(done)));
		return;
	}
	++Dropped;
}

void FScfgSubmitter::restore_lookup(
	scfg::Json tx_hash, scfg::worker::OnDone<std::vector<scfg::restore::RestoreCandidate>> done)
{
	if (FScfgWorker* W = Live())
	{
		W->RestoreLookup(std::move(tx_hash), Wrap(std::move(done)));
		return;
	}
	++Dropped;
}

void FScfgSubmitter::restore_apply(std::vector<scfg::restore::RestoreCandidate> candidates,
	std::optional<std::map<int64_t, scfg::Json>> choices, scfg::worker::OnDone<scfg::Json> done)
{
	if (FScfgWorker* W = Live())
	{
		W->RestoreApply(std::move(candidates), std::move(choices), Wrap(std::move(done)));
		return;
	}
	++Dropped;
}
