#include <editor/session/content_controller.h>

#include <filesystem>
#include <system_error>
#include <utility>

#include <base/content/content_manifest.h>
#include <editor/import/import_run.h>
#include <editor/model/core_finding.h>
#include <editor/model/diagnostic.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>
#include <editor/session/content_operation.h>
#include <editor/session/disk_watch.h>
#include <editor/session/session_core.h>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

std::string count_text(size_t n, const char *one, const char *many) {
	return std::to_string(n) + " " + (n == 1 ? one : many);
}

// Whether the folder at `dir` may take a get: missing, empty, or holding the machine's own folder
// alone (a get cut short before its first file).
bool empty_folder(const std::string &dir) {
	std::error_code ec;
	const fs::path path = system_path(dir);
	if (!fs::exists(path, ec))
		return true;
	if (!fs::is_directory(path, ec))
		return false;
	for (const auto &entry : fs::directory_iterator(path, ec))
		if (entry.path().filename() != ".opennova")
			return false;
	return !ec;
}

// A release's version: one to four numbers between dots ("0.2.0", "1.7.5.8"), as the service's
// route takes it (apps/novaworld_web/content/content_routes.cpp valid_version).
bool release_version(const std::string &version) {
	if (version.empty() || version.size() > 32)
		return false;
	int dots = 0;
	size_t digits = 0;
	for (const char c : version) {
		if (c >= '0' && c <= '9') {
			++digits;
			continue;
		}
		if (c != '.' || digits == 0)
			return false;
		++dots;
		digits = 0;
	}
	return digits > 0 && dots < 4;
}

} // namespace

ContentController::ContentController(SessionCore &core) : core_(core), accounts_(&memory_) {}

void ContentController::set_transport(ContentTransport *transport) {
	transport_ = transport;
	core_.view().content.available = transport != nullptr;
	core_.touch(ViewConcern::Content);
}

void ContentController::set_accounts(ContentAccountStore *accounts) {
	accounts_ = accounts ? accounts : &memory_;
	account_ = accounts_->load();
	show_account();
}

void ContentController::show_account() {
	ContentView &view = core_.view().content;
	view.server = account_.server;
	view.username = account_.username;
	view.role = account_.signed_in() ? account_.role : std::string();
	view.signed_in = account_.signed_in();
	core_.touch(ViewConcern::Content);
}

void ContentController::keep_account(const ContentAccountUpdate &update) {
	if (!update.changed)
		return;
	account_ = update.account;
	std::string error;
	if (!accounts_->save(account_, error))
		core_.note("The content account was not kept: " + error);
	show_account();
}

void ContentController::refuse(const std::string &why) {
	core_.refuse_request(CoreFinding::ContentRefused, why);
}

bool ContentController::ready(bool admin, bool project, bool bound) {
	if (!transport_) {
		refuse("This editor reaches no content store.");
		return false;
	}
	if (admin && !account_.admin()) {
		refuse(account_.signed_in() ? "Only an admin pushes or binds a project for pushing."
									: "Sign in to the content store first.");
		return false;
	}
	if (project && !core_.view().project.open) {
		refuse("Open a project first.");
		return false;
	}
	if (bound && !core_.view().content.bound) {
		refuse("The project is no working copy of a content project yet: bind it first.");
		return false;
	}
	return true;
}

std::string ContentController::export_dir() const {
	const auto &document = core_.view().project.document;
	return document ? core_.paths().export_dir(*document) : std::string();
}

void ContentController::start(OperationKind kind, std::unique_ptr<ContentJob> job) {
	if (core_.start_operation(std::make_unique<ContentOperation>(kind, *this, std::move(job))) == 0)
		core_.refuse_busy(std::string());
	core_.touch(ViewConcern::Content);
}

void ContentController::sign_in(const EditorRequest &request) {
	if (!transport_) {
		refuse("This editor reaches no content store.");
		return;
	}
	const std::string server =
			content_server(request.server.empty() ? account_.server : request.server);
	if (server.empty() || request.username.empty() || request.password.empty()) {
		refuse("Sign in names the service, the account and its password.");
		return;
	}
	login_server_ = server;
	login_username_ = request.username;
	login_ = ContentCall(
			*transport_, content_api::login(server, request.username, request.password));
	core_.view().content.calling = true;
	core_.touch(ViewConcern::Content);
}

void ContentController::sign_out() {
	if (transport_ && account_.signed_in())
		logout_ = ContentCall(*transport_, content_api::logout(account_)); // its answer is no one's
	ContentAccountUpdate update{ true, account_ };
	update.account.cookie.clear();
	update.account.role.clear();
	keep_account(update);
	core_.view().content.last = "Signed out of " + account_.server + ".";
	list_projects();
}

void ContentController::list_projects() {
	if (!transport_ || account_.server.empty())
		return;
	projects_ = ContentCall(*transport_, content_api::projects(account_));
	core_.view().content.calling = true;
	core_.touch(ViewConcern::Content);
}

void ContentController::check() {
	if (!ready(false, false, false))
		return;
	if (account_.server.empty()) {
		refuse("Sign in to a content store first, or name its server.");
		return;
	}
	list_projects();
	if (!core_.view().project.open || !core_.view().content.bound)
		return;
	ContentBinding binding;
	std::string error;
	read_content_binding(ContentPaths::for_project(core_.paths()), binding, error);
	start(OperationKind::ContentCheck,
			std::make_unique<ContentCheck>(
					*transport_, account_, core_.paths(), export_dir(), binding));
}

void ContentController::get(const EditorRequest &request) {
	if (!ready(false, false, false))
		return;
	if (request.dir.empty() || request.project_key.empty()) {
		refuse("A get names the content project and the folder it goes into.");
		return;
	}
	if (!empty_folder(request.dir)) {
		refuse("The folder " + request.dir +
				" holds files: a project is got into an empty folder.");
		return;
	}
	ContentBinding binding{ account_.server, request.project_key, 0 };
	get_dir_ = request.dir;
	paused_.reset();
	start(OperationKind::ContentPull,
			std::make_unique<ContentPull>(
					*transport_, account_, ProjectPaths::for_root(request.dir), "", binding));
}

void ContentController::bind(const EditorRequest &request) {
	if (!ready(false, true, false))
		return;
	if (request.project_key.empty()) {
		refuse("A bind names the content project the open project is a working copy of.");
		return;
	}
	start(OperationKind::ContentBind,
			std::make_unique<ContentBind>(*transport_, account_, core_.paths(),
					ContentBinding{ account_.server, request.project_key, 0 }));
}

void ContentController::pull() {
	if (!ready(false, true, true))
		return;
	if (paused_) {
		if (paused_->plan().conflicts() > 0) {
			refuse("Settle every conflict first: keep yours or take theirs (content_resolve).");
			return;
		}
		paused_->resume();
		start(OperationKind::ContentPull, std::move(paused_));
		return;
	}
	ContentBinding binding;
	std::string error;
	if (!read_content_binding(ContentPaths::for_project(core_.paths()), binding, error)) {
		refuse(error.empty() ? "The project's binding is gone: bind it again." : error);
		return;
	}
	get_dir_.clear();
	start(OperationKind::ContentPull,
			std::make_unique<ContentPull>(
					*transport_, account_, core_.paths(), export_dir(), binding));
}

void ContentController::resolve(const EditorRequest &request) {
	if (!paused_) {
		refuse("No pull waits on a conflict.");
		return;
	}
	if (!request.resolution || !paused_->resolve(request.path, *request.resolution)) {
		refuse("The pull has no conflict at " + request.path + ".");
		return;
	}
	show_conflicts();
	if (paused_->plan().conflicts() == 0)
		core_.view().content.last = "Every conflict is settled: pull to go on.";
	core_.touch(ViewConcern::Content);
}

void ContentController::push(const EditorRequest &request) {
	if (!ready(true, true, true))
		return;
	if (!content::is_utf8(request.message) || request.message.empty() ||
			request.message.size() > 2000) {
		refuse("A push names what it changes: a message of 1 to 2000 bytes.");
		return;
	}
	if (paused_) {
		refuse("A pull waits on its conflicts: settle them and pull before pushing.");
		return;
	}
	ContentBinding binding;
	std::string error;
	if (!read_content_binding(ContentPaths::for_project(core_.paths()), binding, error)) {
		refuse(error.empty() ? "The project's binding is gone: bind it again." : error);
		return;
	}
	start(OperationKind::ContentPush,
			std::make_unique<ContentPush>(
					*transport_, account_, core_.paths(), export_dir(), binding, request.message));
}

void ContentController::release(const EditorRequest &request) {
	if (!ready(true, true, true))
		return;
	const ContentView &view = core_.view().content;
	if (!release_version(request.version)) {
		refuse("A release names its version: one to four numbers between dots (0.2.0).");
		return;
	}
	if (view.revision <= 0) {
		refuse("Push the project first: a release is built from a revision of the store's.");
		return;
	}
	if (release_.started()) {
		refuse("A release is being asked for already.");
		return;
	}
	release_server_ = account_.server;
	release_ = ContentCall(*transport_,
			content_api::release(account_, view.key, request.version, view.revision));
	core_.view().content.calling = true;
	core_.touch(ViewConcern::Content);
}

void ContentController::cancel() {
	// A content job running gave way to this request at the gate (its row's superseded_by); a pull
	// kept for its conflicts goes too.
	if (paused_) {
		paused_.reset();
		get_dir_.clear();
		core_.view().content.conflicts.clear();
		core_.view().content.pull_revision = 0;
		core_.view().content.last = "The pull was given up.";
	}
	core_.touch(ViewConcern::Content);
}

void ContentController::poll() {
	ContentView &view = core_.view().content;
	if (logout_.started() && logout_.poll())
		logout_ = ContentCall();
	if (login_.started() && login_.poll()) {
		ContentAccount account;
		account.server = login_server_;
		account.username = login_username_;
		ContentApiError error;
		if (content_api::read_login(login_.reply(), account, error)) {
			keep_account({ true, account });
			view.last = "Signed in to " + account.server + " as " + account.username + ".";
			core_.note("Content store: " + view.last);
			list_projects();
		} else {
			view.last = "The content store's sign-in failed: " + error.line();
			core_.refuse_request(make_finding(
					CoreFinding::ContentFailed, DiagnosticSeverity::Warning, view.last));
		}
		login_ = ContentCall();
		core_.touch(ViewConcern::Content);
	}
	if (projects_.started() && projects_.poll()) {
		ContentApiError error;
		std::vector<RemoteProject> projects;
		if (content_api::read_projects(projects_.reply(), projects, error)) {
			view.projects = std::move(projects);
			view.projects_error.clear();
		} else {
			view.projects_error = error.line();
		}
		projects_ = ContentCall();
		core_.touch(ViewConcern::Content);
	}
	if (release_.started() && release_.poll()) {
		ContentApiError error;
		int64_t id = 0;
		std::string name;
		if (content_api::read_release(release_.reply(), id, name, error)) {
			view.last = "Release " + name + " is building on " + release_server_ +
					": publish it in the web admin's Game data once it is a draft.";
			core_.note("Content store: " + view.last);
		} else {
			view.last = "The release was refused: " + error.line();
			core_.refuse_request(make_finding(
					CoreFinding::ContentFailed, DiagnosticSeverity::Warning, view.last));
		}
		release_ = ContentCall();
		core_.touch(ViewConcern::Content);
	}
	const bool calling = login_.started() || projects_.started() || release_.started();
	if (view.calling != calling) {
		view.calling = calling;
		core_.touch(ViewConcern::Content);
	}
}

bool ContentController::busy() const {
	return login_.started() || projects_.started() || release_.started();
}

void ContentController::show_binding() {
	ContentView &view = core_.view().content;
	ContentBinding binding;
	std::string error;
	view.bound = core_.view().project.open &&
			read_content_binding(ContentPaths::for_project(core_.paths()), binding, error);
	view.key = view.bound ? binding.key : std::string();
	view.revision = view.bound ? binding.revision : 0;
	core_.touch(ViewConcern::Content);
}

void ContentController::show_conflicts() {
	ContentView &view = core_.view().content;
	view.conflicts.clear();
	if (paused_) {
		for (const PullItem &item : paused_->plan().items)
			if (item.step == PullStep::Conflict)
				view.conflicts.push_back({ item.path, item.mine_change, item.theirs_change });
		view.pull_revision = paused_->remote().head;
	} else {
		view.pull_revision = 0;
	}
	core_.touch(ViewConcern::Content);
}

void ContentController::project_opened() {
	show_binding();
	ContentView &view = core_.view().content;
	view.checked = false;
	view.changes.clear();
	view.problems.clear();
}

void ContentController::forget_project() {
	if (get_dir_.empty())
		paused_.reset();
	ContentView &view = core_.view().content;
	view.bound = false;
	view.key.clear();
	view.revision = 0;
	view.checked = false;
	view.head_known = false;
	view.changes.clear();
	view.problems.clear();
	show_conflicts();
}

void ContentController::finish_pull(ContentPull &pull) {
	ContentView &view = core_.view().content;
	if (pull.up_to_date()) {
		view.last = "The project is at the head, revision " + std::to_string(pull.revision()) + ".";
		show_binding();
		return;
	}
	view.last = "Pulled revision " + std::to_string(pull.revision()) + ": " +
			count_text(pull.applied().written, "file written", "files written") + ", " +
			count_text(pull.applied().removed, "removed", "removed") + ".";
	core_.note("Content store: " + view.last);
	view.head = pull.revision();
	view.head_known = true;
	if (!get_dir_.empty()) {
		const std::string dir = std::move(get_dir_);
		get_dir_.clear();
		core_.open_project(dir);
		return;
	}
	show_binding();
	// The files the pull wrote and removed, read again as an outside change is; a changed project
	// file is the project opened again.
	std::vector<std::string> changed;
	bool project_file = false;
	for (const PullItem &item : pull.plan().items) {
		changed.push_back(item.path);
		if (item.path == kProjectFileName)
			project_file = true;
	}
	if (project_file) {
		core_.open_project(core_.paths().root);
		return;
	}
	ExternalChanges changes;
	add_changed_files(core_.paths(), changed, changes);
	if (!changes.empty())
		core_.start_changed_refresh(std::move(changes));
}

void ContentController::finish_push(ContentPush &push) {
	ContentView &view = core_.view().content;
	switch (push.outcome()) {
		case PushOutcome::Pushed:
			view.last = "Pushed revision " + std::to_string(push.revision()) + " (" +
					count_text(push.changes().size(), "change", "changes") + ").";
			core_.note("Content store: " + view.last);
			view.head = push.revision();
			view.head_known = true;
			view.checked = true;
			view.changes.clear();
			show_binding();
			return;
		case PushOutcome::NothingToPush:
			view.last = "Nothing to push: the folder holds its base's files.";
			return;
		case PushOutcome::Stale:
			view.head = push.head();
			view.head_known = true;
			view.last = "The project's head is revision " + std::to_string(push.head()) +
					", past the folder's " + std::to_string(view.revision) + ": pull, then push.";
			refuse(view.last);
			return;
	}
}

OperationOutcome ContentController::finish_job(
		OperationKind kind, std::unique_ptr<ContentJob> job) {
	OperationOutcome outcome;
	keep_account(job->take_account());
	ContentView &view = core_.view().content;
	switch (job->state()) {
		case ContentJobState::Failed: {
			const Diagnostic failed = make_finding(CoreFinding::ContentFailed,
					DiagnosticSeverity::Warning,
					std::string(operation_kind_row(kind).noun) + " failed: " + job->error());
			core_.refuse_request(failed); // the status line and Output: no problem of the project's
			outcome.end = OperationEnd::Failed;
			outcome.findings.push_back(failed);
			view.last = failed.message;
			if (kind == OperationKind::ContentPull)
				get_dir_.clear();
			break;
		}
		case ContentJobState::Cancelled:
			outcome.end = OperationEnd::Cancelled;
			view.last = std::string(operation_kind_row(kind).noun) + " was cancelled.";
			break;
		case ContentJobState::NeedsResolution: {
			if (kind != OperationKind::ContentPull)
				break; // only a pull waits on its author
			paused_.reset(static_cast<ContentPull *>(job.release()));
			show_conflicts();
			const Diagnostic waiting =
					make_finding(CoreFinding::ContentConflicts, DiagnosticSeverity::Warning,
							"The pull of revision " + std::to_string(paused_->remote().head) +
									" stopped on " +
									count_text(paused_->plan().conflicts(), "file", "files") +
									" both you and the revision changed: keep yours or take "
									"theirs, then pull.");
			core_.refuse_request(waiting);
			outcome.findings.push_back(waiting);
			view.last = waiting.message;
			break;
		}
		case ContentJobState::Running:
		case ContentJobState::Done:
			if (kind == OperationKind::ContentCheck) {
				const auto &checked = static_cast<ContentCheck &>(*job);
				view.checked = true;
				view.head_known = checked.remote_known();
				view.head = checked.remote_known() ? checked.remote().head : view.head;
				view.head_error = checked.remote_error();
				view.changes = checked.changes();
				view.problems = checked.problems();
				view.last = count_text(view.changes.size(), "change", "changes") +
						(view.head_known ? "; the head is revision " + std::to_string(view.head) +
												"."
										 : "; the service did not answer.");
			} else if (kind == OperationKind::ContentBind) {
				view.last = "Bound to " + static_cast<ContentBind &>(*job).binding().key +
						" at revision " +
						std::to_string(static_cast<ContentBind &>(*job).binding().revision) + ".";
				core_.note("Content store: " + view.last);
				show_binding();
			} else if (kind == OperationKind::ContentPull) {
				finish_pull(static_cast<ContentPull &>(*job));
			} else if (kind == OperationKind::ContentPush) {
				finish_push(static_cast<ContentPush &>(*job));
			}
			break;
	}
	core_.touch(ViewConcern::Content);
	return outcome;
}

} // namespace opennova::editor
