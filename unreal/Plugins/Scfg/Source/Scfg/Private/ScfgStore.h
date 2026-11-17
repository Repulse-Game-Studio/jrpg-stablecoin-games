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
//     save stopped between its delete and its rename, see SAVING), the .tmp is read instead.
//   - A file that exists but cannot be read (a permission, a lock, an I/O error, a directory in its place) is an
//     Error (RuntimeError naming the file): the store is BLOCKED. A blocked store starts no worker and never writes,
//     so what is on disk is never replaced by empty stores; the next launch tries again.
//   - A file that is not that document (not UTF-8 JSON, not an object, a version other than the int 1, a pending or
//     receipts that is not an object) is moved aside to <path>.corrupt, then .corrupt.2, ..., never over an earlier
//     copy, and the stores start empty with a warning (players can bring purchases back with Restore). A bad file
//     that cannot be moved aside is an Error (blocked) instead: an empty start would let the next save replace it
//     with nothing kept anywhere.
//   - Each pending record is checked against the frozen schema (state_schema.check's pending rules: every required
//     field, the right types, the enums, no unknown field, the match object, and an id equal to its key). A record
//     that fails is dropped with a warning naming it and what is wrong, and the whole file is first copied aside as
//     above (copied, not moved), so the record is kept for support; the cleaned stores are then saved. A malformed
//     record would otherwise fail every tick (Engine::tick's expiry pass is not contained per session). Receipts are
//     not checked here: the core tolerates any receipt value (compute_ownership skips it) and the first reverify
//     removes an invalid one (vectors reverify/malformed_entries_are_removed, invalid_receipts_are_removed).
//     Godot's store checks neither; this is the record-level half of "a file that is not that document".
//
// SAVING (Save, and after each OnStateChanged of the attached worker): the text is compared with what was last written
// (or read), and nothing is written when it is the same (the worker fires after every tick). Otherwise <path>.tmp is
// written and flushed (fdatasync), then IFileManager::Move replaces <path> with it. Move deletes the target before it
// renames, so a crash between the two leaves only the complete .tmp, which loading reads. A failed save (a dumps Err
// such as an int of more than 4300 digits, a write or a rename that fails) keeps the old file, is reported once (not
// at every retry, until a save succeeds), and is retried every RETRY_INTERVAL seconds from the ticker; a newer state
// replaces the one owed. Destroying the store makes one last attempt at a save it still owes (the game quitting).
// (The engine itself logs a LogUnixPlatformFile warning for every open() that fails other than with ENOENT, so on
// Linux a retry that keeps failing to write adds one engine line per attempt; ours stays at one.)
//
// ERRORS. Every Err and warning goes to the options' OnError / OnWarning (default: LogScfg), once; callers must not
// log them again. That includes CreateWorker's Err (the block reason, or FScfgWorker::Create's).
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
};

class SCFG_API FScfgStore
{
public:
	static constexpr int64 VERSION = 1;
	// Seconds between the attempts of a failed save (Godot's PENDING_SAVE_INTERVAL).
	static constexpr float RETRY_INTERVAL = 10.0f;

	using FOptions = FScfgStoreOptions;

	// FPaths::ProjectSavedDir()/Scfg/state.json, as an absolute path.
	static FString DefaultPath();
	// The stores in Path (see LOADING). Never fails: a bad file is a warning, an unreadable one an Error. A corrupt
	// file is moved aside, a file with a malformed pending record copied aside.
	static FScfgStoreLoad Load(const FString& Path);
	// Writes the stores to Path atomically (see SAVING): a dumps Err (ValueError), or a RuntimeError naming the step
	// that failed ("cannot create", "cannot write", "cannot replace").
	static scfg::Status SaveFile(const FString& Path, const scfg::Json& Pending, const scfg::Json& Receipts);
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

	// Saves the stores now, unless the text is what was last written (true then). False when the store is blocked or
	// the save failed: then it is reported (once) and retried every RETRY_INTERVAL seconds.
	bool Save(const scfg::Json& InPending, const scfg::Json& InReceipts);
	// A failed save waits for its retry.
	bool IsSaveOwed() const { return Owed.IsSet(); }
	// Files written by this store (the tests count them).
	int32 NumWrites() const { return Writes; }

private:
	// Writes Doc unless its text is the last written one; reports a first failure. No ticker change.
	bool TrySave(const scfg::Json& Doc);
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
	// The text on disk as far as this store knows (written, or read and checked); empty when unknown.
	std::string LastText;
	// The document a failed save still owes.
	TOptional<scfg::Json> Owed;
	bool bSaveFailing = false;  // the last save failed and was reported: report again only after a success
	int32 Writes = 0;
	FTSTicker::FDelegateHandle RetryHandle;
	// The attached worker (only read from inside its own OnStateChanged, while it is alive) and its handler.
	FScfgWorker* Worker = nullptr;
	TWeakPtr<FScfgStateChanged> Event;
	FDelegateHandle Handle;
};
