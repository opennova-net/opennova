#include <editor/content_store/content_jobs.h>

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <unordered_map>

#include <base/io/file_io.h>
#include <editor/project/project_files.h>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

// What one request polled costs of a step's budget, as a directory entry listed does
// (assets/project_scan.h kWalkEntryCost).
constexpr uint64_t kCallCost = 4096;
// The files one missing request names: the service's ContentRoutes::kMissingMax.
constexpr size_t kMissingBatch = 20000;
// The files one downloads request names: the service's ContentRoutes::kDownloadsMax.
constexpr size_t kDownloadsBatch = 1000;

// A head before the folder's base: the store was reset (a NovaWorld database reset) since the
// folder last pulled or pushed, so its revision numbers name other revisions now.
std::string reset_text(const ContentBinding &binding, int64_t head) {
	return binding.key + "'s newest revision is " + std::to_string(head) +
			", before the folder's " + std::to_string(binding.revision) +
			": NovaWorld was reset, so bind the project again (the NovaWorld window's Bind again)";
}

} // namespace

// --- ContentCall ---

ContentCall::ContentCall(ContentTransport &transport, ContentRequest request) :
		transport_(&transport), id_(transport.start(std::move(request))) {}

ContentCall::ContentCall(ContentCall &&other) noexcept :
		transport_(other.transport_),
		id_(other.id_),
		done_(other.done_),
		reply_(std::move(other.reply_)) {
	other.transport_ = nullptr;
}

ContentCall &ContentCall::operator=(ContentCall &&other) noexcept {
	if (this != &other) {
		if (transport_ && !done_)
			transport_->cancel(id_);
		transport_ = other.transport_;
		id_ = other.id_;
		done_ = other.done_;
		reply_ = std::move(other.reply_);
		other.transport_ = nullptr;
	}
	return *this;
}

ContentCall::~ContentCall() {
	if (transport_ && !done_)
		transport_->cancel(id_);
}

bool ContentCall::poll() {
	if (done_)
		return true;
	if (!transport_)
		return false;
	done_ = transport_->poll(id_, reply_);
	return done_;
}

// --- ContentJob ---

ContentAccountUpdate ContentJob::take_account() {
	ContentAccountUpdate update{ account_changed_, account_ };
	account_changed_ = false;
	return update;
}

bool ContentJob::note_session(const ContentResponse &reply) {
	bool deleted = false;
	const std::string renewed = content_api::renewed_cookie(reply, deleted);
	if (!renewed.empty() && renewed != account_.cookie) {
		account_.cookie = renewed;
		account_changed_ = true;
	}
	if (reply.transport_ok && reply.status == 401)
		deleted = true;
	if (deleted && account_.signed_in()) {
		account_.cookie.clear();
		account_changed_ = true;
	}
	return !(reply.transport_ok && reply.status == 401);
}

ContentJobState ContentJob::fail(std::string error) {
	error_ = std::move(error);
	state_ = ContentJobState::Failed;
	return state_;
}

// --- ContentPull ---

ContentPull::ContentPull(ContentTransport &transport, ContentAccount account, ProjectPaths paths,
		std::string export_dir, ContentBinding binding) :
		ContentJob(transport, std::move(account)),
		paths_(std::move(paths)),
		content_paths_(ContentPaths::for_project(paths_)),
		export_dir_(std::move(export_dir)),
		binding_(std::move(binding)) {}

ContentPull::ContentPull(ContentTransport &transport, ContentAccount account, ProjectPaths paths,
		std::string export_dir, ContentBinding binding, ContentRestore restore) :
		ContentPull(transport, std::move(account), std::move(paths), std::move(export_dir),
				std::move(binding)) {
	restore_ = std::move(restore);
	remote_.key = binding_.key;
	remote_.head = restore_->revision > 0 ? restore_->revision : binding_.revision;
}

bool ContentPull::resolve(const std::string &path, Resolution resolution) {
	return state_ == ContentJobState::NeedsResolution && resolve_conflict(plan_, path, resolution);
}

void ContentPull::resume() {
	if (state_ != ContentJobState::NeedsResolution || plan_.conflicts() > 0)
		return;
	state_ = ContentJobState::Running;
	phase_ = Phase::Locate;
	downloads_ = plan_.downloads();
	for (const content::ContentFile &file : downloads_)
		bytes_to_download_ += file.size;
}

void ContentPull::cancel() {
	call_ = ContentCall();
	downloading_.clear();
	if (state_ == ContentJobState::Running || state_ == ContentJobState::NeedsResolution)
		state_ = ContentJobState::Cancelled;
}

ContentJobProgress ContentPull::progress() const {
	switch (phase_) {
		case Phase::AskHead:
			if (restore_)
				return { "Reading the base's files", 0, 0, false };
			return { "Asking " + binding_.key + "'s newest revision", 0, 0, false };
		case Phase::ReadManifest:
			return { "Reading revision " + std::to_string(remote_.head) + "'s files", 0, 0, false };
		case Phase::Scan:
			if (!scan_ || scan_->listing())
				return { "Listing the folder's files", scan_ ? scan_->files_listed() : 0, 0,
					false };
			return { "Reading the folder's files", scan_->bytes_hashed(), scan_->bytes_to_hash(),
				true };
		case Phase::Resolve:
			return { count_text(plan_.conflicts(), "conflict", "conflicts") + " to settle", 0, 0,
				false };
		case Phase::Locate:
			if (next_locate_ == 0)
				return { "Asking where " + count_text(downloads_.size(), "file is", "files are"), 0,
					downloads_.size(), false };
			// A later batch's ask, between its downloads.
			return { "Downloading " + count_text(downloads_.size(), "file", "files"),
				bytes_downloaded_, bytes_to_download_, true };
		case Phase::Download:
			return { "Downloading " + count_text(downloads_.size(), "file", "files"),
				bytes_downloaded_, bytes_to_download_, true };
		case Phase::Apply:
			if (restore_)
				return { "Putting " + count_text(restore_->paths.size(), "file", "files") +
								 " back as revision " + std::to_string(remote_.head) + " has them",
					0, 0, false };
			return { "Putting revision " + std::to_string(remote_.head) + " in place", 0, 0,
				false };
		case Phase::Done:
			return { restore_ ? "Restored" : "Pulled", 0, 0, false };
	}
	return {};
}

bool ContentPull::step_downloads() {
	std::error_code ec;
	fs::create_directories(system_path(content_paths_.incoming_dir), ec);
	// The replies come first: each landed file checked by its SHA-256, then named by it.
	for (auto it = downloading_.begin(); it != downloading_.end();) {
		if (!it->second.poll()) {
			++it;
			continue;
		}
		const ContentResponse &reply = it->second.reply();
		const content::ContentFile &file = it->first;
		const std::string landed = incoming_path(content_paths_, file.sha256);
		if (!note_session(reply)) {
			fail("the session ended: sign in again");
			return false;
		}
		ContentApiError error;
		if (!content_api::read_blob(reply, file, error)) {
			fail("cannot download " + file.path + ": " + error.line());
			return false;
		}
		std::string message;
		if (!io::replace_file(landed + ".part", landed, message)) {
			fail(message);
			return false;
		}
		bytes_downloaded_ += file.size;
		it = downloading_.erase(it);
	}
	// The files located so far: the batch asked of last.
	while (downloading_.size() < std::max<size_t>(transport_.parallel(), 1) &&
			next_download_ < next_locate_) {
		const content::ContentFile &file = downloads_[next_download_++];
		// A file an earlier pull landed (checked when it was named) is downloaded again by none.
		if (landed(file)) {
			bytes_downloaded_ += file.size;
			continue;
		}
		const auto located = requests_.find(file.sha256);
		if (located == requests_.end()) {
			fail("the service named no download of " + file.path);
			return false;
		}
		ContentRequest request = located->second;
		request.response_file = incoming_path(content_paths_, file.sha256) + ".part";
		downloading_.emplace_back(file, ContentCall(transport_, std::move(request)));
	}
	return downloading_.empty() && next_download_ == next_locate_;
}

bool ContentPull::landed(const content::ContentFile &file) const {
	std::error_code ec;
	const fs::path path = system_path(incoming_path(content_paths_, file.sha256));
	return fs::is_regular_file(path, ec) && fs::file_size(path, ec) == file.size;
}

ContentJobState ContentPull::step(uint64_t budget) {
	if (state_ != ContentJobState::Running)
		return state_;
	uint64_t spent = 0;
	do {
		switch (phase_) {
			case Phase::AskHead: {
				if (restore_) {
					// A restore asks no head: its revision is the base's (read here) or the one
					// asked, whose manifest is read next.
					std::string error;
					if (!read_content_base(content_paths_, base_, error))
						return fail(error);
					if (remote_.head == binding_.revision) {
						theirs_ = base_;
						scan_ = std::make_unique<ContentScan>(paths_, export_dir_);
						phase_ = Phase::Scan;
					} else {
						phase_ = Phase::ReadManifest;
					}
					break;
				}
				if (!call_.started())
					call_ = ContentCall(transport_, content_api::project(account_, binding_.key));
				spent += kCallCost;
				if (!call_.poll())
					return state_;
				if (!note_session(call_.reply()))
					return fail("the session ended: sign in again");
				ContentApiError error;
				if (!content_api::read_project(call_.reply(), remote_, head_, error))
					return fail("cannot read " + binding_.key + ": " + error.line());
				call_ = ContentCall();
				if (remote_.head < binding_.revision)
					return fail(reset_text(binding_, remote_.head));
				if (remote_.head == 0 || remote_.head == binding_.revision) {
					up_to_date_ = true;
					phase_ = Phase::Done;
					state_ = ContentJobState::Done;
					return state_;
				}
				phase_ = Phase::ReadManifest;
				break;
			}
			case Phase::ReadManifest: {
				if (!call_.started())
					call_ = ContentCall(transport_,
							content_api::manifest(account_, binding_.key, remote_.head));
				spent += kCallCost;
				if (!call_.poll())
					return state_;
				if (!note_session(call_.reply()))
					return fail("the session ended: sign in again");
				ContentApiError error;
				if (!content_api::read_manifest(call_.reply(),
							head_ ? head_->manifest_sha256 : std::string(), theirs_, error))
					return fail("cannot read revision " + std::to_string(remote_.head) + ": " +
							error.line());
				call_ = ContentCall();
				// A get's folder made before it is listed: an empty folder is a whole list of none,
				// a missing one no list at all.
				std::error_code made;
				fs::create_directories(system_path(paths_.root), made);
				scan_ = std::make_unique<ContentScan>(paths_, export_dir_);
				phase_ = Phase::Scan;
				break;
			}
			case Phase::Scan: {
				if (!scan_->step(budget > spent ? budget - spent : 1))
					return state_;
				// A file the list lacks would be planned as one the folder deleted, as a push would
				// send it.
				if (!scan_->incomplete().empty())
					return fail("the folder could not all be listed; a pull compares every file: " +
							scan_->incomplete());
				std::vector<content::ContentFile> base;
				std::string error;
				if (!read_content_base(content_paths_, base, error))
					return fail(error);
				plan_ = restore_ ? plan_restore(scan_->files(), scan_->unread(), theirs_,
										   restore_->paths)
								 : plan_pull(base, scan_->files(), scan_->unread(), theirs_);
				phase_ = Phase::Resolve;
				state_ = ContentJobState::NeedsResolution;
				if (plan_.conflicts() > 0)
					return state_;
				resume(); // nothing to settle: on to the downloads
				break;
			}
			case Phase::Resolve:
				return state_;
			case Phase::Locate: {
				// Where the next batch of files is downloaded from (the store's own URL, or the
				// service's route): the service's kDownloadsMax at most, and batch_bytes_ of files
				// (ContentJob::kSignedBatchBytes), downloaded before the next is asked of; a file an
				// earlier pull landed is asked of none.
				if (!call_.started()) {
					const size_t most = batch_files_ ? std::min(batch_files_, kDownloadsBatch) : kDownloadsBatch;
					std::vector<std::string> batch;
					uint64_t bytes = 0;
					while (next_locate_ < downloads_.size() && batch.size() < most) {
						const content::ContentFile &file = downloads_[next_locate_];
						if (!landed(file)) {
							if (!batch.empty() && bytes + file.size > batch_bytes_)
								break;
							batch.push_back(file.sha256);
							bytes += file.size;
						}
						++next_locate_;
					}
					if (batch.empty()) {
						phase_ = Phase::Download;
						break;
					}
					call_ = ContentCall(
							transport_, content_api::downloads(account_, binding_.key, batch));
				}
				spent += kCallCost;
				if (!call_.poll())
					return state_;
				if (!note_session(call_.reply()))
					return fail("the session ended: sign in again");
				ContentApiError error;
				requests_.clear(); // an earlier batch's, downloaded
				if (!content_api::read_downloads(call_.reply(), account_, requests_, error))
					return fail("cannot ask where the files are: " + error.line());
				call_ = ContentCall();
				phase_ = Phase::Download;
				break;
			}
			case Phase::Download: {
				spent += kCallCost;
				if (!step_downloads())
					return state_;
				// The batch landed: the next asked of, or the files put in place.
				phase_ = next_locate_ < downloads_.size() ? Phase::Locate : Phase::Apply;
				break;
			}
			case Phase::Apply: {
				std::string error;
				if (restore_) {
					// The base and the binding stay: a file put back as an older revision has it is
					// a change to push, one put back as the base's no change at all.
					if (!apply_pull(paths_, plan_, base_, binding_, applied_, error))
						return fail(error);
					phase_ = Phase::Done;
					state_ = ContentJobState::Done;
					return state_;
				}
				ContentBinding pulled = binding_;
				pulled.revision = remote_.head;
				if (!apply_pull(paths_, plan_, theirs_, pulled, applied_, error))
					return fail(error);
				binding_ = pulled;
				phase_ = Phase::Done;
				state_ = ContentJobState::Done;
				return state_;
			}
			case Phase::Done:
				return state_;
		}
	} while (spent < budget && state_ == ContentJobState::Running);
	return state_;
}

// --- ContentCheck ---

ContentCheck::ContentCheck(ContentTransport &transport, ContentAccount account, ProjectPaths paths,
		std::string export_dir, ContentBinding binding) :
		ContentJob(transport, std::move(account)),
		paths_(std::move(paths)),
		export_dir_(std::move(export_dir)),
		binding_(std::move(binding)) {}

void ContentCheck::cancel() {
	call_ = ContentCall();
	if (state_ == ContentJobState::Running)
		state_ = ContentJobState::Cancelled;
}

ContentJobProgress ContentCheck::progress() const {
	if (phase_ == Phase::AskHead)
		return { "Asking " + binding_.key + "'s newest revision", 0, 0, false };
	if (!scan_ || scan_->listing())
		return { "Listing the folder's files", scan_ ? scan_->files_listed() : 0, 0, false };
	return { "Reading the folder's files", scan_->bytes_hashed(), scan_->bytes_to_hash(), true };
}

ContentJobState ContentCheck::step(uint64_t budget) {
	if (state_ != ContentJobState::Running)
		return state_;
	uint64_t spent = 0;
	do {
		switch (phase_) {
			case Phase::AskHead: {
				if (!call_.started())
					call_ = ContentCall(transport_, content_api::project(account_, binding_.key));
				spent += kCallCost;
				if (!call_.poll())
					return state_;
				// The service's answer, or why there is none: the folder's own changes are read
				// either way.
				if (!note_session(call_.reply())) {
					remote_error_ = "the session ended: sign in again";
				} else {
					std::optional<RemoteRevision> head;
					ContentApiError error;
					remote_known_ = content_api::read_project(call_.reply(), remote_, head, error);
					if (!remote_known_)
						remote_error_ = error.line();
				}
				call_ = ContentCall();
				scan_ = std::make_unique<ContentScan>(paths_, export_dir_);
				phase_ = Phase::Scan;
				break;
			}
			case Phase::Scan: {
				if (!scan_->step(budget > spent ? budget - spent : 1))
					return state_;
				std::vector<content::ContentFile> base;
				std::string error;
				if (!read_content_base(ContentPaths::for_project(paths_), base, error))
					return fail(error);
				// A folder not listed whole holds files the list lacks: none taken for deleted.
				changes_ = local_changes(
						base, scan_->files(), scan_->unread(), scan_->incomplete().empty());
				problems_ = scan_->problems();
				phase_ = Phase::Done;
				state_ = ContentJobState::Done;
				return state_;
			}
			case Phase::Done:
				return state_;
		}
	} while (spent < budget && state_ == ContentJobState::Running);
	return state_;
}

// --- ContentBind ---

ContentBind::ContentBind(ContentTransport &transport, ContentAccount account, ProjectPaths paths,
		ContentBinding binding) :
		ContentJob(transport, std::move(account)),
		paths_(std::move(paths)),
		binding_(std::move(binding)) {}

void ContentBind::cancel() {
	call_ = ContentCall();
	if (state_ == ContentJobState::Running)
		state_ = ContentJobState::Cancelled;
}

ContentJobProgress ContentBind::progress() const {
	if (phase_ == Phase::AskHead)
		return { "Asking " + binding_.key + "'s newest revision", 0, 0, false };
	return { "Reading revision " + std::to_string(remote_.head) + "'s files", 0, 0, false };
}

ContentJobState ContentBind::write(const std::vector<content::ContentFile> &base) {
	const ContentPaths content_paths = ContentPaths::for_project(paths_);
	binding_.revision = remote_.head;
	std::string error;
	if (!write_content_base(content_paths, base, error) ||
			!write_content_binding(content_paths, binding_, error))
		return fail("the folder's binding is not written: " + error);
	phase_ = Phase::Done;
	state_ = ContentJobState::Done;
	return state_;
}

ContentJobState ContentBind::step(uint64_t budget) {
	if (state_ != ContentJobState::Running)
		return state_;
	uint64_t spent = 0;
	do {
		switch (phase_) {
			case Phase::AskHead: {
				if (!call_.started())
					call_ = ContentCall(transport_, content_api::project(account_, binding_.key));
				spent += kCallCost;
				if (!call_.poll())
					return state_;
				if (!note_session(call_.reply()))
					return fail("the session ended: sign in again");
				ContentApiError error;
				if (!content_api::read_project(call_.reply(), remote_, head_, error))
					return fail("cannot read " + binding_.key + ": " + error.line());
				call_ = ContentCall();
				if (remote_.head == 0)
					return write({}); // no revision yet: every file of the folder is its first push
				phase_ = Phase::ReadManifest;
				break;
			}
			case Phase::ReadManifest: {
				if (!call_.started())
					call_ = ContentCall(transport_,
							content_api::manifest(account_, binding_.key, remote_.head));
				spent += kCallCost;
				if (!call_.poll())
					return state_;
				if (!note_session(call_.reply()))
					return fail("the session ended: sign in again");
				std::vector<content::ContentFile> base;
				ContentApiError error;
				if (!content_api::read_manifest(call_.reply(),
							head_ ? head_->manifest_sha256 : std::string(), base, error))
					return fail("cannot read revision " + std::to_string(remote_.head) + ": " +
							error.line());
				call_ = ContentCall();
				return write(base);
			}
			case Phase::Done:
				return state_;
		}
	} while (spent < budget && state_ == ContentJobState::Running);
	return state_;
}

// --- ContentPush ---

ContentPush::ContentPush(ContentTransport &transport, ContentAccount account, ProjectPaths paths,
		std::string export_dir, ContentBinding binding, std::string message) :
		ContentJob(transport, std::move(account)),
		paths_(std::move(paths)),
		content_paths_(ContentPaths::for_project(paths_)),
		export_dir_(std::move(export_dir)),
		binding_(std::move(binding)),
		message_(std::move(message)) {}

void ContentPush::cancel() {
	call_ = ContentCall();
	uploading_.clear();
	if (state_ == ContentJobState::Running)
		state_ = ContentJobState::Cancelled;
}

ContentJobProgress ContentPush::progress() const {
	switch (phase_) {
		case Phase::AskHead:
			return { "Asking " + binding_.key + "'s newest revision", 0, 0, false };
		case Phase::Scan:
			if (!scan_ || scan_->listing())
				return { "Listing the folder's files", scan_ ? scan_->files_listed() : 0, 0,
					false };
			return { "Reading the folder's files", scan_->bytes_hashed(), scan_->bytes_to_hash(),
				true };
		case Phase::Missing:
			return { "Asking which files NovaWorld lacks", 0, 0, false };
		case Phase::Upload:
			return { "Uploading " + count_text(uploads_.size(), "file", "files"), bytes_uploaded_,
				bytes_to_upload_, true };
		case Phase::Commit:
			return { "Pushing revision " + std::to_string(binding_.revision + 1), 0, 0, false };
		case Phase::Done:
			return { "Pushed", 0, 0, false };
	}
	return {};
}

bool ContentPush::step_uploads() {
	for (auto it = uploading_.begin(); it != uploading_.end();) {
		if (!it->second.poll()) {
			++it;
			continue;
		}
		if (!note_session(it->second.reply())) {
			fail("the session ended: sign in again");
			return false;
		}
		ContentApiError error;
		if (!content_api::read_upload(it->second.reply(), error)) {
			fail("cannot upload a file of " + std::to_string(it->first.file.size) +
					" bytes: " + error.line());
			return false;
		}
		bytes_uploaded_ += it->first.file.size;
		it = uploading_.erase(it);
	}
	std::unordered_map<std::string, std::string> path_of_sha;
	for (const content::ContentFile &file : files_)
		path_of_sha.emplace(file.sha256, file.path);
	while (uploading_.size() < std::max<size_t>(transport_.parallel(), 1) &&
			next_upload_ < uploads_.size()) {
		const UploadTarget &target = uploads_[next_upload_++];
		const auto path = path_of_sha.find(target.file.sha256);
		if (path == path_of_sha.end()) {
			fail("NovaWorld asked for a file the folder does not hold");
			return false;
		}
		const std::string file = join_path(paths_.root, path->second);
		uploading_.emplace_back(
				target, ContentCall(transport_, content_api::upload(account_, target, file)));
	}
	return uploading_.empty() && next_upload_ == uploads_.size();
}

ContentJobState ContentPush::step(uint64_t budget) {
	if (state_ != ContentJobState::Running)
		return state_;
	uint64_t spent = 0;
	do {
		switch (phase_) {
			case Phase::AskHead: {
				if (!call_.started())
					call_ = ContentCall(transport_, content_api::project(account_, binding_.key));
				spent += kCallCost;
				if (!call_.poll())
					return state_;
				if (!note_session(call_.reply()))
					return fail("the session ended: sign in again");
				RemoteProject remote;
				std::optional<RemoteRevision> head;
				ContentApiError error;
				if (!content_api::read_project(call_.reply(), remote, head, error))
					return fail("cannot read " + binding_.key + ": " + error.line());
				call_ = ContentCall();
				if (!remote.can_push)
					return fail("this account may not push " + binding_.key + ": an admin pushes");
				head_ = remote.head;
				if (head_ < binding_.revision)
					return fail(reset_text(binding_, head_));
				if (head_ != binding_.revision) {
					outcome_ = PushOutcome::Stale;
					phase_ = Phase::Done;
					state_ = ContentJobState::Done;
					return state_;
				}
				scan_ = std::make_unique<ContentScan>(paths_, export_dir_);
				phase_ = Phase::Scan;
				break;
			}
			case Phase::Scan: {
				if (!scan_->step(budget > spent ? budget - spent : 1))
					return state_;
				if (!scan_->incomplete().empty())
					return fail("the folder could not all be listed; a push sends every file: " +
							scan_->incomplete());
				if (!scan_->unread().empty())
					return fail(count_text(scan_->unread().size(), "file", "files") +
							" of the folder could not be read; a push sends every file: " +
							scan_->problems().front());
				std::vector<content::ContentFile> base;
				std::string error;
				if (!read_content_base(content_paths_, base, error))
					return fail(error);
				files_ = scan_->files();
				changes_ = local_changes(base, files_, {});
				if (changes_.empty()) {
					outcome_ = PushOutcome::NothingToPush;
					phase_ = Phase::Done;
					state_ = ContentJobState::Done;
					return state_;
				}
				phase_ = Phase::Missing;
				break;
			}
			case Phase::Missing: {
				// At most the service's kMissingMax files a request (project_routes.h) and batch_bytes_
				// of files (ContentJob::kSignedBatchBytes), what the store lacks of them uploaded before
				// the next is asked of.
				if (!call_.started()) {
					const size_t most = batch_files_ ? std::min(batch_files_, kMissingBatch) : kMissingBatch;
					size_t end = next_missing_;
					uint64_t bytes = 0;
					while (end < files_.size() && end - next_missing_ < most &&
							(end == next_missing_ || bytes + files_[end].size <= batch_bytes_))
						bytes += files_[end++].size;
					const std::vector<content::ContentFile> batch(
							files_.begin() + static_cast<std::ptrdiff_t>(next_missing_),
							files_.begin() + static_cast<std::ptrdiff_t>(end));
					call_ = ContentCall(
							transport_, content_api::missing(account_, binding_.key, batch));
					next_missing_ = end;
				}
				spent += kCallCost;
				if (!call_.poll())
					return state_;
				if (!note_session(call_.reply()))
					return fail("the session ended: sign in again");
				std::vector<UploadTarget> lacking;
				ContentApiError error;
				if (!content_api::read_missing(call_.reply(), account_, lacking, error))
					return fail("cannot ask which files NovaWorld lacks: " + error.line());
				call_ = ContentCall();
				for (UploadTarget &target : lacking) {
					bytes_to_upload_ += target.file.size;
					uploads_.push_back(std::move(target));
				}
				phase_ = Phase::Upload;
				break;
			}
			case Phase::Upload: {
				spent += kCallCost;
				if (!step_uploads())
					return state_;
				// The batch's uploads stored: the next asked of, or the revision pushed.
				phase_ = next_missing_ < files_.size() ? Phase::Missing : Phase::Commit;
				break;
			}
			case Phase::Commit: {
				if (!call_.started())
					call_ = ContentCall(transport_,
							content_api::push(account_, binding_.key, binding_.revision, message_,
									content::encode_manifest(files_)));
				spent += kCallCost;
				if (!call_.poll())
					return state_;
				if (!note_session(call_.reply()))
					return fail("the session ended: sign in again");
				int64_t head = 0;
				std::vector<std::string> hashes;
				ContentApiError error;
				const content_api::PushAnswer answer =
						content_api::read_push(call_.reply(), head, hashes, error);
				call_ = ContentCall();
				if (answer == content_api::PushAnswer::Stale) {
					head_ = head;
					outcome_ = PushOutcome::Stale;
					phase_ = Phase::Done;
					state_ = ContentJobState::Done;
					return state_;
				}
				if (answer == content_api::PushAnswer::MissingBlobs)
					return fail(count_text(hashes.size(), "file", "files") +
							" did not reach NovaWorld: push again");
				if (answer != content_api::PushAnswer::Pushed)
					return fail("the push was refused: " + error.line());
				ContentBinding pushed = binding_;
				pushed.revision = head;
				std::string message;
				if (!write_content_base(content_paths_, files_, message) ||
						!write_content_binding(content_paths_, pushed, message))
					return fail("revision " + std::to_string(head) +
							" is pushed, but the folder's base is not written: " + message);
				binding_ = pushed;
				head_ = head;
				outcome_ = PushOutcome::Pushed;
				phase_ = Phase::Done;
				state_ = ContentJobState::Done;
				return state_;
			}
			case Phase::Done:
				return state_;
		}
	} while (spent < budget && state_ == ContentJobState::Running);
	return state_;
}

} // namespace opennova::editor
