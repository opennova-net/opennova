#pragma once

#include <string>

#include <base/io/json.h>

namespace opennova::editor {

class MnuDocument;
class ProjectSession;
struct SessionView;

// What the editor MCP's editor_menu reads of one menu and how it edits it (ADR 0046 S9m):
// its screens and windows in one answer, with where the render check placed each window,
// the Problems rows that concern it, and a batch on it. Every op finds the menu one way
// (menu_for's), so an identity a pathless tree answered names a record of the menu a
// pathless edit changes. Portable, so the ctest and the MCP read one schema.

// The menu `path` names (a project-relative path or a logical name; "" is the previewed
// menu, else the active document when it is a menu): the open document, else the file as
// the last validation read it (the render check's copy). Null when the project has no such
// menu, or its file does not load.
const MnuDocument *menu_for(const SessionView &view, const std::string &path);

// editor_menu's edit and list: record_batch_request (session/record_batch.h) on the menu
// `path` names, found as menu_for finds it (opened first when it is not open). Refused
// ({ok: false, error}, nothing asked of the session) when it names no menu of the project:
// with no path, when no menu is previewed and the active document is not one.
io::JsonValue menu_edit_request(ProjectSession &session, const std::string &path, const io::JsonValue &request);

// {path, open, dirty, revision, screens: [{id, name, index, status (the render check's
// menu_preview_status_token: ready, unserializable, screen_missing; "none" when it has no render
// of the screen), current (the render shows the menu as it is now), window_count,
// windows: [{id, name, type, parent (the window holding it, 0 for a root), depth,
// index (the frame compiler's pre-order widget index), text (a STRING's value when
// written), lists {kind token: count} (the non-empty lists it holds but its child
// windows), rect [left, top, right, bottom] and local (in its parent) in 800x600 design
// units and shown, when the render is current}]}]}. The windows are the screen's roots and
// the windows they hold, pre-order; the windows a part holds are not (the compiler numbers
// none of them). Null when menu_for finds no menu.
io::JsonValue menu_tree_to_json(const SessionView &view, const std::string &path);

// The Problems rows of one menu (graph and render notes included): {path, count, counts
// {error, warning, info}, sources {graph, menu, render, document, ...: the rows by where
// they come from}, screens: [{id, name, status, current, notes (every compiler note the
// render check's render of it made, those only the preview shows included), problems (the
// rows on the screen or a record of it)}], problems: [the
// rows as editor_problems gives them, each with its `source`]}. A row's source is its
// code's family: graph (reference.*, graph.*: the asset graph), render (menu.render.*: the
// render check's compiler notes), menu (the menu validator and the reader), document, and
// so on. Null when menu_for finds no menu.
io::JsonValue menu_findings_to_json(const SessionView &view, const std::string &path);

} // namespace opennova::editor
