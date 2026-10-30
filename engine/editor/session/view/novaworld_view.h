#pragma once

// What the editor shows of NovaWorld (ADR 0069; session/novaworld_controller.h): the account
// (its service, name and role, never its session cookie or a password), the projects it may read,
// the open project's place on NovaWorld (the key its project file names, its working copy on the
// account's service, the project as the service last answered, and what Put on NovaWorld would make
// of a project with no key) and its standing (its changes since its base, the project's head), a
// pull's conflicts while they wait for the author, and what the last NovaWorld job came to. The
// NovaWorld window draws it, the view JSON's novaworld section is it.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <editor/novaworld/novaworld_api.h>
#include <editor/novaworld/novaworld_plan.h>

namespace opennova::editor {

// A file a pull's revision and the folder both changed otherwise: what each did to the base's file.
struct NovaWorldConflictView {
	std::string path;
	std::optional<NovaWorldChange> mine;
	std::optional<NovaWorldChange> theirs;
};

struct NovaWorldView {
	// The host gave the session a way to reach a service (ProjectSession::set_novaworld_transport).
	bool available = false;
	// The account: the service's address, signed in or not, the account's name and role.
	std::string server;
	std::string username;
	std::string role;
	bool signed_in = false;
	// A sign-in, a project list, the open project's ask, a put, a release or a history in flight, and
	// what NovaWorld is asked in a line ("Putting MYMOD on NovaWorld..."; "" none): the NovaWorld window
	// and the status bar say it.
	bool calling = false;
	std::string waiting;
	// The projects as last listed (the public ones, the account's own, every one for an admin), and
	// why the last list failed.
	std::vector<RemoteProject> projects;
	std::string projects_error;
	// The games a new project of the signed-in account's may be a mod of (GET /api/me/projects), as
	// last listed: a put's base game is found among them, or chosen from them; and the titles a new game
	// of its may run as (the same reply's: "jo", "dfx2").
	std::vector<RemoteGame> games;
	std::vector<RemoteLobby> lobbies;
	std::vector<std::string> titles;
	// The open project's place on NovaWorld: the key its project file names ("" not on NovaWorld),
	// whether its folder is a working copy of that project on the account's service (its state
	// there), and its base revision.
	std::string key;
	bool tracked = false;
	int64_t revision = 0;
	// What the folder's own state names that its project file does not (a key taken out, or changed), the
	// way back Rejoin takes: the key, the revision the folder held of it, and the service it held it on, ""
	// this one. Read only for a folder that is no working copy here.
	std::string former_key;
	int64_t former_revision = 0;
	std::string former_host;
	// The folder's working copies of this project on other services, as its own state has them: one folder,
	// a debug build's staging and a release build's live service each keeping a base of their own.
	struct OtherCopy {
		std::string host;
		int64_t revision = 0;
	};
	std::vector<OtherCopy> other_copies;
	// The project as the service last answered (the open project's ask, its setup, a check, a pull
	// or a push): whether the account may push it is its can_push.
	bool remote_known = false;
	RemoteProject remote;
	// The open project's lobby file: the gt.ssc the service generated for its lobby, as cached under
	// the folder's .opennova for this service, project and title (novaworld_state.h's
	// cached_novaworld_gate_tag; "" none cached). Found once as the project opens, as each answer of
	// the service's is cached and as a build starts, never as a plan's target is asked: a build of
	// a project on NovaWorld ships it (SessionCore::build_target), and refuses without it.
	std::string gate_tag_file;
	// An expansion's version.txt the service generated for its published release (the CRC a joiner must
	// match, novaworld_state.h's cached_novaworld_version_file; "" none: a game, or nothing published),
	// found and shipped as the lobby file is; a build without it ships none.
	std::string version_file;
	// What Put on NovaWorld makes of the open project as a new project of the account's (its
	// project file naming no key): a "game" of `game` (the title code it runs as) or an
	// "expansion" of the game `base` (its NovaWorld key, "" none found: the put names it) as
	// `expansion_code`; `refusal` why it cannot ("" it can: an expansion's name the service's code
	// rule refuses).
	struct PutPlan {
		std::string kind;
		std::string game;
		std::string base;
		std::string expansion_code;
		std::string refusal;
	};
	PutPlan put;
	// The base game's project New project's mod is made on, for the form's
	// NovaWorld game `key` and folder `for_dir` as the session's resolver finds it: `dir`, a project of it on
	// this computer (`found`), else where one is made first (`from_install`: imported from the game install,
	// else got from NovaWorld). `key` "": none to say (no mod, no folder, the game not listed).
	struct NewBase {
		std::string key;
		std::string for_dir;
		std::string dir;
		bool found = false;
		bool from_install = false;
	};
	NewBase new_base;
	// The open mod's base game's project folder, where its project file names one that holds no project (a got
	// mod's the editor could not make: no install of its game); "" none. novaworld_make_base makes it.
	std::string missing_base;
	// Why the service refused the last put, in a line ("" none, or a put asked since): the Put dialog says it.
	std::string put_refusal;
	// Its standing as the last check, pull or push left it: the project's head (when the service
	// answered; the project's ask and its setup set it too), the folder's changes since its base,
	// and what kept a file out of them. `head_error` says why the head is unknown: for a project
	// the service does not show the account, "<KEY> is not on <host>, or is private: ...", with
	// `head_missing` set (a 404, where anything else is the service not answering). `setting_up`: the
	// folder's setup is due or running. novaworld_standing.h reads all of this one way.
	bool checked = false;
	bool head_known = false;
	int64_t head = 0;
	std::string head_error;
	bool head_missing = false;
	bool setting_up = false;
	std::vector<LocalChange> changes;
	std::vector<std::string> problems;
	// A pull stopped on conflicts: the revision it pulls, and each conflict still to settle.
	int64_t pull_revision = 0;
	std::vector<NovaWorldConflictView> conflicts;
	// The open project's releases as last listed, newest first (asked as the project is, for an account
	// that may release it), and why the last list failed.
	std::vector<RemoteRelease> releases;
	std::string releases_error;
	// The project's revisions as last listed (novaworld_history), newest first, and why the last list
	// failed.
	std::vector<RemoteRevision> history;
	std::string history_error;
	// What the last NovaWorld job or call came to, one line ("Pushed revision 42.").
	std::string last;
};

} // namespace opennova::editor
