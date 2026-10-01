#pragma once

#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/model/value.h>
#include <editor/session/session_json.h>
#include <runtime/menu/menu_frame.h>

namespace opennova::editor {

class MnuDocument;
struct Node;
struct SessionView;

// A compiled screen's widgets (the menu viewport's items, the menu_render query's widgets): [{index,
// id (the window's record), name, type, shown, disabled, rect [l,t,r,b] (absolute design units),
// local [l,t,r,b] (in its parent), text, font, text_color}], in the compiled screen's pre-order.
io::JsonValue menu_widgets_to_json(const MnuDocument &document, const Node &screen,
		const menu::MenuFrameCompiler &compiler, const menu::MenuFrameState &state);
// The compiler's notes on a screen: [{index (the window's pre-order index, -1 the screen),
// window_id, id (the record the note sits on: the window, or the record of its list the note
// names), name (the window's), code (the token), finding ("menu.render.<code>"), basis (witnessed,
// port_policy, deferred), severity (warning or info as a Problems row, "preview" when only the
// viewport shows it), subject, list, record, field, message}].
io::JsonValue menu_notes_to_json(const MnuDocument &document, const Node &screen,
		const menu::MenuFrameCompiler &compiler, const std::vector<menu::MenuFrameNote> &notes);

// What the query seam reads of one menu (ADR 0046 S9m, S13 A5: the menu_tree, menu_findings and
// menu_render queries, which editor_menu's reads became): its screens and windows in one answer,
// with where the render check placed each window, the Problems rows that concern it, and a screen
// as the render check compiled it. Every read finds the menu one way (menu_for's), and a pathless
// read the active document, as a pathless request's edits do, so an identity a pathless tree
// answered names the record a pathless edit changes. Portable, so the ctest and the MCP read one
// schema.

// The menu `path` names (a project-relative path or a logical name; "" is the active
// document, when it is a menu, and never another): the open document, else the file as the
// last validation read it (the render check's copy). Null when the project has no such menu,
// the active document is none or no menu, or the file does not load.
const MnuDocument *menu_for(const SessionView &view, const std::string &path);

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
// rows as the problems query gives them, each with its `source`]}. A row's source is its finding
// code row's (session/finding_codes.h: finding_source_token): graph (the asset graph's
// reference.missing and graph.unreadable), render (the render check's menu.render.*), else its
// group's key, its code's family (menu: the menu validator and the reader; document; and so on).
// Null when menu_for finds no menu.
io::JsonValue menu_findings_to_json(const SessionView &view, const std::string &path);

// A screen (its row identity) of a menu (as menu_for finds it) as the render check compiled it
// headless with the last validation (S9j2): {status (menu_screen_status_token), message, detail,
// path, screen {id, name}, revision, shown_revision, current, missing[] (the files the project
// lacks), unreadable[] (the files it has that did not load), widgets (menu_widgets_to_json, a page:
// `widget_count`, set_page's `count`, their whole number), notes (menu_notes_to_json, by the same
// page: `note_count` theirs; `next_offset` runs to the end of the longer list)}, the widgets and the
// notes only while the render shows the menu as it is now. The open document when the menu is open
// (its current state), else the file as the check read it; status no_project without a project,
// no_screen when the check has no such screen. Null, with a project, when menu_for finds no menu.
io::JsonValue menu_render_to_json(
		const SessionView &view, const std::string &path, NodeId screen, const JsonPage &page);

} // namespace opennova::editor
