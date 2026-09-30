#pragma once

#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/preview/menu_arrange.h>
#include <editor/preview/menu_layout_edit.h>
#include <editor/preview/menu_preview_state.h>
#include <runtime/menu/menu_frame.h>

namespace opennova::editor {

class MenuScreenRender;
class MnuDocument;
class ProjectSession;
struct Node;
struct SessionView;

// What the menu preview shows, for the MCP and the tests (ADR 0046 S9j): the status and
// the configured screen's widgets as the runtime's own compiler placed them; and what the
// MCP asks of it (its options, a drag, an arrange).
struct MenuPreviewSnapshot {
	MenuPreviewStatus status = MenuPreviewStatus::NoProject;
	std::string detail;
	const MnuDocument *document = nullptr; // the previewed menu (current)
	const Node *screen = nullptr;          // its previewed screen row
	const menu::MenuFrameCompiler *compiler = nullptr; // Ready only
	const menu::MenuFrameState *state = nullptr;
	MenuPreviewOptions options;
	std::vector<std::string> missing;    // files the project lacks
	std::vector<std::string> unreadable; // files it has that did not load
	uint64_t shown_revision = 0;
};

// The snapshot of a model and its device's compiler (null: no device attached).
MenuPreviewSnapshot menu_preview_snapshot(const SessionView &view, const MenuPreviewModel &model,
                                          const menu::MenuFrameCompiler *compiler, const menu::MenuFrameState *state);

// The snapshot of a screen the render check compiled headless (MenuRenderCheck::render),
// in the preview's schema: the parity read of the preview (same widgets, same notes).
MenuPreviewSnapshot render_snapshot(const MenuScreenRender &render, const MnuDocument &document, const Node &screen);

// {status, message, detail, path, screen {id, name}, revision, shown_revision, current,
//  device {width, height}, options {show_hidden, force_id, force_state ("normal",
//  "mouseover", "selected", "disabled"), checked, popup_open, focus}, missing[] (the
//  files the project lacks), unreadable[] (the files it has that did not load),
//  widgets[{index, id, name, type, shown, disabled, rect [l,t,r,b] (absolute design units),
//  local [l,t,r,b] (in its parent), text, font, text_color}], notes[] (menu_notes_to_json:
//  what configure noted, then what the frame state lays out to)}. The widgets and the notes
// only when ready and current (the device shows the document's revision).
io::JsonValue menu_preview_to_json(const MenuPreviewSnapshot &snapshot);
// The compiler's notes on a screen: [{index (the window's pre-order index, -1 the
// screen), window_id, id (the record the note sits on: the window, or the record of its
// list the note names), name (the window's), code (the token), finding
// ("menu.render.<code>"), basis (witnessed, port_policy, deferred), severity (warning or
// info as a Problems row, "preview" when only the preview shows it), subject, list,
// record, field, message}].
io::JsonValue menu_notes_to_json(const MnuDocument &document, const Node &screen,
                                 const menu::MenuFrameCompiler &compiler,
                                 const std::vector<menu::MenuFrameNote> &notes);
// The front-most shown widget under a design-space point, as the game's pump would claim
// it (MenuFrameCompiler::hit_widget): {index, id, name}, index -1 for none.
io::JsonValue menu_preview_hit_to_json(const MenuPreviewSnapshot &snapshot, float x, float y);

// --- what the editor MCP's editor_menu_preview asks of the preview -------------------------

// The options an {width, height (1..8192), show_hidden, force_id (a window record, 0 none),
// force_state (menu_preview_state_from_token), checked, popup_open, focus} object sets over
// `options`, each member optional, a number's fraction dropped. False, `options` untouched,
// for another member or a value out of range or of another type.
bool menu_preview_options_from_json(const io::JsonValue &json, MenuPreviewOptions &options);

// One drag of the window `window` of the screen the snapshot shows, as the Preview window's
// pane plans one (selected_windows, layout_press, layout_press_edits): `dx` and `dy` design
// units from where it is by `handle`, snapped on kLayoutGrid when `snap`, sent to the session
// as one batch and one undo step. False when the snapshot does not show the document as it is
// now, the window is not one of its screen's, the drag writes nothing (no area), or the session
// did not take the batch (its outcome not done).
bool menu_preview_drag(ProjectSession &session, const MenuPreviewSnapshot &snapshot, NodeId window, LayoutHandle handle,
                       int dx, int dy, bool snap);
// One arrange of `windows` (windows of the screen the snapshot shows, the first the one the
// others align to; arrange_edits) sent as one batch. False as menu_preview_drag is, and when
// arrange_edits refuses.
bool menu_preview_arrange(ProjectSession &session, const MenuPreviewSnapshot &snapshot,
                          const std::vector<NodeId> &windows, ArrangeOp op);

} // namespace opennova::editor
