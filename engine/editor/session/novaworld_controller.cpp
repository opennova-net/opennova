#include <editor/session/novaworld_controller.h>

#include <filesystem>
#include <system_error>
#include <utility>

#include <base/io/file_io.h>
#include <base/io/json.h>
#include <base/io/os_path.h>
#include <base/project/project_manifest.h>
#include <editor/import/import_run.h>
#include <editor/model/core_finding.h>
#include <editor/model/diagnostic.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>
#include <editor/session/disk_watch.h>
#include <editor/session/novaworld_operation.h>
#include <editor/session/session_core.h>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

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
// route takes it (apps/novaworld_web/products/project_routes.cpp valid_version).
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

// Why a get of the project `key` gets nothing: it has no revision yet (the NovaWorld window offers
// no Get for one).
std::string no_revision_text(const std::string &key) {
	return key + " has no revision yet, so there is nothing to get: open a project, bind it to " +
			key + " and push its first revision.";
}

// Where the expansion's project at `dir` finds its base game's project: its project file's
// expansion.base_project resolved against `dir`; false when it names none.
bool base_project_folder(const std::string &dir, std::string &out) {
	std::string text, error;
	io::JsonValue document;
	if (!io::read_file_text(dir + "/" + kProjectFileName, text, error) ||
			!io::json_parse(text, document, error) || !document.is_object())
		return false;
	const io::JsonValue *expansion = document.get("expansion");
	const std::string named =
			expansion && expansion->is_object() ? expansion->get_string("base_project", "") : std::string();
	if (named.empty())
		return false;
	out = io::utf8_generic_path((io::os_path(dir) / io::os_path(named)).lexically_normal());
	return true;
}

} // namespace

NovaWorldController::NovaWorldController(SessionCore &core) : core_(core), accounts_(&memory_) {}

void NovaWorldController::set_transport(NovaWorldTransport *transport) {
	transport_ = transport;
	core_.view().novaworld.available = transport != nullptr;
	core_.touch(ViewConcern::NovaWorld);
}

void NovaWorldController::set_accounts(NovaWorldAccountStore *accounts) {
	accounts_ = accounts ? accounts : &memory_;
	account_ = accounts_->load();
	show_account();
}

void NovaWorldController::show_account() {
	NovaWorldView &view = core_.view().novaworld;
	view.server = account_.server;
	view.username = account_.username;
	view.role = account_.signed_in() ? account_.role : std::string();
	view.signed_in = account_.signed_in();
	core_.touch(ViewConcern::NovaWorld);
}

void NovaWorldController::keep_account(const NovaWorldAccountUpdate &update) {
	if (!update.changed)
		return;
	account_ = update.account;
	std::string error;
	if (!accounts_->save(account_, error))
		core_.note("The NovaWorld account was not kept: " + error);
	show_account();
}

void NovaWorldController::refuse(const std::string &why) {
	core_.refuse_request(CoreFinding::NovaWorldRefused, why);
}

bool NovaWorldController::ready(bool admin, bool project, bool bound) {
	if (!transport_) {
		refuse("This editor reaches no NovaWorld service.");
		return false;
	}
	if (admin && !account_.admin()) {
		refuse(account_.signed_in() ? "Only an admin pushes or binds a project for pushing."
									: "Sign in to NovaWorld first.");
		return false;
	}
	if (project && !core_.view().project.open) {
		refuse("Open a project first.");
		return false;
	}
	if (bound && !core_.view().novaworld.bound) {
		refuse("The project is no working copy of a NovaWorld project yet: bind it first.");
		return false;
	}
	// A working copy is of one service's project (a debug build signs in to staging, a release
	// build to the live service): another service's revisions are not its.
	NovaWorldBinding binding;
	std::string error;
	if (bound && read_novaworld_binding(NovaWorldPaths::for_project(core_.paths()), binding, error) &&
			binding.server != account_.server) {
		refuse("The project is a working copy of " + binding.key + " on " + binding.server +
				": sign in there first.");
		return false;
	}
	return true;
}

std::string NovaWorldController::export_dir() const {
	const auto &document = core_.view().project.document;
	return document ? core_.paths().export_dir(*document) : std::string();
}

void NovaWorldController::start(OperationKind kind, std::unique_ptr<NovaWorldJob> job) {
	if (core_.start_operation(std::make_unique<NovaWorldOperation>(kind, *this, std::move(job))) == 0)
		core_.refuse_busy(std::string());
	core_.touch(ViewConcern::NovaWorld);
}

void NovaWorldController::sign_in(const EditorRequest &request) {
	if (!transport_) {
		refuse("This editor reaches no NovaWorld service.");
		return;
	}
	const std::string server =
			novaworld_server(request.server.empty() ? account_.server : request.server);
	if (server.empty() || request.username.empty() || request.password.empty()) {
		refuse("Sign in names the service, the account and its password.");
		return;
	}
	login_server_ = server;
	login_username_ = request.username;
	login_ = NovaWorldCall(
			*transport_, novaworld_api::login(server, request.username, request.password));
	core_.view().novaworld.calling = true;
	core_.touch(ViewConcern::NovaWorld);
}

void NovaWorldController::sign_out() {
	if (transport_ && account_.signed_in())
		logout_ = NovaWorldCall(*transport_, novaworld_api::logout(account_)); // its answer is no one's
	NovaWorldAccountUpdate update{ true, account_ };
	update.account.cookie.clear();
	update.account.role.clear();
	keep_account(update);
	core_.view().novaworld.last = "Signed out of " + account_.server + ".";
	list_projects();
}

void NovaWorldController::list_projects() {
	if (!transport_ || account_.server.empty())
		return;
	projects_ = NovaWorldCall(*transport_, novaworld_api::projects(account_));
	core_.view().novaworld.calling = true;
	core_.touch(ViewConcern::NovaWorld);
}

void NovaWorldController::check() {
	if (!ready(false, false, false))
		return;
	if (account_.server.empty()) {
		refuse("Sign in to NovaWorld first, or name its server.");
		return;
	}
	list_projects();
	if (!core_.view().project.open || !core_.view().novaworld.bound || !ready(false, true, true))
		return;
	NovaWorldBinding binding;
	std::string error;
	read_novaworld_binding(NovaWorldPaths::for_project(core_.paths()), binding, error);
	start(OperationKind::NovaWorldCheck,
			std::make_unique<NovaWorldCheck>(
					*transport_, account_, core_.paths(), export_dir(), binding));
}

void NovaWorldController::get(const EditorRequest &request) {
	if (!ready(false, false, false))
		return;
	if (request.dir.empty() || request.project_key.empty()) {
		refuse("A get names the project and the folder it goes into.");
		return;
	}
	if (!empty_folder(request.dir)) {
		refuse("The folder " + request.dir +
				" holds files: a project is got into an empty folder.");
		return;
	}
	// The list may be older than the project (a revision pushed since it was asked): whether the project has a
	// revision is the head the get's pull reads (finish_pull refuses one at none).
	const RemoteProject *listed = nullptr;
	for (const RemoteProject &project : core_.view().novaworld.projects)
		if (project.key == request.project_key)
			listed = &project;
	NovaWorldBinding binding{ account_.server, request.project_key, 0 };
	get_dir_ = request.dir;
	get_open_after_.clear();
	// An expansion's project builds on its base game's, which comes along beside it.
	get_base_key_ = listed ? listed->base : std::string();
	paused_.reset();
	show_conflicts();
	start(OperationKind::NovaWorldPull,
			std::make_unique<NovaWorldPull>(
					*transport_, account_, ProjectPaths::for_root(request.dir), "", binding));
}

void NovaWorldController::bind(const EditorRequest &request) {
	if (!ready(false, true, false))
		return;
	if (request.project_key.empty()) {
		refuse("A bind names the NovaWorld project the open project is a working copy of.");
		return;
	}
	start(OperationKind::NovaWorldBind,
			std::make_unique<NovaWorldBind>(*transport_, account_, core_.paths(),
					NovaWorldBinding{ account_.server, request.project_key, 0 }));
}

void NovaWorldController::pull() {
	if (!ready(false, true, true))
		return;
	if (paused_) {
		if (paused_->plan().conflicts() > 0) {
			refuse("Settle every conflict first: keep yours or take theirs (novaworld_resolve).");
			return;
		}
		paused_->resume();
		start(OperationKind::NovaWorldPull, std::move(paused_));
		return;
	}
	NovaWorldBinding binding;
	std::string error;
	if (!read_novaworld_binding(NovaWorldPaths::for_project(core_.paths()), binding, error)) {
		refuse(error.empty() ? "The project's binding is gone: bind it again." : error);
		return;
	}
	get_dir_.clear();
	start(OperationKind::NovaWorldPull,
			std::make_unique<NovaWorldPull>(
					*transport_, account_, core_.paths(), export_dir(), binding));
}

void NovaWorldController::resolve(const EditorRequest &request) {
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
		core_.view().novaworld.last = "Every conflict is settled: pull to go on.";
	core_.touch(ViewConcern::NovaWorld);
}

void NovaWorldController::push(const EditorRequest &request) {
	if (!ready(true, true, true))
		return;
	if (!project::is_utf8(request.message) || request.message.empty() ||
			request.message.size() > 2000) {
		refuse("A push names what it changes: a message of 1 to 2000 bytes.");
		return;
	}
	if (paused_) {
		refuse("A pull waits on its conflicts: settle them and pull before pushing.");
		return;
	}
	NovaWorldBinding binding;
	std::string error;
	if (!read_novaworld_binding(NovaWorldPaths::for_project(core_.paths()), binding, error)) {
		refuse(error.empty() ? "The project's binding is gone: bind it again." : error);
		return;
	}
	start(OperationKind::NovaWorldPush,
			std::make_unique<NovaWorldPush>(
					*transport_, account_, core_.paths(), export_dir(), binding, request.message));
}

void NovaWorldController::release(const EditorRequest &request) {
	if (!ready(true, true, true))
		return;
	const NovaWorldView &view = core_.view().novaworld;
	if (!release_version(request.version)) {
		refuse("A release names its version: one to four numbers between dots (0.2.0).");
		return;
	}
	if (view.revision <= 0) {
		refuse("Push the project first: a release is built from a revision on NovaWorld.");
		return;
	}
	if (release_.started()) {
		refuse("A release is being asked for already.");
		return;
	}
	release_server_ = account_.server;
	release_ = NovaWorldCall(*transport_,
			novaworld_api::release(account_, view.key, request.version, view.revision));
	core_.view().novaworld.calling = true;
	core_.touch(ViewConcern::NovaWorld);
}

void NovaWorldController::restore(const EditorRequest &request) {
	if (!ready(false, true, true))
		return;
	std::string why;
	if (!project::is_project_path(request.path, why)) {
		refuse("A restore names one of the project's files: " + why);
		return;
	}
	if (paused_) {
		refuse("A pull waits on its conflicts: settle them and pull first.");
		return;
	}
	NovaWorldBinding binding;
	std::string error;
	if (!read_novaworld_binding(NovaWorldPaths::for_project(core_.paths()), binding, error)) {
		refuse(error.empty() ? "The project's binding is gone: bind it again." : error);
		return;
	}
	start(OperationKind::NovaWorldPull,
			std::make_unique<NovaWorldPull>(*transport_, account_, core_.paths(), export_dir(), binding,
					NovaWorldRestore{ { request.path }, request.revision }));
}

void NovaWorldController::history() {
	if (!ready(false, true, true))
		return;
	if (history_.started())
		return; // the list in flight answers this ask too
	history_ = NovaWorldCall(*transport_, novaworld_api::revisions(account_, core_.view().novaworld.key, 0, 50));
	core_.view().novaworld.calling = true;
	core_.touch(ViewConcern::NovaWorld);
}

void NovaWorldController::cancel() {
	// A NovaWorld job running gave way to this request at the gate (its row's superseded_by); a pull
	// kept for its conflicts goes too.
	if (paused_) {
		paused_.reset();
		get_dir_.clear();
		core_.view().novaworld.conflicts.clear();
		core_.view().novaworld.pull_revision = 0;
		core_.view().novaworld.last = "The pull was given up.";
	}
	core_.touch(ViewConcern::NovaWorld);
}

void NovaWorldController::poll() {
	NovaWorldView &view = core_.view().novaworld;
	if (logout_.started() && logout_.poll())
		logout_ = NovaWorldCall();
	if (login_.started() && login_.poll()) {
		NovaWorldAccount account;
		account.server = login_server_;
		account.username = login_username_;
		NovaWorldApiError error;
		if (novaworld_api::read_login(login_.reply(), account, error)) {
			keep_account({ true, account });
			view.last = "Signed in to " + account.server + " as " + account.username + ".";
			core_.note("NovaWorld: " + view.last);
			list_projects();
		} else {
			view.last = "The NovaWorld sign-in failed: " + error.line();
			core_.refuse_request(make_finding(
					CoreFinding::NovaWorldFailed, DiagnosticSeverity::Warning, view.last));
		}
		login_ = NovaWorldCall();
		core_.touch(ViewConcern::NovaWorld);
	}
	if (projects_.started() && projects_.poll()) {
		NovaWorldApiError error;
		std::vector<RemoteProject> projects;
		if (novaworld_api::read_projects(projects_.reply(), projects, error)) {
			view.projects = std::move(projects);
			view.projects_error.clear();
		} else {
			view.projects_error = error.line();
		}
		projects_ = NovaWorldCall();
		core_.touch(ViewConcern::NovaWorld);
	}
	if (release_.started() && release_.poll()) {
		NovaWorldApiError error;
		int64_t id = 0;
		std::string name;
		if (novaworld_api::read_release(release_.reply(), id, name, error)) {
			view.last = "Release " + name + " is building on " + release_server_ +
					": publish it in the web admin's Releases once it is a draft.";
			core_.note("NovaWorld: " + view.last);
		} else {
			view.last = "The release was refused: " + error.line();
			core_.refuse_request(make_finding(
					CoreFinding::NovaWorldFailed, DiagnosticSeverity::Warning, view.last));
		}
		release_ = NovaWorldCall();
		core_.touch(ViewConcern::NovaWorld);
	}
	if (history_.started() && history_.poll()) {
		NovaWorldApiError error;
		std::vector<RemoteRevision> revisions;
		if (novaworld_api::read_revisions(history_.reply(), revisions, error)) {
			view.history = std::move(revisions);
			view.history_error.clear();
		} else {
			view.history_error = error.line();
		}
		history_ = NovaWorldCall();
		core_.touch(ViewConcern::NovaWorld);
	}
	const bool calling =
			login_.started() || projects_.started() || release_.started() || history_.started();
	if (view.calling != calling) {
		view.calling = calling;
		core_.touch(ViewConcern::NovaWorld);
	}
}

bool NovaWorldController::busy() const {
	return login_.started() || projects_.started() || release_.started() || history_.started();
}

void NovaWorldController::show_binding() {
	NovaWorldView &view = core_.view().novaworld;
	NovaWorldBinding binding;
	std::string error;
	view.bound = core_.view().project.open &&
			read_novaworld_binding(NovaWorldPaths::for_project(core_.paths()), binding, error);
	view.key = view.bound ? binding.key : std::string();
	view.revision = view.bound ? binding.revision : 0;
	core_.touch(ViewConcern::NovaWorld);
}

void NovaWorldController::show_conflicts() {
	NovaWorldView &view = core_.view().novaworld;
	view.conflicts.clear();
	if (paused_) {
		for (const PullItem &item : paused_->plan().items)
			if (item.step == PullStep::Conflict)
				view.conflicts.push_back({ item.path, item.mine_change, item.theirs_change });
		view.pull_revision = paused_->remote().head;
	} else {
		view.pull_revision = 0;
	}
	core_.touch(ViewConcern::NovaWorld);
}

void NovaWorldController::project_opened() {
	show_binding();
	NovaWorldView &view = core_.view().novaworld;
	view.checked = false;
	view.changes.clear();
	view.problems.clear();
}

void NovaWorldController::forget_project() {
	if (get_dir_.empty())
		paused_.reset();
	NovaWorldView &view = core_.view().novaworld;
	view.bound = false;
	view.key.clear();
	view.revision = 0;
	view.checked = false;
	view.head_known = false;
	view.changes.clear();
	view.problems.clear();
	history_ = NovaWorldCall(); // a list in flight is the closed project's
	view.history.clear();
	view.history_error.clear();
	show_conflicts();
}

void NovaWorldController::finish_pull(NovaWorldPull &pull) {
	NovaWorldView &view = core_.view().novaworld;
	if (const NovaWorldRestore *restore = pull.restore()) {
		const std::string what = restore->paths.size() == 1
				? restore->paths.front()
				: count_text(restore->paths.size(), "file", "files");
		const std::string revision = std::to_string(pull.restored_revision());
		view.last = pull.plan().items.empty() ? "Nothing to restore: " + what + " is as revision " + revision + " has it."
				: pull.restored_revision() == view.revision ? "Discarded the changes to " + what + "."
															: what + " is back as revision " + revision +
								" has it: a change to push.";
		core_.note("NovaWorld: " + view.last);
		view.checked = false; // the changes are counted again by the next check
		view.changes.clear();
	} else {
		if (pull.up_to_date() && !get_dir_.empty()) {
			// A get of a project with no revision yet (its head as the pull read it, whatever the list
			// said): nothing written, no folder bound. An expansion's base with none: the expansion opens.
			const std::string open_after = std::move(get_open_after_);
			get_dir_.clear();
			get_base_key_.clear();
			get_open_after_.clear();
			view.last = no_revision_text(pull.remote().key);
			if (!open_after.empty()) {
				core_.note("NovaWorld: " + view.last);
				core_.open_project(open_after);
				return;
			}
			refuse(view.last);
			return;
		}
		if (pull.up_to_date()) {
			view.last = "The project is at the head, revision " + std::to_string(pull.revision()) + ".";
			show_binding();
			return;
		}
		view.last = "Pulled revision " + std::to_string(pull.revision()) + ": " +
				count_text(pull.applied().written, "file written", "files written") + ", " +
				count_text(pull.applied().removed, "removed", "removed") + ".";
		core_.note("NovaWorld: " + view.last);
		view.head = pull.revision();
		view.head_known = true;
		view.checked = false; // the changes are counted again by the next check
		view.changes.clear();
		if (!get_dir_.empty()) {
			std::string dir = std::move(get_dir_);
			get_dir_.clear();
			if (!get_open_after_.empty()) {
				// The base an expansion builds on landed beside it: the expansion opens.
				dir = std::move(get_open_after_);
				get_open_after_.clear();
			} else if (!get_base_key_.empty()) {
				// An expansion's: its base game's project got where its project file names it, unless a
				// project is there already.
				const std::string base_key = std::move(get_base_key_);
				get_base_key_.clear();
				std::string base_dir;
				if (base_project_folder(dir, base_dir) && empty_folder(base_dir)) {
					get_dir_ = base_dir;
					get_open_after_ = dir;
					view.last += " Getting its base game's project, " + base_key + ", into " + base_dir + ".";
					start(OperationKind::NovaWorldPull,
							std::make_unique<NovaWorldPull>(*transport_, account_,
									ProjectPaths::for_root(base_dir), "",
									NovaWorldBinding{ account_.server, base_key, 0 }));
					return;
				}
			}
			core_.open_project(dir);
			return;
		}
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

void NovaWorldController::finish_push(NovaWorldPush &push) {
	NovaWorldView &view = core_.view().novaworld;
	switch (push.outcome()) {
		case PushOutcome::Pushed:
			view.last = "Pushed revision " + std::to_string(push.revision()) + " (" +
					count_text(push.changes().size(), "change", "changes") + ").";
			core_.note("NovaWorld: " + view.last);
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

OperationOutcome NovaWorldController::finish_job(
		OperationKind kind, std::unique_ptr<NovaWorldJob> job) {
	OperationOutcome outcome;
	keep_account(job->take_account());
	NovaWorldView &view = core_.view().novaworld;
	switch (job->state()) {
		case NovaWorldJobState::Failed: {
			const Diagnostic failed = make_finding(CoreFinding::NovaWorldFailed,
					DiagnosticSeverity::Warning,
					std::string(operation_kind_row(kind).noun) + " failed: " + job->error());
			core_.refuse_request(failed); // the status line and Output: no problem of the project's
			outcome.end = OperationEnd::Failed;
			outcome.findings.push_back(failed);
			view.last = failed.message;
			if (kind == OperationKind::NovaWorldPull) {
				get_dir_.clear();
				get_base_key_.clear();
				get_open_after_.clear();
			}
			break;
		}
		case NovaWorldJobState::Cancelled:
			outcome.end = OperationEnd::Cancelled;
			view.last = std::string(operation_kind_row(kind).noun) + " was cancelled.";
			break;
		case NovaWorldJobState::NeedsResolution: {
			if (kind != OperationKind::NovaWorldPull)
				break; // only a pull waits on its author
			paused_.reset(static_cast<NovaWorldPull *>(job.release()));
			show_conflicts();
			const Diagnostic waiting =
					make_finding(CoreFinding::NovaWorldConflicts, DiagnosticSeverity::Warning,
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
		case NovaWorldJobState::Running:
		case NovaWorldJobState::Done:
			if (kind == OperationKind::NovaWorldCheck) {
				const auto &checked = static_cast<NovaWorldCheck &>(*job);
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
			} else if (kind == OperationKind::NovaWorldBind) {
				view.last = "Bound to " + static_cast<NovaWorldBind &>(*job).binding().key +
						" at revision " +
						std::to_string(static_cast<NovaWorldBind &>(*job).binding().revision) + ".";
				core_.note("NovaWorld: " + view.last);
				show_binding();
			} else if (kind == OperationKind::NovaWorldPull) {
				finish_pull(static_cast<NovaWorldPull &>(*job));
			} else if (kind == OperationKind::NovaWorldPush) {
				finish_push(static_cast<NovaWorldPush &>(*job));
			}
			break;
	}
	core_.touch(ViewConcern::NovaWorld);
	return outcome;
}

} // namespace opennova::editor
