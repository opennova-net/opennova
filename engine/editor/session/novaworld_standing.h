#pragma once

// Where the open project stands with NovaWorld, read one way (ADR 0069, ADR 0073): what the view's
// NovaWorld section comes to, for the status bar, the NovaWorld window, Project settings and the view
// JSON's standing.state. The kind is the first that holds, the one to act on first; what the folder
// changed and what NovaWorld holds past it stay counts beside it, since a folder can be both behind and
// changed. It says what is known and no more: a 404 is NotFound, never deleted (a private project and
// another server's read the same), and changes not counted since the last save are not "up to date".

#include <cstddef>
#include <cstdint>
#include <string>

namespace opennova::editor {

struct NovaWorldView;

enum class NovaWorldStandingKind : uint8_t {
	None,		 // no project open, or one on no NovaWorld project
	WasOnNovaWorld, // on none, its folder's state naming the one it left
	SettingUp,	 // the folder being made a working copy on this service
	NotSetUp,	 // no working copy on this service, and nothing making one
	NotFound,	 // the service shows the account no such project
	Unreachable, // the service did not answer
	Conflicts,	 // a pull waits on the author
	NoRevision,	 // a working copy of a project with revisions, holding none of them
	Behind,		 // revisions past the folder's on NovaWorld
	Unchecked,	 // nothing known yet of what changed
	Changed,	 // changes of the folder's, nothing incoming
	UpToDate,	 // the folder's revision is the newest, and no change is counted
};

struct NovaWorldStanding {
	NovaWorldStandingKind kind = NovaWorldStandingKind::None;
	size_t changed = 0;	  // the changes counted, as they last were
	int64_t incoming = 0; // the revisions on NovaWorld past the folder's
	std::string line;	  // the window's words ("r5, r7 on NovaWorld, 2 changed")
	std::string tip;	  // the rest of it, the service named
	std::string elsewhere; // where the folder stands on another service ("At r12 on staging.opennova.net")
	std::string status;	  // the status bar's ("opennova.net: r5, 2 changed, 2 incoming"; "" none)
};

NovaWorldStanding novaworld_standing(const NovaWorldView &novaworld);

// The kind's word in the view JSON ("up_to_date").
const char *novaworld_standing_name(NovaWorldStandingKind kind);

// A working copy at no revision of a project that has revisions (session/novaworld_controller.h: a folder
// whose files were not the head's as it was set up): a pull comes first, a push waits for it.
bool novaworld_holds_no_revision(const NovaWorldView &novaworld);

} // namespace opennova::editor
