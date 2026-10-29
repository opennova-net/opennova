#include <editor/session/novaworld_controller.h>

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <utility>

#include <base/io/file_io.h>
#include <base/io/json.h>
#include <base/io/os_path.h>
#include <base/io/strutil.h>
#include <base/project/project_manifest.h>
#include <base/title/title.h>
#include <base/version/version.h>
#include <base/vfs/vfs.h>
#include <editor/assets/install_check.h>
#include <editor/import/import_run.h>
#include <editor/model/core_finding.h>
#include <editor/model/document_base.h>
#include <editor/model/diagnostic.h>
#include <editor/project/base_project.h>
#include <editor/project/local_settings.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>
#include <editor/session/base_setup.h>
#include <editor/session/disk_watch.h>
#include <editor/session/editor_preferences.h>
#include <editor/session/novaworld_operation.h>
#include <editor/session/request_factories.h>
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

// Why the account may not put `doc` on NovaWorld at all ("" it may): a standalone project holding the game's own
// files (imported from a game install: its archives recorded) is the game's, and only an admin puts a retail
// game's project there.
std::string own_game_refusal(const ProjectDocument &doc, bool admin) {
	if (admin || !doc.expansion.standalone() || doc.archives.empty())
		return {};
	return "The project holds the game's own files, imported from your game install, which are not yours to put "
		   "on NovaWorld: make a mod of the game instead (New project, A mod of a game).";
}

// What an operation of `kind` is called at the start of a line ("The pull", "The get" for a get's pull).
std::string noun_of(OperationKind kind, bool get) {
	std::string noun = get && kind == OperationKind::NovaWorldPull ? "the get" : operation_kind_row(kind).noun;
	if (!noun.empty() && noun[0] >= 'a' && noun[0] <= 'z')
		noun[0] = char(noun[0] - 'a' + 'A');
	return noun;
}

// Why the service shows the account no project `key` (a 404): one it lacks, or a private one.
std::string unreadable_text(const std::string &key, const std::string &server) {
	return key + " is not on " + novaworld_host(server) +
			", or is private: sign in with an account that may read it, or on the NovaWorld server it is "
			"on.";
}

// A refusal in a line's plain words: the service's own sentence for a code it knows, the message otherwise
// (never the code and the HTTP status a person cannot act on).
std::string plain(const NovaWorldApiError &error) {
	if (error.code == "transport")
		return "NovaWorld did not answer (" + error.message + "): check its address and the connection.";
	if (error.code == "invalid_credentials")
		return "the account or its password is not right.";
	if (error.code == "unauthorized")
		return "your NovaWorld session ended: sign in again.";
	if (error.code == "forbidden")
		return "NovaWorld does not let this account do that.";
	if (error.code == "not_found")
		return "NovaWorld has no such project, or it is private.";
	if (error.code == "rate_limited")
		return "NovaWorld asks to wait a minute before trying again.";
	if (error.status >= 500)
		return "NovaWorld had a problem (HTTP " + std::to_string(error.status) + "): try again in a minute.";
	return !error.message.empty() ? error.message : error.line();
}

// Why the service would not make a new project of the account's (POST /api/products), by its code;
// `code` the mod's short name asked for.
std::string created_refusal(
		const NovaWorldApiError &error, const std::string &key, const std::string &code) {
	if (error.code == "duplicate_key")
		return "NovaWorld has a project " + key + " already: choose another key.";
	if (error.code == "invalid_field" && error.field == "key")
		return "NovaWorld takes no key " + key + ": up to 11 of A-Z, 0-9 and _, without \"QQ\".";
	// A mod's short name is NovaWorld's own across every game (expansion\<code>\ in any install).
	if (error.code == "duplicate_expansion" ||
			(error.status == 409 && error.field == "expansion_code"))
		return "another mod on NovaWorld uses the short name " + code +
				" already: a mod keeps the short name it is made with, so make it again under another "
				"(New project, A mod of a game) and copy its files in, then put that one on NovaWorld.";
	if (error.code == "too_many_projects")
		return "the account has as many projects as NovaWorld keeps for it: delete one on the "
			   "site first.";
	if (error.code == "no_such_base")
		return "NovaWorld has no such base game: name the game it changes (base_key).";
	if (error.code == "rate_limited")
		return "NovaWorld asks to wait before making another project: try again in a minute.";
	return plain(error);
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
	return plain(error);
}

// A game's name by its title code ("Joint Operations"), the code for one the editor has no title
// of.
std::string game_words(const std::string &code) {
	const Title *title = title_by_code(code);
	return title && title->display_name ? title->display_name : code;
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

bool novaworld_player_key(const std::string &key) {
	return is_novaworld_key(key) && key.size() <= kPlayerKeyMost && key.find("QQ") == std::string::npos;
}

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
	list_projects(); // what New project offers is NovaWorld's
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
	view.lobbies.clear();
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
	// A mod's base game's project named where no project is (a got mod whose base the editor could not make).
	view.missing_base.clear();
	if (open && core_.view().project.document->expansion.on_base_project()) {
		const std::string base = io::utf8_generic_path(
				(io::os_path(core_.paths().root) /
						io::os_path(core_.view().project.document->expansion.base_project))
						.lexically_normal());
		std::error_code ec;
		if (!fs::is_regular_file(system_path(base + "/" + kProjectFileName), ec))
			view.missing_base = base;
	}
	// What a put would make of it is found as a check asks (put_due_), only while it has no key.
	if (!open || !view.key.empty())
		view.put = NovaWorldView::PutPlan();
	show_lobby();
	core_.touch(ViewConcern::NovaWorld);
}

NovaWorldView::PutPlan NovaWorldController::put_plan() const {
	NovaWorldView::PutPlan plan;
	const ProjectDocument &doc = *core_.view().project.document;
	plan.game = doc.target_game;
	if (doc.expansion.standalone()) {
		plan.kind = "game";
		plan.refusal = own_game_refusal(doc, account_.admin());
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
	show_calls();
}

void NovaWorldController::take_remote(const NovaWorldJob &job) {
	NovaWorldView &view = core_.view().novaworld;
	if (job.remote_known()) {
		cache_lobby(core_.paths(), job.remote());
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

void NovaWorldController::cache_lobby(const ProjectPaths &paths, const RemoteProject &project) {
	const NovaWorldPaths cache = NovaWorldPaths::for_project(paths, account_.server);
	std::string error;
	// An expansion's version.txt, the answer's or none: an older one never stands for a newer release.
	if (!cache_novaworld_version_file(cache, project.key, project.version_file, error))
		core_.note("NovaWorld version file was not cached: " + error);
	error.clear();
	if (project.lobby && cache_novaworld_lobby(cache, project.key, *project.lobby, error)) {
		show_lobby();
		return;
	}
	// A fresh answer supersedes the old selection. An absent or malformed configuration
	// cannot leave the previous lobby authoritative for subsequent offline builds.
	std::error_code ignored;
	if (!cache.lobby_file.empty())
		fs::remove(system_path(cache.lobby_file), ignored);
	core_.note("NovaWorld lobby configuration was not cached: " +
			(error.empty() ? "the service returned no lobby file" : error));
	show_lobby();
}

void NovaWorldController::refresh_lobby() {
	if (!project_.started())
		ask_project();
}

void NovaWorldController::show_lobby() {
	NovaWorldView &view = core_.view().novaworld;
	std::string file, version;
	if (core_.view().project.open && !core_.view().project.document->novaworld.empty()) {
		const NovaWorldPaths cache = NovaWorldPaths::for_project(core_.paths(), account_.server);
		const ProjectDocument &doc = *core_.view().project.document;
		file = cached_novaworld_gate_tag(cache, doc.novaworld, title_protocol_code(doc.target_game));
		// The service answers one for an expansion alone; a build of one ships it (SessionCore::build_target).
		version = cached_novaworld_version_file(cache, doc.novaworld);
	}
	if (file == view.gate_tag_file && version == view.version_file)
		return;
	view.gate_tag_file = std::move(file);
	view.version_file = std::move(version);
	core_.touch(ViewConcern::NovaWorld);
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
	core_.view().novaworld.head_error.clear();
	core_.start_operation(std::make_unique<NovaWorldOperation>(OperationKind::NovaWorldTrack, *this,
			std::make_unique<NovaWorldTrack>(
					*transport_, account_, core_.paths(), export_dir(), view.key)));
	core_.touch(ViewConcern::NovaWorld);
}

bool NovaWorldController::start_check() {
	NovaWorldWorkingCopy copy;
	if (!transport_ || !working_copy(copy))
		return false;
	files_seen_ = core_.view().revisions.of(ViewConcern::Files);
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
	show_calls();
}

void NovaWorldController::sign_out() {
	if (transport_ && account_.signed_in())
		logout_ = NovaWorldCall(*transport_, novaworld_api::logout(account_)); // its answer is no one's
	NovaWorldAccountUpdate update{ true, account_ };
	update.account.cookie.clear();
	update.account.role.clear();
	keep_account(update);
	core_.view().novaworld.last = "Signed out of " + novaworld_host(account_.server) + ".";
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
	if (!account_.signed_in()) {
		core_.view().novaworld.games.clear();
		core_.view().novaworld.lobbies.clear();
		core_.view().novaworld.titles.clear();
	}
	show_calls();
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
	if (const std::string own = own_game_refusal(doc, account_.admin()); !own.empty()) {
		refuse(own);
		return;
	}
	if (request.create && !account_.admin() && !novaworld_player_key(request.project_key)) {
		refuse("A key of yours is up to 11 of A-Z, 0-9 and _, without \"QQ\": " + request.project_key + " is not.");
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
		made.lobby = request.lobby;
		made.new_lobby_name = request.new_lobby_name;
		if (made.kind == "expansion" && made.base.empty()) {
			refuse("Name the NovaWorld game the mod changes (base_key): the editor finds none.");
			return;
		}
		ask = novaworld_api::create_project(account_, made);
	} else {
		ask = novaworld_api::project(account_, request.project_key);
	}
	core_.view().novaworld.put_refusal.clear();
	put_root_ = core_.paths().root;
	put_server_ = account_.server;
	put_key_ = request.project_key;
	put_code_ = doc.expansion.name;
	put_create_ = request.create;
	put_ = NovaWorldCall(*transport_, std::move(ask));
	show_calls();
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
	cache_lobby(core_.paths(), put_project_);
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
	if (paused_->plan().conflicts() == 0) {
		// The pull goes on by itself, unless an unsaved edit waits (a pull is the unsaved prompt's to start).
		const auto &open = core_.view().documents.open;
		const bool unsaved = std::any_of(open.begin(), open.end(),
				[](const auto &document) { return document && document->unsaved(); });
		if (!unsaved) {
			core_.view().novaworld.last = "Every conflict is settled: the pull goes on.";
			pull();
			return;
		}
		core_.view().novaworld.last = "Every conflict is settled: pull to go on.";
	}
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
	// The release route's rule (Version's one): one to four numbers between dots.
	if (!Version::parse(request.version)) {
		refuse("A release names its game version: one to four numbers between dots (0.2.0, 1.0).");
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
	release_version_ = request.version;
	release_admin_ = account_.admin();
	release_ = NovaWorldCall(*transport_,
			novaworld_api::release(account_, view.key, request.version, view.revision));
	show_calls();
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
	show_calls();
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
			view.last = "Signed in to " + novaworld_host(account.server) + " as " + account.username + ".";
			core_.note("NovaWorld: " + view.last);
			list_projects();
			account_changed();
		} else {
			view.last = "The NovaWorld sign-in failed: " + plain(error);
			core_.refuse_request(make_finding(
					CoreFinding::NovaWorldFailed, DiagnosticSeverity::Warning, view.last));
		}
		login_ = NovaWorldCall();
		core_.touch(ViewConcern::NovaWorld);
	}
	if (projects_.started() && projects_.poll()) {
		session_ended(projects_.reply());
		NovaWorldApiError error;
		std::vector<RemoteProject> projects;
		if (novaworld_api::read_projects(projects_.reply(), projects, error)) {
			view.projects = std::move(projects);
			view.projects_error.clear();
		} else {
			view.projects_error = plain(error);
		}
		projects_ = NovaWorldCall();
		core_.touch(ViewConcern::NovaWorld);
	}
	if (games_.started() && games_.poll()) {
		session_ended(games_.reply());
		std::vector<RemoteGame> games;
		NovaWorldApiError error;
		// A list that fails leaves the games as they were: a new project names its base itself
		// then.
		std::vector<std::string> titles;
		std::vector<RemoteLobby> lobbies;
		if (novaworld_api::read_my_games(games_.reply(), games, error) &&
				novaworld_api::read_my_titles(games_.reply(), titles, error) &&
				novaworld_api::read_my_lobbies(games_.reply(), lobbies, error)) {
			view.games = std::move(games);
			view.lobbies = std::move(lobbies);
			view.titles = std::move(titles);
		}
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
	// A project New project made to be put on NovaWorld, open now: put once its Open's work is done
	// and the lists a put plan reads are in.
	if (put_after_open_ && !core_.operations().running() && !projects_.started() &&
			!games_.started()) {
		put_after_open_ = false;
		const EditorRequest ask = std::move(pending_put_);
		pending_put_ = EditorRequest();
		if (core_.view().project.open && view.key.empty())
			put(ask);
	}
	if (project_.started() && project_.poll()) {
		RemoteProject remote;
		std::optional<RemoteRevision> head;
		NovaWorldApiError error;
		const bool read = novaworld_api::read_project(project_.reply(), remote, head, error);
		session_ended(project_.reply());
		project_ = NovaWorldCall();
		// The open project's answer, unless it is another project's now.
		if (core_.view().project.open && view.key == project_key_) {
			if (read)
				cache_lobby(core_.paths(), remote);
			view.remote_known = read;
			view.remote = read ? remote : RemoteProject();
			view.head_known = read;
			view.head = read ? remote.head : 0;
			view.head_error = read		  ? std::string()
					: error.status == 404 ? unreadable_text(view.key, account_.server)
										  : plain(error);
			// A project the account may read, its folder no working copy of it here yet: set up.
			if (read && !view.tracked)
				track(false);
			if (read)
				list_releases();
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
		session_ended(put_.reply());
		put_ = NovaWorldCall();
		std::string refused;
		// An empty project is the open project's to take only where it is what the project is: a
		// game or a mod, of its game, a mod by its short name (the project closed or changed since,
		// finish_put says so).
		const bool same_open = core_.view().project.open && core_.paths().root == put_root_;
		if (!read)
			refused = put_create_		  ? created_refusal(error, put_key_, put_code_)
					: error.status == 404 ? unreadable_text(put_key_, put_server_)
										  : plain(error);
		else if (!put_create_ && project.head > 0)
			refused = put_key_ +
					" has revisions already, so it is no empty project: get it (Get...) instead.";
		else if (!project.can_push)
			refused = novaworld_push_refusal(put_key_);
		else if (!put_create_ && same_open)
			refused = novaworld_put_mismatch(*core_.view().project.document, project);
		if (!refused.empty()) {
			view.put_refusal = refused; // the Put dialog says it under its fields
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
		session_ended(release_.reply());
		if (novaworld_api::read_release(release_.reply(), id, name, error)) {
			// Where the account takes it on once it is a draft: an admin publishes it on the admin's
			// Products page; an owner submits it on its project page under Projects, and an admin
			// approves it before players get it (the release they get now stays theirs meanwhile).
			view.last = "Release " + name + " is building on " + novaworld_host(release_server_) +
					(release_admin_ ? ": publish it on the admin's Products page once it is a draft ("
									: ": once it is a draft, submit it for approval on its page under "
									  "Projects on the site; an admin approves it before players get it (") +
					release_server_ + (release_admin_ ? "/admin/products/" : "/me/projects/") +
					release_key_ + ").";
			core_.note("NovaWorld: " + view.last);
			release_ = NovaWorldCall();
			list_releases(); // the new one among them, building
		} else {
			view.last = "The release was refused: " +
					release_refusal(error, release_.reply(), release_key_);
			core_.refuse_request(make_finding(
					CoreFinding::NovaWorldFailed, DiagnosticSeverity::Warning, view.last));
		}
		release_ = NovaWorldCall();
		core_.touch(ViewConcern::NovaWorld);
	}
	if (releases_.started() && releases_.poll()) {
		session_ended(releases_.reply());
		NovaWorldApiError error;
		std::vector<RemoteRelease> releases;
		if (novaworld_api::read_releases(releases_.reply(), releases, error)) {
			view.releases = std::move(releases);
			view.releases_error.clear();
		} else {
			view.releases_error = plain(error);
		}
		releases_ = NovaWorldCall();
		core_.touch(ViewConcern::NovaWorld);
	}
	if (submit_.started() && submit_.poll()) {
		session_ended(submit_.reply());
		NovaWorldApiError error;
		RemoteRelease submitted;
		if (novaworld_api::read_submitted(submit_.reply(), submitted, error)) {
			view.last = submitted.status == "published"
					? submit_name_ + " is published: players update to it."
					: submit_name_ + " is submitted: an admin approves it before players get it; the release "
									 "they get now stays theirs meanwhile.";
			core_.note("NovaWorld: " + view.last);
		} else {
			view.last = submit_name_ + " was not submitted: " +
					(error.code == "not_draft" ? std::string("it is no draft (built and checked) any more.")
											   : plain(error));
			core_.refuse_request(make_finding(
					CoreFinding::NovaWorldFailed, DiagnosticSeverity::Warning, view.last));
		}
		submit_ = NovaWorldCall();
		list_releases();
		core_.touch(ViewConcern::NovaWorld);
	}
	if (history_.started() && history_.poll()) {
		NovaWorldApiError error;
		std::vector<RemoteRevision> revisions;
		session_ended(history_.reply());
		if (novaworld_api::read_revisions(history_.reply(), revisions, error)) {
			view.history = std::move(revisions);
			view.history_error.clear();
		} else {
			view.history_error = plain(error);
		}
		history_ = NovaWorldCall();
		core_.touch(ViewConcern::NovaWorld);
	}
	// A change to the project's files since the last check (a save, an import, a refresh): its changes
	// are counted again by the next check (a push finds them itself meanwhile).
	if (core_.view().project.open && view.tracked) {
		const uint64_t files = core_.view().revisions.of(ViewConcern::Files);
		if (files != files_seen_) {
			files_seen_ = files;
			if (view.checked) {
				view.checked = false;
				core_.touch(ViewConcern::NovaWorld);
			}
		}
	}
	show_new_base();
	// The setup, then a check, once the slot is free.
	if (!core_.operations().running()) {
		if (track_due_) {
			track(false);
		} else if (check_due_) {
			check_due_ = false;
			start_check();
		}
	}
	show_calls();
}

bool NovaWorldController::session_ended(const NovaWorldResponse &reply) {
	if (!reply.transport_ok || reply.status != 401 || !account_.signed_in())
		return false;
	NovaWorldAccountUpdate update{ true, account_ };
	update.account.cookie.clear();
	update.account.role.clear();
	keep_account(update);
	core_.view().novaworld.last = "Your NovaWorld session ended: sign in again.";
	core_.note("NovaWorld: " + core_.view().novaworld.last);
	core_.touch(ViewConcern::NovaWorld);
	return true;
}

void NovaWorldController::make_own() {
	if (!ready(true, false))
		return;
	const ProjectDocument &doc = *core_.view().project.document;
	if (doc.novaworld.empty()) {
		refuse("The project is not on NovaWorld: Put on NovaWorld... puts it there.");
		return;
	}
	const std::string key = doc.novaworld;
	ProjectDocument project = doc;
	project.novaworld.clear();
	Diagnostic error;
	if (!core_.write_project(project, error)) {
		refuse("The project file was not written (" + error.message + "): it still names " + key + ".");
		return;
	}
	paused_.reset();
	show_conflicts();
	project_opened(); // its place on NovaWorld read again: none
	view_last("The project names " + key + " no longer: Put on NovaWorld... makes it a project of yours, its files "
			  "yours to push.");
}

void NovaWorldController::make_missing_base(const EditorRequest &request) {
	if (!ready(true, false))
		return;
	const NovaWorldView &view = core_.view().novaworld;
	if (view.missing_base.empty()) {
		refuse("The project's base game's project is not missing.");
		return;
	}
	if (!view.remote_known || view.remote.base.empty()) {
		refuse("NovaWorld has not said which game the mod is of: Check first.");
		return;
	}
	const std::string dir = view.missing_base;
	const std::string key = view.remote.base;
	const RemoteProject *listed = listed_project(view, key);
	const std::string title = listed && !listed->title.empty() ? listed->title : key;
	start_over_half_made(dir);
	if (!empty_folder(dir)) {
		refuse(dir + " holds files and no project of " + title + ": move them out, then make it again.");
		return;
	}
	const std::string install = request.game_install.empty() ? core_.game_install() : request.game_install;
	std::string why;
	if (!make_base(BaseSetup{ dir, key, title, request::open_project(core_.paths().root) },
				core_.view().project.document->target_game, install, why)) {
		refuse(why + " Name one in Project settings (Game install).");
		return;
	}
	view_last("Making " + title + "'s project in " + dir + " for the mod to build on.");
}

void NovaWorldController::list_releases() {
	const NovaWorldView &view = core_.view().novaworld;
	if (!transport_ || !account_.signed_in() || !core_.view().project.open || view.key.empty() ||
			!view.remote_known || !view.remote.can_release || releases_.started())
		return;
	releases_ = NovaWorldCall(*transport_, novaworld_api::releases(account_, view.key));
	show_calls();
}

void NovaWorldController::submit(const EditorRequest &request) {
	if (!ready(true, true))
		return;
	const NovaWorldView &view = core_.view().novaworld;
	if (!account_.signed_in() || (view.remote_known && !view.remote.can_release)) {
		refuse(release_rights_text(view.key));
		return;
	}
	const RemoteRelease *draft = nullptr;
	for (const RemoteRelease &release : view.releases)
		if (!draft && release.status == "draft" && release.version == request.version)
			draft = &release;
	if (!draft) {
		refuse(view.key + " has no draft release " + request.version +
				" to submit: a release is a draft once NovaWorld has built and checked it.");
		return;
	}
	if (submit_.started()) {
		refuse("A release is being submitted already.");
		return;
	}
	submit_name_ = draft->name.empty() ? view.key + " " + draft->version : draft->name;
	submit_ = NovaWorldCall(*transport_, novaworld_api::submit_release(account_, view.key, draft->id));
	show_calls();
}

void NovaWorldController::open_site(const EditorRequest &request) {
	if (account_.server.empty()) {
		refuse("Sign in to NovaWorld first, or name its server.");
		return;
	}
	std::string path;
	if (request.page == "register" || request.page == "forgot") {
		path = "/" + request.page;
	} else if (request.page == "project") {
		const std::string &key = core_.view().novaworld.key;
		if (!core_.view().project.open || key.empty()) {
			refuse("The project is not on NovaWorld: put it there first (Put on NovaWorld...).");
			return;
		}
		path = (account_.admin() ? "/admin/products/" : "/me/projects/") + key;
	} else {
		refuse("NovaWorld's site has no page \"" + request.page + "\": register, forgot or project.");
		return;
	}
	ViewEvent open;
	open.kind = ViewEventKind::OpenExternally;
	open.path = account_.server + path;
	core_.view().events.post(std::move(open));
	core_.view().novaworld.last = "Opened " + account_.server + path + " in the browser.";
	core_.touch(ViewConcern::NovaWorld);
}

void NovaWorldController::show_calls() {
	NovaWorldView &view = core_.view().novaworld;
	const bool calling = login_.started() || projects_.started() || games_.started() ||
			project_.started() || put_.started() || release_.started() || history_.started() ||
			releases_.started() || submit_.started();
	// The one most worth saying, a person's own ask first.
	const std::string waiting = put_.started() ? "Putting " + put_key_ + " on NovaWorld..."
			: release_.started() ? "Asking NovaWorld to build " + release_key_ + " " + release_version_ + "..."
			: submit_.started()	 ? "Submitting " + submit_name_ + "..."
			: login_.started()	 ? "Signing in to " + novaworld_host(login_server_) + "..."
			: history_.started() ? "Listing " + view.key + "'s revisions..."
			: projects_.started() || games_.started() ? std::string("Listing NovaWorld's projects...")
			: project_.started() ? "Asking NovaWorld for " + project_key_ + "..."
								 : std::string();
	if (view.calling != calling || view.waiting != waiting) {
		view.calling = calling;
		view.waiting = waiting;
		core_.touch(ViewConcern::NovaWorld);
	}
}

bool NovaWorldController::busy() const {
	return login_.started() || projects_.started() || games_.started() || project_.started() ||
			put_.started() || put_waiting_ || release_.started() || history_.started() ||
			releases_.started() || submit_.started() ||
			track_due_ || check_due_ || put_after_open_;
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
	// The project New project made to be put on NovaWorld: put once the slot is free (poll). Any
	// other's open gives that put up.
	put_after_open_ = !pending_put_root_.empty() && view.key.empty() &&
			io::os_path(pending_put_root_).lexically_normal() ==
					io::os_path(core_.paths().root).lexically_normal();
	pending_put_root_.clear();
	if (!put_after_open_)
		pending_put_ = EditorRequest();
	if (view.key.empty())
		return;
	// The project as the service has it now, and its changes counted; a folder that is no working copy
	// of it on this service yet is set up (then checked) once the Open's own work is done (a game you own's
	// import goes first).
	if (view.tracked) {
		ask_project();
		check_due_ = true;
	} else {
		track_due_ = true;
		check_after_track_ = true;
	}
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
	view.gate_tag_file.clear();
	view.version_file.clear();
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
	put_after_open_ = false;
	history_ = NovaWorldCall(); // a list in flight is the closed project's
	view.history.clear();
	view.history_error.clear();
	releases_ = NovaWorldCall();
	view.releases.clear();
	view.releases_error.clear();
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
			// list said): nothing written, no working copy.
			get_dir_.clear();
			get_base_key_.clear();
			view.last = no_revision_text(pull.remote().key);
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
			const std::string dir = std::move(get_dir_);
			get_dir_.clear();
			const std::string base_key = std::move(get_base_key_);
			get_base_key_.clear();
			// A mod's: its base game's project made where its project file names it, unless a project
			// is there already (make_base: got from NovaWorld, or a retail game's imported from the
			// game install), the mod opened then; one the editor cannot make said so, the mod opened.
			std::string base_dir;
			if (!base_key.empty() && base_project_folder(dir, base_dir))
				start_over_half_made(base_dir);
			// Its base game's project is made inside the folder the get was asked into, never elsewhere on the
			// disk where its project file names it (another drive, a folder far above).
			const fs::path picked = io::os_path(dir).lexically_normal().parent_path();
			if (!base_dir.empty() && !io::path_lexically_within(io::os_path(base_dir), picked)) {
				const std::string outside = "Its base game's project is named at " + base_dir +
						", outside the folder you chose: make or put it there for the mod to build on.";
				view.last += " " + outside;
				core_.note("NovaWorld: " + outside);
				base_dir.clear();
			}
			if (!base_key.empty() && !base_dir.empty() && empty_folder(base_dir)) {
				ProjectDocument mod;
				Diagnostic unread;
				const std::string game = ::opennova::editor::open_project(dir, mod, unread)
						? mod.target_game
						: default_target_game();
				const RemoteProject *base = listed_project(view, base_key);
				std::string why;
				if (make_base(BaseSetup{ base_dir, base_key,
									  base && !base->title.empty() ? base->title : base_key,
									  request::open_project(dir) },
							game, core_.game_install(), why)) {
					view.last += " Making its base game's project, " + base_key + ", in " + base_dir + ".";
					core_.note("NovaWorld: Making " + base_key + "'s project in " + base_dir +
							" for the mod to build on.");
					return;
				}
				const std::string skipped = "Its base game's project, " + base_key + ", is not got: " +
						why + " Name one in Project settings, then the NovaWorld window's Make the base game's project makes "
						"it in " + base_dir + ".";
				view.last += " " + skipped;
				core_.note("NovaWorld: " + skipped);
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
	else if (job->remote_known())
		cache_lobby(ProjectPaths::for_root(get_dir_), job->remote());
	switch (job->state()) {
		case NovaWorldJobState::Failed: {
			// A project the service does not show the account says so as the view does.
			const std::string why = open_project && job->not_found()
					? view.head_error
					: noun_of(kind, !open_project) + " failed: " + job->error();
			const Diagnostic failed =
					make_finding(CoreFinding::NovaWorldFailed, DiagnosticSeverity::Warning, why);
			core_.refuse_request(failed); // the status line and Output: no problem of the project's
			outcome.end = OperationEnd::Failed;
			outcome.findings.push_back(failed);
			view.last = failed.message;
			if (kind == OperationKind::NovaWorldTrack) {
				check_after_track_ = false;
				view.head_error = why; // the window says why it is not set up
			}
			if (kind == OperationKind::NovaWorldPull) {
				get_dir_.clear();
				get_base_key_.clear();
			}
			break;
		}
		case NovaWorldJobState::Cancelled:
			outcome.end = OperationEnd::Cancelled;
			view.last = noun_of(kind, !open_project) + " was cancelled.";
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
				list_releases(); // what its releases came to since
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

void NovaWorldController::new_mod(const EditorRequest &request) {
	if (!ready(false, false))
		return;
	if (!account_.signed_in()) {
		refuse("Sign in to NovaWorld to make a mod: a mod is of one of NovaWorld's games.");
		return;
	}
	if (request.dir.empty() || request.expansion.empty()) {
		refuse("A mod names its folder and its short name.");
		return;
	}
	const RemoteGame *game = nullptr;
	for (const RemoteGame &each : core_.view().novaworld.games)
		if (each.key == request.base_key)
			game = &each;
	if (!game) {
		refuse("NovaWorld lists no game " + request.base_key +
				" a mod of yours may change: list its projects again (Check).");
		return;
	}
	const std::string title = game->title.empty() ? game->key : game->title;
	// A short name that is one of the game install's own expansions (jox01 on JO's): that expansion's project, its
	// files imported from the install as they ship (new_project's all, as the stock import makes one), never a
	// mod standing in for it. It is NovaWorld's retail project already: put nowhere as it is made.
	EditorRequest asked = request;
	const RemoteProject *listed_game = listed_project(core_.view().novaworld, game->key);
	const std::string from = request.game_install.empty() ? core_.game_install() : request.game_install;
	if ((!listed_game || listed_game->retail) && !from.empty())
		for (const std::string &name : vfs_list_expansions(absolute_install_path(from)))
			if (strutil::iequals(name, request.expansion)) {
				asked.all = true;
				asked.expansion = name;
				asked.game_install = from;
				if (asked.create) {
					asked.create = false;
					asked.project_key.clear();
				}
			}
	// A project of the game here already: the mod made on it now (its base_key kept: a put as it is made
	// names it).
	const std::string found = local_base(request.dir, *game);
	if (!found.empty()) {
		core_.note("NovaWorld: " + title + "'s project in " + found + " is the mod's base game.");
		EditorRequest on = asked;
		on.base_project = found;
		make_project(on);
		return;
	}
	// Else one made beside the mod, in a folder named by the game's key (one the editor left half-made made
	// again).
	const std::string dir = io::utf8_generic_path(
			io::os_path(request.dir).lexically_normal().parent_path() / io::os_path(game->key));
	start_over_half_made(dir);
	if (!empty_folder(dir)) {
		refuse(dir + " holds files and no project of " + title +
				": make the mod in another folder, or move them.");
		return;
	}
	EditorRequest then = asked;
	then.base_project = dir;
	const std::string install = from;
	std::string why;
	if (!make_base(BaseSetup{ dir, game->key, title, then }, game->game, install, why)) {
		refuse(why + " Name one (Game install).");
		return;
	}
	view_last("Making " + title + "'s project in " + dir + " for " +
			(request.title.empty() ? request.expansion : request.title) + " to be made on.");
}

bool NovaWorldController::make_base(
		BaseSetup setup, const std::string &game, const std::string &install, std::string &why) {
	const RemoteProject *listed = listed_project(core_.view().novaworld, setup.key);
	std::unique_ptr<BaseSetupOperation> operation;
	if (listed && !listed->retail) {
		auto pull = std::make_unique<NovaWorldPull>(*transport_, account_,
				ProjectPaths::for_root(setup.dir), "", NovaWorldWorkingCopy{ setup.key, 0 });
		operation = std::make_unique<BaseSetupOperation>(*this, std::move(setup), std::move(pull));
	} else {
		// A retail game's files are the game install's (private on NovaWorld, which players cannot get).
		if (install.empty() || install_game(install) != game) {
			why = setup.title + "'s project is made from a " + game_words(game) + " install, and the editor's " +
					(install.empty() ? std::string("game install is none.")
									 : "game install is " + game_words(install_game(install)) + "'s.");
			return false;
		}
		operation = std::make_unique<BaseSetupOperation>(
				*this, std::move(setup), install, core_.preferences().values());
	}
	start_base(std::move(operation));
	return true;
}

void NovaWorldController::start_over_half_made(const std::string &dir) {
	if (!base_setup_half_made(dir))
		return;
	std::error_code ec;
	fs::remove_all(system_path(dir), ec);
	core_.note("NovaWorld: " + (ec ? "The base game's project left half-made in " + dir +
											 " could not be removed (" + ec.message() + ")."
								   : "The base game's project left half-made in " + dir +
											 " was removed, to be made again."));
}

std::string NovaWorldController::local_base(const std::string &dir, const RemoteGame &game) const {
	std::vector<std::string> roots = core_.view().project.recent_projects;
	const fs::path mod = io::os_path(dir).lexically_normal();
	std::error_code ec;
	for (const auto &entry : fs::directory_iterator(mod.parent_path(), ec))
		if (entry.is_directory(ec))
			roots.push_back(io::utf8_generic_path(entry.path()));
	const RemoteProject *listed = listed_project(core_.view().novaworld, game.key);
	const bool imported = !listed || listed->retail;
	std::string owned;
	for (const std::string &root : roots) {
		ProjectDocument doc;
		Diagnostic unread;
		if (io::os_path(root).lexically_normal() == mod || base_setup_half_made(root) ||
				!fs::is_regular_file(system_path(root + "/" + kProjectFileName), ec) ||
				!::opennova::editor::open_project(root, doc, unread) || !doc.expansion.standalone() ||
				doc.target_game != game.game)
			continue;
		if (doc.novaworld == game.key)
			return root;
		if (imported && owned.empty() && doc.novaworld.empty() && !doc.archives.empty())
			owned = root;
	}
	return owned;
}

void NovaWorldController::show_new_base() {
	const NovaWorldView &view = core_.view().novaworld;
	const WorkspaceView::NewProject &form = core_.view().workspace.new_project;
	// Its base game: a mod's.
	const std::string key = form.making == WorkspaceView::Making::Mod ? form.base_key : std::string();
	const RemoteGame *game = nullptr;
	for (const RemoteGame &each : view.games)
		if (each.key == key)
			game = &each;
	// Shown with the form: on the welcome page (no project open) or in File > New project...'s modal.
	const bool shown = !core_.view().project.open || form.open;
	std::string asked;
	if (shown && game && !form.dir.empty()) {
		asked = key + "\n" + form.dir + "\n" + std::to_string(view.games.size()) + "\n" +
				std::to_string(view.projects.size());
		for (const std::string &recent : core_.view().project.recent_projects)
			asked += "\n" + recent;
	}
	if (asked == new_base_asked_)
		return;
	new_base_asked_ = asked;
	NovaWorldView::NewBase found;
	if (!asked.empty()) {
		found.key = key;
		found.for_dir = form.dir;
		found.dir = local_base(form.dir, *game);
		found.found = !found.dir.empty();
		if (!found.found)
			found.dir = io::utf8_generic_path(
					io::os_path(form.dir).lexically_normal().parent_path() / io::os_path(key));
		const RemoteProject *listed = listed_project(view, key);
		found.from_install = !found.found && (!listed || listed->retail);
	}
	core_.view().novaworld.new_base = found;
	core_.touch(ViewConcern::NovaWorld);
}

void NovaWorldController::make_project(const EditorRequest &r) {
	// Put on NovaWorld as it is made: the project made naming no key, the put armed as it opens
	// (project_opened).
	pending_put_ = r.create
			? request::novaworld_put_new(r.project_key, r.title, r.visibility, r.base_key, r.lobby, r.new_lobby_name)
			: EditorRequest();
	pending_put_root_ = r.create ? r.dir : std::string();
	if (!core_.new_project(r.dir, r.title, r.game, r.import_pass,
				ProjectExpansion{ r.expansion, r.builds_on, r.base_project }, r.game_install, r.all,
				r.create ? std::string() : r.project_key)) {
		pending_put_ = EditorRequest();
		pending_put_root_.clear();
	}
}

OperationOutcome NovaWorldController::finish_base(BaseSetupOperation &operation) {
	OperationOutcome outcome;
	if (NovaWorldPull *pull = operation.pull())
		keep_account(pull->take_account());
	const BaseSetup &setup = operation.setup();
	const bool opens = setup.then.kind == EditorRequestKind::OpenProject;
	if (!operation.ok()) {
		const Diagnostic failed = make_finding(CoreFinding::NovaWorldFailed, DiagnosticSeverity::Warning,
				setup.title + "'s project was not made in " + setup.dir + ": " + operation.reason() +
						(opens ? ". The mod opens without it." : ", so the mod was not made."));
		core_.refuse_request(failed);
		outcome.end = OperationEnd::Failed;
		outcome.findings.push_back(failed);
		core_.view().novaworld.last = failed.message;
		core_.touch(ViewConcern::NovaWorld);
		if (opens)
			core_.open_project(setup.then.dir);
		return outcome;
	}
	if (NovaWorldPull *pull = operation.pull(); pull && pull->remote_known())
		cache_lobby(ProjectPaths::for_root(setup.dir), pull->remote());
	view_last(setup.title + "'s project is in " + setup.dir + ".");
	// It is the editor's to find again, as a project of the author's: among the recent projects.
	core_.preferences().remember_recent_project(setup.dir);
	Diagnostic unsaved;
	if (!core_.preferences().save(unsaved))
		core_.report(unsaved);
	if (opens)
		core_.open_project(setup.then.dir);
	else
		make_project(setup.then);
	return outcome;
}

void NovaWorldController::start_base(std::unique_ptr<BaseSetupOperation> operation) {
	if (core_.start_operation(std::move(operation)) == 0)
		core_.refuse_busy(std::string());
	core_.touch(ViewConcern::NovaWorld);
}

void NovaWorldController::view_last(const std::string &words) {
	core_.view().novaworld.last = words;
	core_.note("NovaWorld: " + words);
	core_.touch(ViewConcern::NovaWorld);
}

} // namespace opennova::editor
