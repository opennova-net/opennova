#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <editor/model/edit.h>
#include <editor/model/value.h>
#include <formats/mnu/mnu.h>
#include <formats/mnu/mnu_layout.h>
#include <runtime/menu/menu_frame.h>

namespace opennova::editor {

class Document;
class MnuDocument;

// Moving and resizing a menu window by its drawn handles (ADR 0046 S9k1): what a drag
// in the preview writes to the window's POSITION. The rect a drag aims at is the window's
// rect as the runtime solved it, moved or resized by the drag, the moved edges snapped on
// the absolute grid; the POSITION written is the fewest edges that make the runtime's own
// three-stage solve (MenuFrameCompiler::solve_local_rect) land on that rect. An editor
// authoring aid, not a port: the rect math is the runtime's.

// What the drag holds: the window itself, one of its edges, or one of its corners.
enum class LayoutHandle : uint8_t { Move, Left, Right, Top, Bottom, TopLeft, TopRight, BottomLeft, BottomRight };
// "move", "left", "top_right", ...: the token the MCP and the tests name a handle by.
const char *layout_handle_token(LayoutHandle handle);
bool layout_handle_from_token(const std::string &token, LayoutHandle &out);

// The window where the drag began: its authored POSITION (each edge with whether the file
// writes it), its solved rect in its parent, and the parent's absolute origin (the grid is
// the screen's, so a child snaps where it shows).
struct LayoutStart {
	mnu::Position authored;
	mnu::RectEdges local;
	int parent_x = 0;
	int parent_y = 0;
};
// The drag: the handle and how far, in design units, from where it began; `grid` snaps
// the moved edges on the screen's grid of that many units (0 or 1: no snap).
struct LayoutDrag {
	LayoutHandle handle = LayoutHandle::Move;
	int dx = 0;
	int dy = 0;
	int grid = 8;
};
// A window's rect in its parent under a candidate POSITION: the runtime's solve.
using LayoutSolve = std::function<mnu::RectEdges(const mnu::Position &)>;

// The design grid a drag snaps to unless Alt is held.
inline constexpr int kLayoutGrid = 8;

// The rect the drag aims at: start.local moved (Move) or with the dragged edges moved,
// each moved edge snapped on the absolute grid (a move snaps its leading edges and keeps
// its size), a resized axis kept at least 1 unit across.
mnu::RectEdges layout_drag_target(const LayoutStart &start, const LayoutDrag &drag);

// The POSITION the drag writes: from the authored one, a move shifts each edge the file
// writes on the moved axis and writes a leading edge it leaves out at its shifted 0 (an
// absent leading edge reads 0); a resize writes the dragged edges. Where the solve does not
// land on the target, both edges of that axis are written at it, and if that still misses,
// all four. An edge the file writes is never left out. False when the target has no area.
bool plan_layout_drag(const LayoutStart &start, const LayoutDrag &drag, const LayoutSolve &solve, mnu::Position &out);

// The window's POSITION as the document holds it: the four edges and whether each is
// written. False when the record is not a window of the document.
bool authored_position(const Document &document, const NodeAddress &window, mnu::Position &out);

// The edits that bring `window` from `from` (the document's POSITION now) to `to`, each
// carrying `gesture`: an edge written again (Write) and set, an edge set, and an edge `to`
// leaves out that `from` writes (one an earlier step of the same drag wrote) left out
// again with its value restored. None when nothing differs.
std::vector<Edit> layout_edits(const NodeAddress &window, const mnu::Position &from, const mnu::Position &to,
                               uint64_t gesture);

// Where a drag of `window` (a window of the screen the compiler compiled from the
// document's current revision) begins: `index` its pre-order index, the rects from the
// compiler. False for a record that is not one of the compiled windows.
bool layout_start(const MnuDocument &document, const NodeAddress &window, const menu::MenuFrameCompiler &compiler,
                  const menu::MenuFrameState &state, LayoutStart &out, int *index);

// One step of a drag: the edits that bring the window (index `index` of the compiled
// screen) from the document's POSITION now to where `drag` from `start` puts it, each
// carrying `gesture`. False when the target has no area (nothing to write); true with no
// edits when the window is already there.
bool layout_drag_edits(const Document &document, const NodeAddress &window, int index,
                       const menu::MenuFrameCompiler &compiler, const LayoutStart &start, const LayoutDrag &drag,
                       uint64_t gesture, std::vector<Edit> &out);

// --- several windows (ADR 0046 S9k2) -------------------------------------------------------

// The window a record is, or the nearest window holding it, on the screen row `screen`
// (none for the screen itself, a record of another screen, or a record no window holds).
NodeAddress window_holding(const MnuDocument &document, const NodeAddress &record, NodeId screen);
// The windows of the screen `screen` that `records` are or sit in, each once, in the
// records' order (a selection's windows).
std::vector<NodeAddress> windows_holding(const MnuDocument &document, const std::vector<NodeAddress> &records,
                                         NodeId screen);

// A window of a compiled screen and where a move of it began.
struct LayoutMember {
	NodeAddress window;
	int index = -1; // its pre-order index in the compiled screen
	LayoutStart start;
};
// A move of several windows: the window the drag holds (its snap decides the step), and
// the windows that move (those no other of them holds; the held one, or the one holding it,
// among them).
struct LayoutGroup {
	LayoutMember lead;
	std::vector<LayoutMember> members;
};
// Where a move of `windows` (windows of the screen the compiler compiled from the
// document's current revision) held by `lead` begins. False when the lead or a window is
// not one of the compiled windows.
bool layout_group_start(const MnuDocument &document, const NodeAddress &lead, const std::vector<NodeAddress> &windows,
                        const menu::MenuFrameCompiler &compiler, const menu::MenuFrameState &state, LayoutGroup &out);
// One step of a move of the group: the lead's target as layout_drag_target puts it
// (snapped on the grid), and every member moved as far, unsnapped, so the windows keep
// their places relative to one another; each member written as layout_drag_edits writes
// it, every edit carrying `gesture`. The drag's handle is Move. False when the lead's
// target has no area (a member with no area stays where it is).
bool layout_group_drag_edits(const Document &document, const LayoutGroup &group, const menu::MenuFrameCompiler &compiler,
                             const LayoutDrag &drag, uint64_t gesture, std::vector<Edit> &out);

// --- a drag in the preview (the Preview window's menu pane and the editor MCP) --------------

// The selection's windows on the screen row `screen` (windows_holding over `selected`), with
// the window holding the primary record `primary` among them: what a drag of a selected
// window moves and what the arrows nudge.
std::vector<NodeAddress> selected_windows(const MnuDocument &document, const NodeAddress &primary,
                                          const std::vector<NodeAddress> &selected, NodeId screen);

// A drag of a window by one of its handles, where it began: a move of a window among
// `selected` (selected_windows) takes every one of them, a move of any other window takes it
// alone, and a resize (any other handle) holds the window alone.
struct LayoutPress {
	LayoutHandle handle = LayoutHandle::Move;
	std::vector<NodeAddress> windows; // the windows the drag takes
	LayoutGroup group;                // where they began (a resize: its lead alone)
};
// Where the drag of `window` (a window of the screen the compiler compiled from the
// document's current revision) by `handle` begins. False when a window it takes is not one
// of the compiled windows.
bool layout_press(const MnuDocument &document, const NodeAddress &window, LayoutHandle handle,
                  const std::vector<NodeAddress> &selected, const menu::MenuFrameCompiler &compiler,
                  const menu::MenuFrameState &state, LayoutPress &out);
// One step of the drag the press began, `dx` and `dy` design units from where it began,
// snapped on a grid of `grid` units (0: free): the resize's edits (layout_drag_edits) or the
// move's (layout_group_drag_edits), each carrying `gesture`. False when the target has no
// area.
bool layout_press_edits(const Document &document, const LayoutPress &press, const menu::MenuFrameCompiler &compiler,
                        int dx, int dy, int grid, uint64_t gesture, std::vector<Edit> &out);

} // namespace opennova::editor
