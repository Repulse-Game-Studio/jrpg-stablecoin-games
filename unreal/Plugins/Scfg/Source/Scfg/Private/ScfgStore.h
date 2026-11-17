// The player's payment state on disk, the twin of godot/addons/scfg/store.gd (ScfgStore) and of Ren'Py's
// persistent.scfg_pending / persistent.scfg_receipts: FPaths::ProjectSavedDir()/Scfg/state.json, global to the
// install and never inside a save slot. Saved/ is a per-user writable location in a packaged game.
//
// The file is {"version": 1, "pending": {...}, "receipts": {...}}, the Engine's two stores as they are
// (conformance/schema/state.json), written with json::dumps (Python's json.dumps defaults: insertion order, ", " and
// ": ", ASCII only).
//
// LOADING (Load, and the store's constructor):
//   - A missing file is empty stores, quietly (a first launch). When the file is missing but <path>.tmp exists (a
//     save that stopped before its rename), the .tmp is read instead and, once it parses as the document, renamed to
//     <path> at once, so the next save never truncates the only copy (a .tmp that cannot be renamed blocks).
//   - A file that exists but cannot be read (a permission, a lock, an I/O error, a directory in its place, or a
//     directory above it that cannot be searched: stat failing other than with ENOENT) is an Error (RuntimeError
//     naming the file): the store is BLOCKED. A blocked store starts no worker and never writes, so what is on disk is
//     never replaced by empty stores; the next launch tries again.
//   - A file that is not that document (not UTF-8 JSON, not an object, a version other than the int 1, a pending or
//     receipts that is not an object) is moved aside to <path>.corrupt, then .corrupt.2, ..., never over an earlier
//     copy, and the stores start empty with a warning (players can bring purchases back with Restore). A bad file
//     that cannot be moved aside is an Error (blocked) instead: an empty start would let the next save replace it
//     with nothing kept anywhere.
//   - Each pending record is checked against the frozen schema (state_schema.check's pending rules: every required
//     field, the right types, the enums, no unknown field, the match object, and an id equal to its key). A record
//     that fails is dropped with a warning naming it, what is wrong and that Restore brings back a payment made for
//     it, and the whole file is first copied aside as above (copied, not moved), so the record is kept for support;
//     the cleaned stores are then saved. A malformed record would otherwise fail every tick (Engine::tick's expiry
//     pass is not contained per session). An unknown field is a failure too, as state_schema rules: the format is
//     frozen at version 1, so a build that adds a pending field must bump the version, and a file of another version
//     is moved aside whole before any record is looked at. Receipts are not checked here: the core tolerates any
//     receipt value (compute_ownership skips it) and the first reverify removes an invalid one (vectors
//     reverify/malformed_entries_are_removed, invalid_receipts_are_removed). Godot's store checks neither; this is the
//     record-level half of "a file that is not that document".
//
// SAVING (Save, and after each OnStateChanged of the attached worker), Godot's rule (scfg.gd's _sync):
//   - Nothing is written when the text is what is on disk (the worker fires after every tick).
//   - The receipts changing, the set of sessions changing, or a session's state or hint_tx changing is written at
//     once (a crash can end the game any moment); any other pending change (scan progress: cursors) at most every
//     PENDING_SAVE_INTERVAL seconds: it is owed and written by the ticker, or by the next save that may write.
//   - A write is atomic: <path>.tmp is written and flushed (fdatasync), then renamed over <path>, which replaces it in
//     one step (rename(2) on Linux and Mac; MoveFileExW with MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH on
//     Windows, unverified: no Win64 build here). The old file is never deleted first, so a failed save keeps it.
//   - A failed save (a dumps Err such as an int of more than 4300 digits, a write or a rename that fails) keeps the
//     old file, is reported once (not at every retry, until a save succeeds), and is retried every RETRY_INTERVAL
//     seconds from the ticker; a newer state replaces the one owed.
//   - Destroying the store writes what it still owes (the game quitting).
//   On Linux and Mac the store calls the POSIX functions itself (stat, open, write, fdatasync, rename), so errno is
//   read right at the failing call and the engine logs nothing per attempt; elsewhere it goes through IPlatformFile.
//
// ERRORS. Every Err and warning goes to the options' OnError / OnWarning (default: LogScfg), once; callers must not
// log them again. That includes CreateWorker's Err (the block reason, or FScfgWorker::Create's). OnError may destroy
// the store when it is called from a save, a retry or CreateWorker (the store touches nothing after); it must not
// from inside the constructor (nothing owns the store yet).
//
// Game thread only, as the rest of the glue. The store does not own the worker: it binds to its OnStateChanged by an
// explicit handle on the worker's shared handler list and removes it in Detach() and in its destructor, so either may
// be destroyed first.
#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Templates/Function.h"
#include "Templates/UniquePtr.h"

#include "ScfgTransport.h"
#include "ScfgWorker.h"

#include "Scfg/Config.h"
#include "Scfg/Err.h"
#include "Scfg/Json.h"
#include "Scfg/Result.h"

#include <string>
#include <vector>

// What FScfgStore::Load read from a file.
struct FScfgStoreLoad
{
	scfg::Json Pending = scfg::Json::dict();
	scfg::Json Receipts = scfg::Json::dict();
	// A corrupt file moved aside, and each pending record dropped: one sentence each, naming the file.
	TArray<FString> Warnings;
	// The file exists but cannot be read, or a bad one cannot be kept aside (a RuntimeError naming it): never save over
	// it this run. The stores are empty then.
	TOptional<scfg::Err> Error;
	// Pending records were dropped (and the file copied aside): the cleaned stores are not on disk yet.
	bool bCleaned = false;
};

// FScfgStore's options (FScfgStore::FOptions).
struct FScfgStoreOptions
{
	// The directory of state.json. Default: FPaths::ProjectSavedDir()/Scfg. The tests pass a temporary one.
	FString Directory;
	// The ticker that drives the retries; it must outlive the store. Default: FTSTicker::GetCoreTicker().
	FTSTicker* Ticker = nullptr;
	// Each Err, once (see ERRORS). Default: logged to LogScfg as "scfg: <Err::str()>".
	TFunction<void(const scfg::Err&)> OnError;
	// Each warning (a corrupt file, a dropped record), once. Default: a LogScfg warning.
	TFunction<void(const FString&)> OnWarning;
	// Seconds, for the PENDING_SAVE_INTERVAL throttle. Default: FPlatformTime::Seconds(). The tests pass their own.
	TFunction<double()> Clock;
};

class SCFG_API FScfgStore
{
public:
	static constexpr int64 VERSION = 1;
	// Seconds between the attempts of a failed or deferred save.
	static constexpr float RETRY_INTERVAL = 10.0f;
	// The least seconds between two writes of scan progress alone (Godot's and Ren'Py's PENDING_SAVE_INTERVAL).
	static constexpr double PENDING_SAVE_INTERVAL = 10.0;

	using FOptions = FScfgStoreOptions;

	// FPaths::ProjectSavedDir()/Scfg/state.json, as an absolute path.
	static FString DefaultPath();
	// The stores in Path (see LOADING). Never fails: a bad file is a warning, an unreadable one an Error. A corrupt
	// file is moved aside, a file with a malformed pending record copied aside, a lone complete .tmp renamed to Path.
	static FScfgStoreLoad Load(const FString& Path);
	// The problems state_schema.check finds in one pending record stored under Sid ("pending[ps_a]: missing 'id'"),
	// none when it conforms.
	static std::vector<std::string> CheckPending(const std::string& Sid, const scfg::Json& Record);
	// The schema the checks use: a copy of conformance/schema/state.json (a test compares the two).
	static const scfg::Json& Schema();

	// Loads <Directory>/state.json and logs its absolute path once; reports the warnings and the Error, if any.
	explicit FScfgStore(FOptions InOptions = FOptions());
	FScfgStore(const FScfgStore&) = delete;
	FScfgStore& operator=(const FScfgStore&) = delete;
	~FScfgStore();

	const FString& GetPath() const { return Path; }
	// The file could not be read: no payments, no saves (see LOADING).
	bool IsBlocked() const { return Blocked.IsSet(); }
	// Why (a RuntimeError naming the file), or null.
	const scfg::Err* GetBlockReason() const { return Blocked.GetPtrOrNull(); }
	// The stores as loaded and checked (empty when blocked): what CreateWorker hands to the Engine.
	const scfg::Json& GetPending() const { return Pending; }
	const scfg::Json& GetReceipts() const { return Receipts; }

	// FScfgWorker::Create over the loaded stores, attached to this store; refused with the block reason when the store
	// is blocked. The Err, either way, is already reported (OnError).
	scfg::Result<TUniquePtr<FScfgWorker>> CreateWorker(scfg::config::Config Config,
		TUniquePtr<FScfgTransport> Transport, FScfgWorker::FOptions WorkerOptions = FScfgWorker::FOptions());
	// Saves the worker's Snapshot after each of its OnStateChanged (see SAVING); one worker at a time (an earlier one
	// is detached). Detach() stops it; it is safe after the worker was destroyed.
	void Attach(FScfgWorker& Worker);
	void Detach();

	// Saves the stores (see SAVING): written now, or owed when only scan progress changed within the interval (true
	// both ways, and when the file already holds them). False when the store is blocked or the write failed: then it is
	// reported (once) and retried every RETRY_INTERVAL seconds.
	bool Save(const scfg::Json& InPending, const scfg::Json& InReceipts);
	// A deferred or failed save waits for the ticker.
	bool IsSaveOwed() const { return Owed.IsSet(); }
	// Files written by this store (the tests count them).
	int32 NumWrites() const { return Writes; }

private:
	friend struct FScfgStoreTestAccess;

	// Writes the stores to Path atomically (see SAVING): a dumps Err (ValueError), or a RuntimeError naming the step
	// that failed ("cannot create", "cannot write", "cannot replace"). Private: it has no blocked guard, so only the
	// store (which checks it) and the tests write files.
	static scfg::Status SaveFile(const FString& Path, const scfg::Json& Pending, const scfg::Json& Receipts);
	// Empty in a game. A test sets it to stand in for the final rename of a save (From is the .tmp, To the file), to
	// make that one step fail.
	static TFunction<scfg::Status(const FString& From, const FString& To)>& ReplaceHookForTests();

	// Writes Doc unless its text is on disk; reports a first failure. No ticker change. OnError may destroy the store
	// meanwhile: callers check their Alive copy before touching a member.
	bool TrySave(const scfg::Json& Doc);
	// The receipts, or the sessions' ids, states and hints, differ from what is on disk.
	bool IsUrgent(const scfg::Json& InPending, const scfg::Json& InReceipts) const;
	double Now() const;
	bool Retry(float DeltaTime);
	void ArmRetry();
	void DisarmRetry();
	void OnStateChanged();
	void ReportError(const scfg::Err& Error) const;
	void ReportWarning(const FString& Warning) const;

	FOptions Options;
	FString Path;
	scfg::Json Pending = scfg::Json::dict();
	scfg::Json Receipts = scfg::Json::dict();
	TOptional<scfg::Err> Blocked;
	// The text on disk as far as this store knows (written, or read and checked), and its receipts' text and its
	// pending key's (Godot's _pending_key: each sid's [state, hint_tx]); empty when unknown.
	std::string DiskText;
	std::string DiskReceiptsText;
	std::string DiskKeyText;
	// When the last write was attempted (Now()).
	double LastAttempt = -1e300;
	// The document a failed save still owes.
	TOptional<scfg::Json> Owed;
	bool bSaveFailing = false;  // the last save failed and was reported: report again only after a success
	int32 Writes = 0;
	FTSTicker::FDelegateHandle RetryHandle;
	// The attached worker (only read from inside its own OnStateChanged, while it is alive) and its handler.
	FScfgWorker* Worker = nullptr;
	TWeakPtr<FScfgStateChanged> Event;
	FDelegateHandle Handle;
	// False once the store is destroyed: read through a copy after calling OnError (see ERRORS).
	TSharedRef<bool> Alive = MakeShared<bool>(true);
};
