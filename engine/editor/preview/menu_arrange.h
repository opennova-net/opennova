#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <editor/model/edit.h>
#include <editor/model/value.h>
#include <editor/preview/menu_layout_edit.h>
#include <formats/mnu/mnu.h>
#include <runtime/menu/menu_frame.h>

namespace opennova::editor {

class MnuDocument;

// Arranging several menu windows at once (ADR 0046 S9k2, the Arrange menu ONED's menu
// canvas had): their edges or centres aligned to the primary window's, the windows spread
// evenly, or their drawing order changed among their siblings. An editor authoring aid,
// not a port. The rects are the runtime's own (the compiled screen's absolute rects); a
// window that moves is written through the layout planner's rules (menu_layout_edit: the
// fewest POSITION edges that make the runtime's solve land it, an edge the file writes
// never left out); an order change is Moves among the window's siblings, since the
// runtime draws a window's children in the file's order, a later one over an earlier one,
// and its hit test takes the last one drawn (MenuFrameCompiler's draw and hit walks;
// docs/mnu/menu-re.md, the 2026-08-09 draw-walk grill). Each arrange is one batch, one
// undo step.

enum class ArrangeOp : uint8_t {
	AlignLeft,
	AlignRight,
	AlignTop,
	AlignBottom,
	AlignHorizontalCenters, // the centres on one vertical line
	AlignVerticalCenters,   // the centres on one horizontal line
	DistributeHorizontally, // equal gaps between them, left to right
	DistributeVertically,   // equal gaps between them, top to bottom
	BringToFront,           // last among its siblings: drawn over every one
	BringForward,           // past the next sibling
	SendBackward,           // before the previous sibling
	SendToBack,             // first among its siblings: drawn under every one
};
inline constexpr ArrangeOp kArrangeOps[] = {
        ArrangeOp::AlignLeft,           ArrangeOp::AlignRight,           ArrangeOp::AlignTop,
        ArrangeOp::AlignBottom,         ArrangeOp::AlignHorizontalCenters, ArrangeOp::AlignVerticalCenters,
        ArrangeOp::DistributeHorizontally, ArrangeOp::DistributeVertically, ArrangeOp::BringToFront,
        ArrangeOp::BringForward,        ArrangeOp::SendBackward,         ArrangeOp::SendToBack,
};
// "align_left", "distribute_horizontally", "bring_to_front", ...: the token the MCP and
// the tests name an op by.
const char *arrange_op_token(ArrangeOp op);
bool arrange_op_from_token(const std::string &token, ArrangeOp &out);
// "Align left edges", ...: the name the editor shows.
const char *arrange_op_label(ArrangeOp op);
// How many windows an op needs: two to align, three to distribute, one to reorder.
size_t arrange_minimum(ArrangeOp op);
// True for the ops that move windows (align, distribute); false for the order changes.
bool arrange_moves(ArrangeOp op);

// --- the plan over rects -----------------------------------------------------------------

// One window taking part: where it is, how the runtime solves it, and which window of the
// list holds it (the nearest; -1 when none does).
struct ArrangeWindow {
	LayoutStart start; // its POSITION, its rect in its parent, the parent's absolute origin
	LayoutSolve solve; // its rect in its parent under a candidate POSITION (the runtime's)
	int holder = -1;
};
// Align or distribute: the POSITION each window is written with (its own when it stays).
// Aligning moves every window to the primary's edge or centre on the screen; distributing
// keeps the first and the last (by their left or top edges) and spaces the rest so every
// gap between neighbours is the same. Every window lands where the op puts it on the
// screen: one another window of the list holds is written relative to where that one
// lands. False (nothing planned) for an order change, a list too short, a primary out of
// range, or a window the op must move that has no area (the layout planner writes no move
// of it, and the windows it holds would land off by its move): `stuck` names that one.
bool plan_arrange(ArrangeOp op, const std::vector<ArrangeWindow> &windows, size_t primary,
                  std::vector<mnu::Position> &out, size_t *stuck = nullptr);
// The order of `siblings` after an order change of the `chosen` ones among them (to the
// front or back, keeping their own order; forward or backward past the one next to them).
std::vector<NodeId> arrange_order(ArrangeOp op, const std::vector<NodeId> &siblings, const std::vector<NodeId> &chosen);
// The Moves that turn the order `from` into `to` (the same records), applied one after
// another: each record and the index it lands at. The longest run already in order stays,
// so bringing one window to the front is one Move.
std::vector<std::pair<NodeId, size_t>> reorder_moves(std::vector<NodeId> from, const std::vector<NodeId> &to);

// --- against a menu document and its compiled screen -------------------------------------

// The edits that arrange `windows` (windows of the screen the compiler compiled from the
// document's current revision; `primary` among them, the one the others align to) by
// `op`: one batch on the screen's row, one undo step. False with `why` when it cannot (too
// few windows, a window that is not one of the compiled windows, a window to move that has
// no area on the screen); true with no edits when nothing would change.
bool arrange_edits(const MnuDocument &document, const std::vector<NodeAddress> &windows, const NodeAddress &primary,
                   ArrangeOp op, const menu::MenuFrameCompiler &compiler, const menu::MenuFrameState &state,
                   std::vector<Edit> &out, std::string *why);

} // namespace opennova::editor
