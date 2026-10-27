#pragma once

// What the editor shows of NovaWorld (ADR 0069; session/novaworld_controller.h): the account
// (its service, name and role, never its session cookie or a password), the projects it may read,
// the open project's binding and its standing (its changes since its base, the project's head), a
// pull's conflicts while they wait for the author, and what the last content job came to. The
// Content window draws it, the view JSON's content section is it.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <editor/novaworld/content_api.h>
#include <editor/novaworld/content_plan.h>

namespace opennova::editor {

// A file a pull's revision and the folder both changed otherwise: what each did to the base's file.
struct ContentConflictView {
	std::string path;
	std::optional<ContentChange> mine;
	std::optional<ContentChange> theirs;
};

struct NovaWorldView {
	// The host gave the session a way to reach a service (ProjectSession::set_content_transport).
	bool available = false;
	// The account: the service's address, signed in or not, the account's name and role.
	std::string server;
	std::string username;
	std::string role;
	bool signed_in = false;
	// A sign-in or a project list in flight.
	bool calling = false;
	// The projects as last listed (the public ones, every one for an admin), and why the last list
	// failed.
	std::vector<RemoteProject> projects;
	std::string projects_error;
	// The open project's binding: the content project its folder is a working copy of, and its base
	// revision.
	bool bound = false;
	std::string key;
	int64_t revision = 0;
	// Its standing as the last check, pull or push left it: the project's head (when the service
	// answered), the folder's changes since its base, and what kept a file out of them.
	bool checked = false;
	bool head_known = false;
	int64_t head = 0;
	std::string head_error;
	std::vector<LocalChange> changes;
	std::vector<std::string> problems;
	// A pull stopped on conflicts: the revision it pulls, and each conflict still to settle.
	int64_t pull_revision = 0;
	std::vector<ContentConflictView> conflicts;
	// The project's revisions as last listed (content_history), newest first, and why the last list
	// failed.
	std::vector<RemoteRevision> history;
	std::string history_error;
	// What the last content job or call came to, one line ("Pushed revision 42.").
	std::string last;
};

} // namespace opennova::editor
