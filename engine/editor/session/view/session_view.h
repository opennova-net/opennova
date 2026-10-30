#pragma once

#include <string>

#include <editor/session/editor_request.h>
#include <editor/session/view/activity_view.h>
#include <editor/session/view/dialogs_view.h>
#include <editor/session/view/documents_view.h>
#include <editor/session/view/findings_view.h>
#include <editor/session/view/navigation_view.h>
#include <editor/session/view/novaworld_view.h>
#include <editor/session/view/project_view.h>
#include <editor/session/view/view_events.h>
#include <editor/session/view/view_revisions.h>
#include <editor/session/view/workspace_view.h>

namespace opennova::editor {

// Everything the editor's windows draw (ADR 0046 d11): the records in. The session owns
// one and rewrites it as the project changes; the windows read it by const reference
// every frame and never reach into the session. Since S13 V4 it is an aggregate of sub-views,
// one per part of what it shows (session/view/): the project, the documents, the findings, the
// activity (the operation, the build, Play, Output), the dialogs, the workspace (what the windows
// show of their own: a card, a panel, a form's fields; the MCP gaps lane) and the navigation history's
// places (Back's and Forward's), each holding what is heavy
// behind a shared pointer to a type it only declares, so this header pulls no header of the
// runtime, the graph, the build or the import machinery; and the events, the one-shot asks a
// request makes of a window (view_events.h). `revisions` counts the changes of each concern
// (view_revisions.h: each concern is one sub-view's), so a window keeps what it derives until a
// concern it reads moves.
struct SessionView {
	ViewRevisions revisions;
	ProjectView project;
	DocumentsView documents;
	FindingsView findings;
	ActivityView activity;
	DialogsView dialogs;
	WorkspaceView workspace;
	NavigationView navigation;
	NovaWorldView novaworld;
	ViewEvents events;

	// Whether the session's busy gate takes a request of `kind` now: not busy_refuses over the
	// running operation (request_kinds.h). Every window enables a menu item or a button that
	// raises a request of `kind` by it, so a control is enabled exactly when the session would
	// take its request.
	bool allows(EditorRequestKind kind) const;
};

// Why Help > Update OpenNova Editor... (update_editor, ADR 0072) cannot run now, "" when it can: the
// update quits the editor for update.exe to replace its files, so nothing may be lost or left behind:
// a document with unsaved changes, a game Play started, or an operation a Quit would wait on (the busy
// gate refuses Quit). The shell refuses the rest, which only it can tell: a source run, a folder
// without update.exe or OPENNOVA.WIZ.
std::string update_refusal(const SessionView &view);

// The editor's versions on one line (base/version/version_line.h), which Help names and a menu
// tried in Try mode shows on its STARTUP screen (D-MNU-76): the open project's target title by its
// name, ONE's build (one_build()) and the protocol the title speaks. A project has no release, so
// no game version is named.
std::string editor_version_line(const SessionView &view);

// The editor's OS window title, which the shell applies when it changes: "OpenNova Editor",
// the open project's name before it ("Armory - OpenNova Editor"), and a bullet (U+25CF)
// after the name while a file has unsaved changes.
std::string editor_window_title(const SessionView &view);

} // namespace opennova::editor
