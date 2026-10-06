#pragma once

#include <string>

#include <editor/session/editor_request.h>
#include <editor/session/view/activity_view.h>
#include <editor/session/view/dialogs_view.h>
#include <editor/session/view/documents_view.h>
#include <editor/session/view/findings_view.h>
#include <editor/session/view/navigation_view.h>
#include <editor/session/view/project_view.h>
#include <editor/session/view/view_events.h>
#include <editor/session/view/view_revisions.h>
#include <editor/session/view/workspace_view.h>

namespace opennova::editor {

// Everything the editor's windows draw (ADR 0046 d10): the records in. The session owns
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
	ViewEvents events;

	// Whether the session's busy gate takes a request of `kind` now: not busy_refuses over the
	// running operation (request_kinds.h). Every window enables a menu item or a button that
	// raises a request of `kind` by it, so a control is enabled exactly when the session would
	// take its request.
	bool allows(EditorRequestKind kind) const;
};

// The editor's OS window title, which the shell applies when it changes: "OpenNova Editor",
// the open project's name before it ("Armory - OpenNova Editor"), and a bullet (U+25CF)
// after the name while a file has unsaved changes.
std::string editor_window_title(const SessionView &view);

} // namespace opennova::editor
