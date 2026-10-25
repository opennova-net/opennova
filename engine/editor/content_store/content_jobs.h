#pragma once

// STAGED, NOT WIRED: the session's content controller (session/content_controller.h, ADR 0069's
// editor slice E3) runs these jobs for the Content window and the editor's MCP.

// The content store's long operations as the editor runs them (ADR 0069): a pull and a push, each a
// cursor the session steps a budget at a time (as it steps a build), over the transport seam
// (content_transport.h), the folder's scan (content_scan.h), the plan (content_plan.h) and the
// apply (content_apply.h). A pull asks the project's head, reads the head's manifest, scans the
// folder, plans, stops for the author while a conflict stands, downloads the files the plan writes
// (a few at a time, each landed beside its name and kept once its SHA-256 is the file's), and
// applies the plan. A push asks the head (a head past the folder's base is a pull first), scans the
// folder, asks which of its files the store lacks, uploads them, and pushes the folder's manifest
// on its base; the folder's files are then its new base. A reply that renews the session renews the
// account the job holds; a reply that ends it (401) signs it out.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <base/content/content_manifest.h>
#include <editor/content_store/content_account.h>
#include <editor/content_store/content_api.h>
#include <editor/content_store/content_apply.h>
#include <editor/content_store/content_plan.h>
#include <editor/content_store/content_scan.h>
#include <editor/content_store/content_state.h>
#include <editor/content_store/content_transport.h>
#include <editor/project/project_document.h>

namespace opennova::editor {

enum class ContentJobState : uint8_t {
	Running,
	NeedsResolution, // a pull's conflicts: settle each (resolve), then resume
	Done,
	Failed, // `error` says why
	Cancelled,
};

// Where a job stands, for the status bar and the Content window.
struct ContentJobProgress {
	std::string label; // "Downloading 12 files"
	uint64_t done = 0;
	uint64_t total = 0;
	bool bytes = false; // done and total are bytes (else files)
};

// One request run to its reply over the transport, as a job's step polls it.
class ContentCall {
public:
	ContentCall() = default;
	ContentCall(ContentTransport &transport, ContentRequest request);
	ContentCall(ContentCall &&other) noexcept;
	ContentCall &operator=(ContentCall &&other) noexcept;
	ContentCall(const ContentCall &) = delete;
	ContentCall &operator=(const ContentCall &) = delete;
	~ContentCall();

	bool started() const { return transport_ != nullptr; }
	// Whether the reply came (true once; `reply()` holds it then).
	bool poll();
	bool done() const { return done_; }
	const ContentResponse &reply() const { return reply_; }

private:
	ContentTransport *transport_ = nullptr;
	uint64_t id_ = 0;
	bool done_ = false;
	ContentResponse reply_;
};

// What a job settled about its account: renewed, or signed out.
struct ContentAccountUpdate {
	bool changed = false;
	ContentAccount account;
};

class ContentJob {
public:
	virtual ~ContentJob() = default;
	// One step of at most `budget` bytes of the folder's reading (a request polled counts for
	// little).
	virtual ContentJobState step(uint64_t budget) = 0;
	virtual ContentJobProgress progress() const = 0;
	// The job given up on: its requests cancelled, nothing applied.
	virtual void cancel() = 0;

	ContentJobState state() const { return state_; }
	const std::string &error() const { return error_; }
	// The account as the job's replies left it (taken once by take_account).
	ContentAccountUpdate take_account();

protected:
	ContentJob(ContentTransport &transport, ContentAccount account) :
			transport_(transport), account_(std::move(account)) {}
	// A reply's word on the session: a renewed cookie kept, a 401 signing out. False for a 401.
	bool note_session(const ContentResponse &reply);
	ContentJobState fail(std::string error);

	ContentTransport &transport_;
	ContentAccount account_;
	bool account_changed_ = false;
	ContentJobState state_ = ContentJobState::Running;
	std::string error_;
};

// A pull of the project's head into the folder `paths.root` (an empty one getting a project, its
// binding then at revision 0).
class ContentPull : public ContentJob {
public:
	ContentPull(ContentTransport &transport, ContentAccount account, ProjectPaths paths,
			std::string export_dir, ContentBinding binding);

	ContentJobState step(uint64_t budget) override;
	ContentJobProgress progress() const override;
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

private:
	enum class Phase : uint8_t { AskHead, ReadManifest, Scan, Resolve, Download, Apply, Done };

	bool step_downloads();

	ProjectPaths paths_;
	ContentPaths content_paths_;
	std::string export_dir_;
	ContentBinding binding_;
	Phase phase_ = Phase::AskHead;
	ContentCall call_;
	RemoteProject remote_;
	std::optional<RemoteRevision> head_;
	std::vector<content::ContentFile> theirs_;
	std::unique_ptr<ContentScan> scan_;
	PullPlan plan_;
	std::vector<content::ContentFile> downloads_;
	size_t next_download_ = 0;
	std::vector<std::pair<content::ContentFile, ContentCall>> downloading_;
	uint64_t bytes_downloaded_ = 0;
	uint64_t bytes_to_download_ = 0;
	ApplyResult applied_;
	bool up_to_date_ = false;
};

// What a push came to.
enum class PushOutcome : uint8_t {
	Pushed, // the folder's files are revision `revision()`
	NothingToPush, // the folder holds its base's files
	Stale, // the project's head is past the folder's base: pull first
};

// A push of the folder's files as the revision after its base, with `message`.
class ContentPush : public ContentJob {
public:
	ContentPush(ContentTransport &transport, ContentAccount account, ProjectPaths paths,
			std::string export_dir, ContentBinding binding, std::string message);

	ContentJobState step(uint64_t budget) override;
	ContentJobProgress progress() const override;
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
	ContentPaths content_paths_;
	std::string export_dir_;
	ContentBinding binding_;
	std::string message_;
	Phase phase_ = Phase::AskHead;
	ContentCall call_;
	int64_t head_ = 0;
	std::unique_ptr<ContentScan> scan_;
	std::vector<content::ContentFile> files_;
	std::vector<LocalChange> changes_;
	size_t next_missing_ = 0; // the first file the next missing request names
	std::vector<UploadTarget> uploads_;
	size_t next_upload_ = 0;
	std::vector<std::pair<UploadTarget, ContentCall>> uploading_;
	uint64_t bytes_uploaded_ = 0;
	uint64_t bytes_to_upload_ = 0;
	PushOutcome outcome_ = PushOutcome::Pushed;
};

} // namespace opennova::editor
