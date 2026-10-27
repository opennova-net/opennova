#include <editor/session/novaworld_controller.h>

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <utility>

#include <base/gameprofile/gameprofile.h>
#include <base/io/file_io.h>
#include <base/io/json.h>
#include <base/io/os_path.h>
#include <base/io/strutil.h>
#include <base/project/project_manifest.h>
#include <editor/assets/install_check.h>
#include <editor/import/import_run.h>
#include <editor/model/core_finding.h>
#include <editor/model/diagnostic.h>
#include <editor/project/base_project.h>
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

// An expansion's code as the service takes one (apps/novaworld_web/products/product_routes.cpp
// valid_expansion_code): 1 to 8 of a-z and 0-9.
bool expansion_code(const std::string &name) {
	if (name.empty() || name.size() > 8)
		return false;
	for (const char c : name)
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
			return false;
	return true;
}

// Why a get of the project `key` gets nothing: it has no revision yet (the NovaWorld window offers
// Set up... for one the account may push, and no Get).
std::string no_revision_text(const std::string &key) {
	return key +
			" has no revision yet, so there is nothing to get: set up a new project for it "
			"(Set up...) or put an open project on NovaWorld as " +
			key + ", then push its first revision.";
}

// Why a request finds no working copy where the view showed one: its state gone since.
std::string copy_gone_text() {
	return "The folder's working copy state is gone: Check sets it up again.";
}

// Why the service shows the account no project `key` (a 404): one it lacks, or a private one.
std::string unreadable_text(const std::string &key, const std::string &server) {
	return key + " is not on " + novaworld_host(server) +
			", or is private: sign in with an account that may read it.";
}

// Why the service would not make a new project of the account's (POST /api/products), by its code;
// `code` the mod's short name asked for.
std::string created_refusal(
		const NovaWorldApiError &error, const std::string &key, const std::string &code) {
	if (error.code == "duplicate_key")
		return "NovaWorld has a project " + key + " already: choose another key.";
	// A mod's short name is NovaWorld's own across every game (expansion\<code>\ in any install).
	if (error.code == "duplicate_expansion" ||
			(error.status == 409 && error.field == "expansion_code"))
		return "another mod on NovaWorld uses the short name " + code +
				" already: give the expansion another Short name in Project settings, then put it on "
				"NovaWorld again.";
	if (error.code == "too_many_projects")
		return "the account has as many projects as NovaWorld keeps for it: delete one on the "
			   "site first.";
	if (error.code == "no_such_base")
		return "NovaWorld has no such base game: name the game it changes (base_key).";
	if (error.code == "rate_limited")
		return "NovaWorld asks to wait before making another project: try again in a minute.";
	return error.line();
}

// Why the account may not release the project `key` (the service's can_release for it).
std::string release_rights_text(const std::string &key) {
	return "Only its owner and NovaWorld's admins release " + key + ".";
}

// Why the service would not build a release of the project `key` (POST /api/products/<key>/releases),
// by its code.
std::string release_refusal(
		const NovaWorldApiError &error, const NovaWorldResponse &reply, const std::string &key) {
	if (error.code == "forbidden")
		return release_rights_text(key);
	if (error.code == "release_in_progress")
		return "a release of the account's is building already, and NovaWorld builds one at a time "
			   "for an account: release again once it is done.";
	if (error.code == "too_many_releases")
		return "the account has started as many releases today as NovaWorld builds for an account in "
			   "a day: release again tomorrow.";
	if (const std::string quota = novaworld_api::quota_text(reply, "release"); !quota.empty())
		return quota;
	if (error.code == "no_revision")
		return "NovaWorld has nothing to build it from: " +
				(error.message.empty() ? std::string("no such revision") : error.message) + ".";
	return error.line();
}

// A game's name by its gameprofile code ("Joint Operations"), the code for one the editor has no
// profile of.
std::string game_words(const std::string &code) {
	const gameprofile::GameProfile *profile = gameprofile::gameprofile_by_code(code.c_str());
	return profile && profile->display_name ? profile->display_name : code;
}

// A listed project by its key, null for none: one the account may not read is never listed.
const RemoteProject *listed_project(const NovaWorldView &view, const std::string &key) {
	for (const RemoteProject &project : view.projects)
		if (project.key == key)
			return &project;
	return nullptr;
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

std::string novaworld_put_mismatch(const ProjectDocument &doc, const RemoteProject &project) {
	const bool mod = !doc.expansion.standalone();
	if (project.kind != (mod ? "expansion" : "game"))
		return project.key + " is " + (project.kind == "expansion" ? "a mod" : "a game") +
				" on NovaWorld, and the project is " + (mod ? "a mod" : "a game") +
				": put it on an empty " + (mod ? "mod" : "game") + ", or as a new project.";
	if (project.game != doc.target_game)
		return project.key + " is a project of " + game_words(project.game) + ", and this one of " +
				game_words(doc.target_game) +
				": put it on an empty project of its game, or as a new project.";
	if (mod && !strutil::iequals(project.expansion_code, doc.expansion.name))
		return project.key + " is the mod " + project.expansion_code + ", and this one is " +
				doc.expansion.name + ": give the expansion the Short name " + project.expansion_code +
				" in Project settings, or put it on as a new project.";
	return {};
}

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
	account_changed();
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

void NovaWorldController::account_changed() {
	NovaWorldView &view = core_.view().novaworld;
	project_ = NovaWorldCall(); // the last account's ask
	// A put asked as the last account, or answered and waiting to write its key, is that account's:
	// given up on, said so.
	if (put_.started() || put_waiting_) {
		put_ = NovaWorldCall();
		put_waiting_ = false;
		view.last = "The put of " + put_key_ +
				" on NovaWorld was given up: the account changed before it finished. Put the project on "
				"again" +
				(put_create_ ? ", as the empty project " + put_key_ + " where NovaWorld made it." : ".");
		core_.note("NovaWorld: " + view.last);
	}
	view.remote_known = false;
	view.remote = RemoteProject();
	view.head_known = false;
	view.head_error.clear();
	// Another service's working copy (or none): what was counted against the last one's base goes.
	const bool was_tracked = view.tracked;
	const int64_t was_at = view.revision;
	show_copy();
	if (view.tracked != was_tracked || view.revision != was_at) {
		view.checked = false;
		view.changes.clear();
		view.problems.clear();
		view.history.clear();
	}
	if (!core_.view().project.open || view.key.empty())
		return;
	if (view.tracked)
		ask_project();
	else
		track(false);
}

void NovaWorldController::refuse(const std::string &why) {
	core_.refuse_request(CoreFinding::NovaWorldRefused, why);
}

bool NovaWorldController::ready(bool project, bool tracked) {
	if (!transport_) {
		refuse("This editor reaches no NovaWorld service.");
		return false;
	}
	if (project && !core_.view().project.open) {
		refuse("Open a project first.");
		return false;
	}
	if (!tracked)
		return true;
	// A working copy is of one service's project (a debug build signs in to staging, a release
	// build to the live service), each kept apart: another service's revisions are not its.
	const NovaWorldView &view = core_.view().novaworld;
	if (view.key.empty()) {
		refuse("The project is not on NovaWorld: put it there first (Put on NovaWorld...).");
		return false;
	}
	if (account_.server.empty()) {
		refuse("Sign in to NovaWorld first, or name its server.");
		return false;
	}
	if (!view.tracked) {
		refuse(!view.head_error.empty()
						? view.head_error
						: "The folder is not set up as " + view.key + "'s working copy on " +
								novaworld_host(account_.server) + " yet: Check sets it up.");
		return false;
	}
	return true;
}

std::string NovaWorldController::export_dir() const {
	const auto &document = core_.view().project.document;
	return document ? core_.paths().export_dir(*document) : std::string();
}

bool NovaWorldController::working_copy(NovaWorldWorkingCopy &out) const {
	out = NovaWorldWorkingCopy();
	if (!core_.view().project.open || account_.server.empty())
		return false;
	const std::string &key = core_.view().project.document->novaworld;
	std::string error;
	return !key.empty() &&
			read_novaworld_state(
					NovaWorldPaths::for_project(core_.paths(), account_.server), out, error) &&
			out.key == key;
}

void NovaWorldController::show_copy() {
	NovaWorldView &view = core_.view().novaworld;
	const bool open = core_.view().project.open;
	view.key = open ? core_.view().project.document->novaworld : std::string();
	NovaWorldWorkingCopy copy;
	view.tracked = working_copy(copy);
	view.revision = view.tracked ? copy.revision : 0;
	// What a put would make of it is found as a check asks (put_due_), only while it has no key.
	if (!open || !view.key.empty())
		view.put = NovaWorldView::PutPlan();
	core_.touch(ViewConcern::NovaWorld);
}

NovaWorldView::PutPlan NovaWorldController::put_plan() const {
	NovaWorldView::PutPlan plan;
	const ProjectDocument &doc = *core_.view().project.document;
	plan.game = doc.target_game;
	if (doc.expansion.standalone()) {
		plan.kind = "game";
		return plan;
	}
	plan.kind = "expansion";
	plan.expansion_code = doc.expansion.name;
	if (!expansion_code(doc.expansion.name))
		plan.refusal = "NovaWorld names a mod by 1 to 8 small letters and digits: rename " +
				doc.expansion.name + " in Project settings (its Short name) first.";
	// Its base game's key: the base game's project's own, else the game install's game (its
	// UPDATE.WIZ's, install_game) as NovaWorld lists the retail games, else as the games a new
	// project of the account's may be a mod of name it (one alone of that game, or the one keyed by
	// its code).
	if (doc.expansion.on_base_project()) {
		ProjectDocument base;
		Diagnostic unread;
		const std::string root = base_project_root(core_.paths().root, doc.expansion);
		if (!root.empty() && ::opennova::editor::open_project(root, base, unread))
			plan.base = base.novaworld;
	}
	const NovaWorldView &view = core_.view().novaworld;
	const std::string game = install_game(core_.game_install());
	if (plan.base.empty()) {
		std::vector<std::string> retail;
		for (const RemoteProject &project : view.projects)
			if (project.kind == "game" && project.retail && project.game == game)
				retail.push_back(project.key);
		if (retail.size() == 1)
			plan.base = retail.front();
	}
	if (plan.base.empty()) {
		std::vector<std::string> games;
		for (const RemoteGame &each : view.games)
			if (each.game == game)
				games.push_back(each.key);
		const std::string own_key = strutil::to_upper(game);
		if (games.size() == 1)
			plan.base = games.front();
		else if (std::find(games.begin(), games.end(), own_key) != games.end())
			plan.base = own_key;
	}
	return plan;
}

void NovaWorldController::ask_project() {
	const NovaWorldView &view = core_.view().novaworld;
	if (!transport_ || account_.server.empty() || !core_.view().project.open || view.key.empty())
		return;
	project_key_ = view.key;
	project_ = NovaWorldCall(*transport_, novaworld_api::project(account_, view.key));
	core_.view().novaworld.calling = true;
	core_.touch(ViewConcern::NovaWorld);
}

void NovaWorldController::take_remote(const NovaWorldJob &job) {
	NovaWorldView &view = core_.view().novaworld;
	if (job.remote_known()) {
		view.remote = job.remote();
		view.remote_known = true;
		view.head = job.remote().head;
		view.head_known = true;
		view.head_error.clear();
	} else if (job.not_found()) {
		view.remote = RemoteProject();
		view.remote_known = false;
		view.head_known = false;
		view.head_error = unreadable_text(view.key, account_.server);
	}
}

void NovaWorldController::track(bool then_check) {
	track_due_ = false;
	check_after_track_ = check_after_track_ || then_check;
	const NovaWorldView &view = core_.view().novaworld;
	if (!transport_ || !core_.view().project.open || view.key.empty() || account_.server.empty()) {
		check_after_track_ = false;
		return;
	}
	// The slot taken (an Open's import, a build): set up once it is free.
	if (core_.operations().running()) {
		track_due_ = true;
		return;
	}
	core_.start_operation(std::make_unique<NovaWorldOperation>(OperationKind::NovaWorldTrack, *this,
			std::make_unique<NovaWorldTrack>(
					*transport_, account_, core_.paths(), export_dir(), view.key)));
	core_.touch(ViewConcern::NovaWorld);
}

bool NovaWorldController::start_check() {
	NovaWorldWorkingCopy copy;
	if (!transport_ || !working_copy(copy))
		return false;
	start(OperationKind::NovaWorldCheck,
			std::make_unique<NovaWorldCheck>(
					*transport_, account_, core_.paths(), export_dir(), copy));
	return true;
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
	account_changed();
}

void NovaWorldController::list_projects() {
	if (!transport_ || account_.server.empty())
		return;
	projects_ = NovaWorldCall(*transport_, novaworld_api::projects(account_));
	// The games a new project of the account's may be a mod of: a signed-in account's alone.
	games_ = account_.signed_in() ? NovaWorldCall(*transport_, novaworld_api::my_projects(account_))
								  : NovaWorldCall();
	if (!account_.signed_in())
		core_.view().novaworld.games.clear();
	core_.view().novaworld.calling = true;
	core_.touch(ViewConcern::NovaWorld);
}

void NovaWorldController::check() {
	if (!ready(false, false))
		return;
	if (account_.server.empty()) {
		refuse("Sign in to NovaWorld first, or name its server.");
		return;
	}
	list_projects();
	const NovaWorldView &view = core_.view().novaworld;
	if (!core_.view().project.open)
		return;
	// A project not on NovaWorld: what a put would make of it, found once the lists are in (the Put
	// dialog asks as it opens).
	if (view.key.empty()) {
		put_due_ = true;
		return;
	}
	// A folder no working copy of its project on this service yet is made one, then checked.
	if (!view.tracked) {
		track(true);
		return;
	}
	start_check();
}

void NovaWorldController::get(const EditorRequest &request) {
	if (!ready(false, false))
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
	get_dir_ = request.dir;
	get_open_after_.clear();
	// An expansion's project builds on its base game's, which comes along beside it.
	get_base_key_ = listed ? listed->base : std::string();
	paused_.reset();
	show_conflicts();
	start(OperationKind::NovaWorldPull,
			std::make_unique<NovaWorldPull>(*transport_, account_,
					ProjectPaths::for_root(request.dir), "",
					NovaWorldWorkingCopy{ request.project_key, 0 }));
}

void NovaWorldController::put(const EditorRequest &request) {
	if (!ready(true, false))
		return;
	if (!account_.signed_in()) {
		refuse("Sign in to NovaWorld to put a project there.");
		return;
	}
	const ProjectDocument &doc = *core_.view().project.document;
	if (!doc.novaworld.empty()) {
		refuse("The project is on NovaWorld already, as " + doc.novaworld + ".");
		return;
	}
	if (!is_novaworld_key(request.project_key)) {
		refuse("A NovaWorld project's key is 1 to 16 of A-Z, 0-9 and _ (ACIDGAMBIT).");
		return;
	}
	if (put_.started() || put_waiting_) {
		refuse("The project is being put on NovaWorld already.");
		return;
	}
	NovaWorldRequest ask;
	if (request.create) {
		if (!request.visibility.empty() && request.visibility != "public" &&
				request.visibility != "private") {
			refuse("A new project is public or private.");
			return;
		}
		const NovaWorldView::PutPlan plan = put_plan();
		if (!plan.refusal.empty()) {
			refuse(plan.refusal);
			return;
		}
		NewRemoteProject made;
		made.key = request.project_key;
		made.title = request.title.empty() ? doc.title : request.title;
		made.kind = plan.kind;
		made.game = plan.game;
		made.base = plan.base.empty() ? request.base_key : plan.base;
		made.expansion_code = plan.expansion_code;
		made.visibility = request.visibility.empty() ? "public" : request.visibility;
		if (made.kind == "expansion" && made.base.empty()) {
			refuse("Name the NovaWorld game the mod changes (base_key): the editor finds none.");
			return;
		}
		ask = novaworld_api::create_project(account_, made);
	} else {
		ask = novaworld_api::project(account_, request.project_key);
	}
	put_root_ = core_.paths().root;
	put_server_ = account_.server;
	put_key_ = request.project_key;
	put_code_ = doc.expansion.name;
	put_create_ = request.create;
	put_ = NovaWorldCall(*transport_, std::move(ask));
	core_.view().novaworld.calling = true;
	core_.touch(ViewConcern::NovaWorld);
}

void NovaWorldController::finish_put() {
	put_waiting_ = false;
	NovaWorldView &view = core_.view().novaworld;
	const std::string key = put_key_;
	const auto failed = [&](const std::string &why) {
		view.last = why;
		core_.refuse_request(
				make_finding(CoreFinding::NovaWorldFailed, DiagnosticSeverity::Warning, view.last));
		core_.touch(ViewConcern::NovaWorld);
	};
	if (!core_.view().project.open || core_.paths().root != put_root_ ||
			!core_.view().project.document->novaworld.empty())
		return failed(key +
				" is on NovaWorld, but its project closed or changed before its key was "
				"written: open it and put it on NovaWorld again as the empty project " +
				key + ".");
	// The key written as Project settings' Apply writes the project file.
	ProjectDocument project = *core_.view().project.document;
	project.novaworld = key;
	Diagnostic error;
	if (!core_.write_project(project, error))
		return failed(key + " is on NovaWorld, but the project file was not written (" +
				error.message + "): put it on NovaWorld again as the empty project " + key + ".");
	// The folder its working copy at revision 0 on the service the put was sent to: no base, every
	// file a change to push.
	const NovaWorldPaths paths = NovaWorldPaths::for_project(core_.paths(), put_server_);
	std::string why;
	if (!write_novaworld_base(paths, {}, why) ||
			!write_novaworld_state(paths, NovaWorldWorkingCopy{ key, 0 }, why)) {
		show_copy();
		return failed("The project is on NovaWorld as " + key +
				", but its working copy is not set up (" + why + "): Check sets it up.");
	}
	view.remote = put_project_;
	view.remote_known = true;
	view.head = put_project_.head;
	view.head_known = true;
	view.head_error.clear();
	show_copy();
	view.last = "The project is on NovaWorld as " + key + ": push its files as the first revision.";
	core_.note("NovaWorld: " + view.last);
	// Its files counted as the first push's changes; the projects listed again, a new one with
	// them.
	check_due_ = true;
	list_projects();
	core_.touch(ViewConcern::NovaWorld);
}

void NovaWorldController::pull() {
	if (!ready(true, true))
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
	NovaWorldWorkingCopy copy;
	if (!working_copy(copy))
		return refuse(copy_gone_text());
	get_dir_.clear();
	start(OperationKind::NovaWorldPull,
			std::make_unique<NovaWorldPull>(
					*transport_, account_, core_.paths(), export_dir(), copy));
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
	if (!ready(true, true))
		return;
	const NovaWorldView &view = core_.view().novaworld;
	if (!account_.signed_in()) {
		refuse("Sign in to NovaWorld to push.");
		return;
	}
	// The service's word on it as it last answered; the push asks again as it starts.
	if (view.remote_known && !view.remote.can_push) {
		refuse(novaworld_push_refusal(view.key));
		return;
	}
	if (!project::is_utf8(request.message) || request.message.empty() ||
			request.message.size() > 2000) {
		refuse("A push names what it changes: a message of 1 to 2000 bytes.");
		return;
	}
	if (paused_) {
		refuse("A pull waits on its conflicts: settle them and pull before pushing.");
		return;
	}
	NovaWorldWorkingCopy copy;
	if (!working_copy(copy))
		return refuse(copy_gone_text());
	start(OperationKind::NovaWorldPush,
			std::make_unique<NovaWorldPush>(
					*transport_, account_, core_.paths(), export_dir(), copy, request.message));
}

void NovaWorldController::release(const EditorRequest &request) {
	if (!ready(true, true))
		return;
	const NovaWorldView &view = core_.view().novaworld;
	if (!account_.signed_in()) {
		refuse("Sign in to NovaWorld to release a project.");
		return;
	}
	// The service's word on it as it last answered (its owner and the admins release it); the release
	// route weighs it again.
	if (view.remote_known && !view.remote.can_release) {
		refuse(release_rights_text(view.key));
		return;
	}
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
	release_key_ = view.key;
	release_admin_ = account_.admin();
	release_ = NovaWorldCall(*transport_,
			novaworld_api::release(account_, view.key, request.version, view.revision));
	core_.view().novaworld.calling = true;
	core_.touch(ViewConcern::NovaWorld);
}

void NovaWorldController::restore(const EditorRequest &request) {
	if (!ready(true, true))
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
	NovaWorldWorkingCopy copy;
	if (!working_copy(copy))
		return refuse(copy_gone_text());
	start(OperationKind::NovaWorldPull,
			std::make_unique<NovaWorldPull>(*transport_, account_, core_.paths(), export_dir(),
					copy, NovaWorldRestore{ { request.path }, request.revision }));
}

void NovaWorldController::history() {
	if (!ready(true, true))
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
			account_changed();
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
	if (games_.started() && games_.poll()) {
		std::vector<RemoteGame> games;
		NovaWorldApiError error;
		// A list that fails leaves the games as they were: a new project names its base itself
		// then.
		if (novaworld_api::read_my_games(games_.reply(), games, error))
			view.games = std::move(games);
		games_ = NovaWorldCall();
		core_.touch(ViewConcern::NovaWorld);
	}
	// What a put would make of the open project, as a check asked: found once the lists it reads
	// (its base game among the games listed) are in.
	if (put_due_ && !projects_.started() && !games_.started()) {
		put_due_ = false;
		if (core_.view().project.open && view.key.empty()) {
			view.put = put_plan();
			core_.touch(ViewConcern::NovaWorld);
		}
	}
	if (project_.started() && project_.poll()) {
		RemoteProject remote;
		std::optional<RemoteRevision> head;
		NovaWorldApiError error;
		const bool read = novaworld_api::read_project(project_.reply(), remote, head, error);
		project_ = NovaWorldCall();
		// The open project's answer, unless it is another project's now.
		if (core_.view().project.open && view.key == project_key_) {
			view.remote_known = read;
			view.remote = read ? remote : RemoteProject();
			view.head_known = read;
			view.head = read ? remote.head : 0;
			view.head_error = read		  ? std::string()
					: error.status == 404 ? unreadable_text(view.key, account_.server)
										  : error.line();
			// A project the account may read, its folder no working copy of it here yet: set up.
			if (read && !view.tracked)
				track(false);
		}
		core_.touch(ViewConcern::NovaWorld);
	}
	if (put_.started() && put_.poll()) {
		RemoteProject project;
		std::optional<RemoteRevision> head;
		NovaWorldApiError error;
		const bool read = put_create_
				? novaworld_api::read_created(put_.reply(), project, error)
				: novaworld_api::read_project(put_.reply(), project, head, error);
		put_ = NovaWorldCall();
		std::string refused;
		// An empty project is the open project's to take only where it is what the project is: a
		// game or a mod, of its game, a mod by its short name (the project closed or changed since,
		// finish_put says so).
		const bool same_open = core_.view().project.open && core_.paths().root == put_root_;
		if (!read)
			refused = put_create_		  ? created_refusal(error, put_key_, put_code_)
					: error.status == 404 ? unreadable_text(put_key_, put_server_)
										  : error.line();
		else if (!put_create_ && project.head > 0)
			refused = put_key_ +
					" has revisions already, so it is no empty project: get it (Get...) instead.";
		else if (!project.can_push)
			refused = novaworld_push_refusal(put_key_);
		else if (!put_create_ && same_open)
			refused = novaworld_put_mismatch(*core_.view().project.document, project);
		if (!refused.empty()) {
			view.last = "The project was not put on NovaWorld: " + refused;
			core_.refuse_request(make_finding(
					CoreFinding::NovaWorldFailed, DiagnosticSeverity::Warning, view.last));
		} else {
			put_project_ = project;
			put_waiting_ = true;
		}
		core_.touch(ViewConcern::NovaWorld);
	}
	// A put's key written once nothing holds the project (an Open, the base game's export, a
	// setup).
	if (put_waiting_ && !core_.busy_for(HoldsNothing, HoldsProject))
		finish_put();
	if (release_.started() && release_.poll()) {
		NovaWorldApiError error;
		int64_t id = 0;
		std::string name;
		if (novaworld_api::read_release(release_.reply(), id, name, error)) {
			// Published where the account sees the project's releases: an admin's Products page, an
			// owner's own project page under Projects.
			view.last = "Release " + name + " is building on " + novaworld_host(release_server_) +
					": publish it " +
					(release_admin_ ? "on the admin's Products page"
									: "on its page under Projects on the site") +
					" once it is a draft (" + release_server_ +
					(release_admin_ ? "/admin/products/" : "/me/projects/") + release_key_ + ").";
			core_.note("NovaWorld: " + view.last);
		} else {
			view.last = "The release was refused: " +
					release_refusal(error, release_.reply(), release_key_);
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
	// The setup, then a check, once the slot is free.
	if (!core_.operations().running()) {
		if (track_due_) {
			track(false);
		} else if (check_due_) {
			check_due_ = false;
			start_check();
		}
	}
	const bool calling = login_.started() || projects_.started() || games_.started() ||
			project_.started() || put_.started() || release_.started() || history_.started();
	if (view.calling != calling) {
		view.calling = calling;
		core_.touch(ViewConcern::NovaWorld);
	}
}

bool NovaWorldController::busy() const {
	return login_.started() || projects_.started() || games_.started() || project_.started() ||
			put_.started() || put_waiting_ || release_.started() || history_.started() ||
			track_due_ || check_due_;
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
	NovaWorldView &view = core_.view().novaworld;
	view.checked = false;
	view.changes.clear();
	view.problems.clear();
	view.remote_known = false;
	view.remote = RemoteProject();
	view.head_known = false;
	view.head = 0;
	view.head_error.clear();
	view.put = NovaWorldView::PutPlan(); // another project's, if any: a check finds this one's
	put_due_ = false;
	show_copy();
	if (view.key.empty())
		return;
	// The project as the service has it now; a folder that is no working copy of it on this service
	// yet is set up once the Open's own work is done (a game you own's import goes first).
	if (view.tracked)
		ask_project();
	else
		track_due_ = true;
}

void NovaWorldController::forget_project() {
	if (get_dir_.empty())
		paused_.reset();
	NovaWorldView &view = core_.view().novaworld;
	view.key.clear();
	view.tracked = false;
	view.revision = 0;
	view.remote_known = false;
	view.remote = RemoteProject();
	view.put = NovaWorldView::PutPlan();
	view.checked = false;
	view.head_known = false;
	view.head_error.clear();
	view.changes.clear();
	view.problems.clear();
	project_ = NovaWorldCall(); // the closed project's ask
	track_due_ = false;
	check_due_ = false;
	check_after_track_ = false;
	put_due_ = false;
	history_ = NovaWorldCall(); // a list in flight is the closed project's
	view.history.clear();
	view.history_error.clear();
	show_conflicts();
}

void NovaWorldController::reset_found(int64_t head) {
	NovaWorldView &view = core_.view().novaworld;
	view.last = "NovaWorld was reset: " + view.key + "'s newest revision is " +
			std::to_string(head) + ", before the folder's " + std::to_string(view.revision) +
			". The folder is set up as its working copy again.";
	core_.note("NovaWorld: " + view.last);
	view.checked = false;
	view.changes.clear();
	track(true);
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
			// A get of a project with no revision yet (its head as the pull read it, whatever the
			// list said): nothing written, no working copy. An expansion's base with none: the
			// expansion opens.
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
			show_copy();
			return;
		}
		view.last = "Pulled revision " + std::to_string(pull.revision()) + ": " +
				count_text(pull.applied().written, "file written", "files written") + ", " +
				count_text(pull.applied().removed, "removed", "removed") + ".";
		core_.note("NovaWorld: " + view.last);
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
				// A retail game's project (its files the game install's, private on NovaWorld) and one
				// the account may not read (never listed) are got by none: the mod builds on the
				// install's own project of that game, made there.
				const RemoteProject *base = listed_project(view, base_key);
				if (base_project_folder(dir, base_dir) && empty_folder(base_dir) &&
						(!base || base->retail)) {
					const std::string skipped = "Its base game's project, " + base_key + ", is " +
							(base ? std::string("a retail game's, whose files are your game install's")
								  : std::string("not one this account may read")) +
							", so it is not got: make the game's project from your game install in " +
							base_dir + " (New project, A game you own) for the mod to build on.";
					view.last += " " + skipped;
					core_.note("NovaWorld: " + skipped);
				} else if (!base_dir.empty() && empty_folder(base_dir)) {
					get_dir_ = base_dir;
					get_open_after_ = dir;
					view.last += " Getting its base game's project, " + base_key + ", into " + base_dir + ".";
					start(OperationKind::NovaWorldPull,
							std::make_unique<NovaWorldPull>(*transport_, account_,
									ProjectPaths::for_root(base_dir), "",
									NovaWorldWorkingCopy{ base_key, 0 }));
					return;
				}
			}
			core_.open_project(dir);
			return;
		}
	}
	show_copy();
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
			view.remote.head = push.revision();
			view.checked = true;
			view.changes.clear();
			show_copy();
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
	// The open project's answer (a get's pull is another folder's project).
	const bool open_project = kind != OperationKind::NovaWorldPull || get_dir_.empty();
	if (open_project)
		take_remote(*job);
	switch (job->state()) {
		case NovaWorldJobState::Failed: {
			// A project the service does not show the account says so as the view does.
			const std::string why = open_project && job->not_found()
					? view.head_error
					: std::string(operation_kind_row(kind).noun) + " failed: " + job->error();
			const Diagnostic failed =
					make_finding(CoreFinding::NovaWorldFailed, DiagnosticSeverity::Warning, why);
			core_.refuse_request(failed); // the status line and Output: no problem of the project's
			outcome.end = OperationEnd::Failed;
			outcome.findings.push_back(failed);
			view.last = failed.message;
			if (kind == OperationKind::NovaWorldTrack)
				check_after_track_ = false;
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
			if (kind == OperationKind::NovaWorldTrack)
				check_after_track_ = false;
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
			// The store reset under the folder: its working copy set up again, then checked.
			if (job->reset() && open_project) {
				reset_found(job->remote().head);
				break;
			}
			if (kind == OperationKind::NovaWorldCheck) {
				const auto &checked = static_cast<NovaWorldCheck &>(*job);
				view.checked = true;
				view.head_known = checked.remote_known();
				if (!checked.remote_known() && !checked.not_found())
					view.head_error = checked.remote_error();
				view.changes = checked.changes();
				view.problems = checked.problems();
				view.last = count_text(view.changes.size(), "change", "changes") +
						(view.head_known ? "; the head is revision " + std::to_string(view.head) +
												"."
										 : "; the service did not answer.");
			} else if (kind == OperationKind::NovaWorldTrack) {
				const auto &tracked = static_cast<NovaWorldTrack &>(*job);
				const NovaWorldWorkingCopy &copy = tracked.copy();
				show_copy();
				view.checked = false;
				view.changes.clear();
				view.problems.clear();
				view.last = "The folder is " + copy.key + "'s working copy on " +
						novaworld_host(account_.server) +
						(tracked.differs()
										? ", but its files differ from revision " +
												std::to_string(tracked.remote().head) +
												", so it holds no revision of it yet: pull, settling "
												"each file both hold otherwise, then push."
										: ", at revision " + std::to_string(copy.revision) + ".");
				core_.note("NovaWorld: " + view.last);
				if (check_after_track_) {
					check_after_track_ = false;
					start_check();
				}
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
