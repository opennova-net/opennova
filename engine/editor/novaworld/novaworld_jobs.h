#pragma once

// NovaWorld's long operations as the editor runs them (ADR 0069): a pull and a push, each a
// cursor the session steps a budget at a time (as it steps a build), over the transport seam
// (novaworld_transport.h), the folder's scan (novaworld_scan.h), the plan (novaworld_plan.h) and the
// apply (novaworld_apply.h). A pull asks the project's head, reads the head's manifest, scans the
// folder, plans, stops for the author while a conflict stands, downloads the files the plan writes
// (a few at a time, each landed beside its name and kept once its SHA-256 is the file's), and
// applies the plan. A push asks the head (a head past the folder's base is a pull first), scans the
// folder, asks which of its files the store lacks, uploads them, and pushes the folder's manifest
// on its base; the folder's files are then its new base. A head before the folder's base (the store
// reset since) fails either: the folder binds again. A push also fails on a scan that could not
// list or read every file, which it would send as deleted. A reply that renews the session renews
// the account the job holds; a reply that ends it (401) signs it out.

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <base/project/project_manifest.h>
#include <editor/novaworld/novaworld_account.h>
#include <editor/novaworld/novaworld_api.h>
#include <editor/novaworld/novaworld_apply.h>
#include <editor/novaworld/novaworld_plan.h>
#include <editor/novaworld/novaworld_scan.h>
#include <editor/novaworld/novaworld_state.h>
#include <editor/novaworld/novaworld_transport.h>
#include <editor/project/project_document.h>

namespace opennova::editor {

// "<n> <one>" or "<n> <many>": a count in NovaWorld's words (its jobs' and the controller's status).
inline std::string count_text(size_t n, const char *one, const char *many) {
	return std::to_string(n) + " " + (n == 1 ? one : many);
}

enum class NovaWorldJobState : uint8_t {
	Running,
	NeedsResolution, // a pull's conflicts: settle each (resolve), then resume
	Done,
	Failed, // `error` says why
	Cancelled,
};

// Where a job stands, for the status bar and the NovaWorld window.
struct NovaWorldJobProgress {
	std::string label; // "Downloading 12 files"
	uint64_t done = 0;
	uint64_t total = 0;
	bool bytes = false; // done and total are bytes (else files)
};

// One request run to its reply over the transport, as a job's step polls it.
class NovaWorldCall {
public:
	NovaWorldCall() = default;
	NovaWorldCall(NovaWorldTransport &transport, NovaWorldRequest request);
	NovaWorldCall(NovaWorldCall &&other) noexcept;
	NovaWorldCall &operator=(NovaWorldCall &&other) noexcept;
	NovaWorldCall(const NovaWorldCall &) = delete;
	NovaWorldCall &operator=(const NovaWorldCall &) = delete;
	~NovaWorldCall();

	bool started() const { return transport_ != nullptr; }
	// Whether the reply came (true once; `reply()` holds it then).
	bool poll();
	bool done() const { return done_; }
	const NovaWorldResponse &reply() const { return reply_; }

private:
	NovaWorldTransport *transport_ = nullptr;
	uint64_t id_ = 0;
	bool done_ = false;
	NovaWorldResponse reply_;
};

// What a job settled about its account: renewed, or signed out.
struct NovaWorldAccountUpdate {
	bool changed = false;
	NovaWorldAccount account;
};

class NovaWorldJob {
public:
	virtual ~NovaWorldJob() = default;
	// One step of at most `budget` bytes of the folder's reading (a request polled counts for
	// little).
	virtual NovaWorldJobState step(uint64_t budget) = 0;
	virtual NovaWorldJobProgress progress() const = 0;
	// The job given up on: its requests cancelled, nothing applied.
	virtual void cancel() = 0;

	NovaWorldJobState state() const { return state_; }
	const std::string &error() const { return error_; }
	// The account as the job's replies left it (taken once by take_account).
	NovaWorldAccountUpdate take_account();

	// The bytes of files one batch of the service's signed URLs covers. A pull asks where a batch of files is,
	// downloads them, then asks of the next; a push asks which of a batch's files the store lacks, uploads those,
	// then asks of the next. The store signs a URL for an hour (the service's S3ProjectUrls::kExpiresS), so a URL
	// waits no longer than its own batch's transfers take: past the hour only below about 150 kbit/s.
	static constexpr uint64_t kSignedBatchBytes = uint64_t(64) << 20;
	// A batch's bounds: at most `files` (0: the route's own cap, the service's kDownloadsMax or kMissingMax) and
	// `bytes` of files, a file larger than that a batch of its own. kSignedBatchBytes unless set (a test's smaller).
	void limit_batches(size_t files, uint64_t bytes) {
		batch_files_ = files;
		batch_bytes_ = bytes;
	}

protected:
	NovaWorldJob(NovaWorldTransport &transport, NovaWorldAccount account) :
			transport_(transport), account_(std::move(account)) {}
	// A reply's word on the session: a renewed cookie kept, a 401 signing out. False for a 401.
	bool note_session(const NovaWorldResponse &reply);
	NovaWorldJobState fail(std::string error);

	NovaWorldTransport &transport_;
	NovaWorldAccount account_;
	size_t batch_files_ = 0;
	uint64_t batch_bytes_ = kSignedBatchBytes;
	bool account_changed_ = false;
	NovaWorldJobState state_ = NovaWorldJobState::Running;
	std::string error_;
};

// A pull of the project's head into the folder `paths.root` (an empty one getting a project, its
// binding then at revision 0).
// A restore (NovaWorldPull's second form): `paths` put back as `revision` has them (0: the folder's
// base, what Discard changes puts back), the base and the binding left as they are.
struct NovaWorldRestore {
	std::vector<std::string> paths;
	int64_t revision = 0;
};

class NovaWorldPull : public NovaWorldJob {
public:
	NovaWorldPull(NovaWorldTransport &transport, NovaWorldAccount account, ProjectPaths paths,
			std::string export_dir, NovaWorldBinding binding);
	// A restore of `restore.paths` (plan_restore), downloaded and applied as a pull's files are,
	// the replaced files to the trash.
	NovaWorldPull(NovaWorldTransport &transport, NovaWorldAccount account, ProjectPaths paths,
			std::string export_dir, NovaWorldBinding binding, NovaWorldRestore restore);

	NovaWorldJobState step(uint64_t budget) override;
	NovaWorldJobProgress progress() const override;
	void cancel() override;

	// While NeedsResolution: the plan, its conflicts listed; each settled, then resume().
	const PullPlan &plan() const { return plan_; }
	bool resolve(const std::string &path, Resolution resolution);
	void resume();

	// Once Done: the revision the folder holds now (its binding's), whether the pull changed
	// nothing (the folder was at the head already), and what the apply did.
	int64_t revision() const { return binding_.revision; }
	bool up_to_date() const { return up_to_date_; }
	const ApplyResult &applied() const { return applied_; }
	const RemoteProject &remote() const { return remote_; }
	// A restore's ask, null for a pull.
	const NovaWorldRestore *restore() const { return restore_ ? &*restore_ : nullptr; }
	// The revision a restore put its files back as (the base's number for Discard changes).
	int64_t restored_revision() const { return remote_.head; }

private:
	enum class Phase : uint8_t {
		AskHead,
		ReadManifest,
		Scan,
		Resolve,
		Locate,
		Download,
		Apply,
		Done
	};

	bool step_downloads();
	// Whether an earlier pull landed `file` already (its download kept by its SHA-256 at its size).
	bool landed(const project::ProjectFile &file) const;

	ProjectPaths paths_;
	NovaWorldPaths novaworld_paths_;
	std::string export_dir_;
	NovaWorldBinding binding_;
	Phase phase_ = Phase::AskHead;
	NovaWorldCall call_;
	RemoteProject remote_;
	std::optional<RemoteRevision> head_;
	std::vector<project::ProjectFile> theirs_;
	std::unique_ptr<NovaWorldScan> scan_;
	PullPlan plan_;
	std::vector<project::ProjectFile> downloads_;
	size_t next_locate_ = 0; // the first file the next downloads request names; those before it located
	std::map<std::string, NovaWorldRequest> requests_; // each file's download, by SHA-256
	size_t next_download_ = 0;
	std::vector<std::pair<project::ProjectFile, NovaWorldCall>> downloading_;
	uint64_t bytes_downloaded_ = 0;
	uint64_t bytes_to_download_ = 0;
	ApplyResult applied_;
	bool up_to_date_ = false;
	std::optional<NovaWorldRestore> restore_;
	std::vector<project::ProjectFile> base_; // a restore's: the base, which its apply keeps
};

// The folder's standing against its NovaWorld project: the project's head (asked of the service) and
// the folder's changes since its base (its files scanned), so a status says how many revisions the
// base is behind and what the folder changed. A service it cannot reach leaves the head unknown and
// still reads the changes.
class NovaWorldCheck : public NovaWorldJob {
public:
	NovaWorldCheck(NovaWorldTransport &transport, NovaWorldAccount account, ProjectPaths paths,
			std::string export_dir, NovaWorldBinding binding);

	NovaWorldJobState step(uint64_t budget) override;
	NovaWorldJobProgress progress() const override;
	void cancel() override;

	// Once Done.
	bool remote_known() const { return remote_known_; } // false: `remote_error()` says why
	const std::string &remote_error() const { return remote_error_; }
	const RemoteProject &remote() const { return remote_; }
	const std::vector<LocalChange> &changes() const { return changes_; }
	const std::vector<std::string> &problems() const { return problems_; }

private:
	enum class Phase : uint8_t { AskHead, Scan, Done };

	ProjectPaths paths_;
	std::string export_dir_;
	NovaWorldBinding binding_;
	Phase phase_ = Phase::AskHead;
	NovaWorldCall call_;
	bool remote_known_ = false;
	std::string remote_error_;
	RemoteProject remote_;
	std::unique_ptr<NovaWorldScan> scan_;
	std::vector<LocalChange> changes_;
	std::vector<std::string> problems_;
};

// The folder made a working copy of the project `binding.key` as it stands: its base the project's
// head (the head's manifest read) and its binding at the head, its files untouched, so whatever the
// folder holds otherwise is its change to push. What makes a folder kept before the store (a
// project on a disk, a stock install imported afresh) the project's working copy; for a project
// with no revision yet, its base is no file and every file of the folder is its first push.
class NovaWorldBind : public NovaWorldJob {
public:
	NovaWorldBind(NovaWorldTransport &transport, NovaWorldAccount account, ProjectPaths paths,
			NovaWorldBinding binding);

	NovaWorldJobState step(uint64_t budget) override;
	NovaWorldJobProgress progress() const override;
	void cancel() override;

	// Once Done: the binding written.
	const NovaWorldBinding &binding() const { return binding_; }

private:
	enum class Phase : uint8_t { AskHead, ReadManifest, Done };

	NovaWorldJobState write(const std::vector<project::ProjectFile> &base);

	ProjectPaths paths_;
	NovaWorldBinding binding_;
	Phase phase_ = Phase::AskHead;
	NovaWorldCall call_;
	RemoteProject remote_;
	std::optional<RemoteRevision> head_;
};

// What a push came to.
enum class PushOutcome : uint8_t {
	Pushed, // the folder's files are revision `revision()`
	NothingToPush, // the folder holds its base's files
	Stale, // the project's head is past the folder's base: pull first
};

// A push of the folder's files as the revision after its base, with `message`.
class NovaWorldPush : public NovaWorldJob {
public:
	NovaWorldPush(NovaWorldTransport &transport, NovaWorldAccount account, ProjectPaths paths,
			std::string export_dir, NovaWorldBinding binding, std::string message);

	NovaWorldJobState step(uint64_t budget) override;
	NovaWorldJobProgress progress() const override;
	void cancel() override;

	// Once Done.
	PushOutcome outcome() const { return outcome_; }
	int64_t revision() const { return binding_.revision; }
	int64_t head() const { return head_; } // the project's head as the push last saw it
	const std::vector<LocalChange> &changes() const { return changes_; }

private:
	enum class Phase : uint8_t { AskHead, Scan, Missing, Upload, Commit, Done };

	bool step_uploads();

	ProjectPaths paths_;
	NovaWorldPaths novaworld_paths_;
	std::string export_dir_;
	NovaWorldBinding binding_;
	std::string message_;
	Phase phase_ = Phase::AskHead;
	NovaWorldCall call_;
	int64_t head_ = 0;
	std::unique_ptr<NovaWorldScan> scan_;
	std::vector<project::ProjectFile> files_;
	std::vector<LocalChange> changes_;
	size_t next_missing_ = 0; // the first file the next missing request names
	std::vector<UploadTarget> uploads_;
	size_t next_upload_ = 0;
	std::vector<std::pair<UploadTarget, NovaWorldCall>> uploading_;
	uint64_t bytes_uploaded_ = 0;
	uint64_t bytes_to_upload_ = 0;
	PushOutcome outcome_ = PushOutcome::Pushed;
};

} // namespace opennova::editor
