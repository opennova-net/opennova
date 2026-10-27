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
	std::vector<std::string> titles;
	// The open project's place on NovaWorld: the key its project file names ("" not on NovaWorld),
	// whether its folder is a working copy of that project on the account's service (its state
	// there), and its base revision.
	std::string key;
	bool tracked = false;
	int64_t revision = 0;
	// The project as the service last answered (the open project's ask, its setup, a check, a pull
	// or a push): whether the account may push it is its can_push.
	bool remote_known = false;
	RemoteProject remote;
	// What Put on NovaWorld makes of the open project as a new project of the account's (its
	// project file naming no key): a "game" of `game` (the gameprofile code it runs as) or an
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
	// Why the service refused the last put, in a line ("" none, or a put asked since): the Put dialog says it.
	std::string put_refusal;
	// Its standing as the last check, pull or push left it: the project's head (when the service
	// answered; the project's ask and its setup set it too), the folder's changes since its base,
	// and what kept a file out of them. `head_error` says why the head is unknown: for a project
	// the service does not show the account, "<KEY> is not on <host>, or is private: ...".
	bool checked = false;
	bool head_known = false;
	int64_t head = 0;
	std::string head_error;
	std::vector<LocalChange> changes;
	std::vector<std::string> problems;
	// A pull stopped on conflicts: the revision it pulls, and each conflict still to settle.
	int64_t pull_revision = 0;
	std::vector<NovaWorldConflictView> conflicts;
	// The project's revisions as last listed (novaworld_history), newest first, and why the last list
	// failed.
	std::vector<RemoteRevision> history;
	std::string history_error;
	// What the last NovaWorld job or call came to, one line ("Pushed revision 42.").
	std::string last;
};

} // namespace opennova::editor
