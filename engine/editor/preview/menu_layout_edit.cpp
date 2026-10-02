#include <editor/preview/menu_layout_edit.h>

#include <algorithm>
#include <variant>

#include <editor/documents/mnu_document.h>
#include <editor/model/document.h>

namespace opennova::editor {

namespace {

struct HandleToken {
	LayoutHandle handle;
	const char *token;
};
constexpr HandleToken kHandleTokens[] = {
        {LayoutHandle::Move, "move"},           {LayoutHandle::Left, "left"},
        {LayoutHandle::Right, "right"},         {LayoutHandle::Top, "top"},
        {LayoutHandle::Bottom, "bottom"},       {LayoutHandle::TopLeft, "top_left"},
        {LayoutHandle::TopRight, "top_right"},  {LayoutHandle::BottomLeft, "bottom_left"},
        {LayoutHandle::BottomRight, "bottom_right"},
};

bool moves_left(LayoutHandle h) {
	return h == LayoutHandle::Left || h == LayoutHandle::TopLeft || h == LayoutHandle::BottomLeft;
}
bool moves_right(LayoutHandle h) {
	return h == LayoutHandle::Right || h == LayoutHandle::TopRight || h == LayoutHandle::BottomRight;
}
bool moves_top(LayoutHandle h) {
	return h == LayoutHandle::Top || h == LayoutHandle::TopLeft || h == LayoutHandle::TopRight;
}
bool moves_bottom(LayoutHandle h) {
	return h == LayoutHandle::Bottom || h == LayoutHandle::BottomLeft || h == LayoutHandle::BottomRight;
}

// Floor division (the grid below a negative coordinate is the next one down).
int floor_div(int value, int divisor) {
	const int quotient = value / divisor;
	return (value % divisor != 0 && ((value < 0) != (divisor < 0))) ? quotient - 1 : quotient;
}

// A parent-relative edge snapped to the nearest line of the screen's grid.
int snap(int local, int parent, int grid) {
	if (grid <= 1) return local;
	return floor_div(parent + local + grid / 2, grid) * grid - parent;
}

// One axis of a POSITION: its leading and trailing edges.
struct Axis {
	int *lead;
	bool *has_lead;
	int *trail;
	bool *has_trail;
};
Axis x_axis(mnu::Position &p) { return {&p.left, &p.has_left, &p.right, &p.has_right}; }
Axis y_axis(mnu::Position &p) { return {&p.top, &p.has_top, &p.bottom, &p.has_bottom}; }

// A move by `delta`: every edge the file writes shifts; a leading edge it leaves out reads
// 0, so it is written at 0 + delta; a trailing edge it leaves out stays out (the width
// still comes from the art or the text).
void shift_axis(const Axis &axis, int delta) {
	if (delta == 0) return;
	*axis.lead = (*axis.has_lead ? *axis.lead : 0) + delta;
	*axis.has_lead = true;
	if (*axis.has_trail) *axis.trail += delta;
}

void write_axis(const Axis &axis, int lead, int trail) {
	*axis.lead = lead;
	*axis.has_lead = true;
	*axis.trail = trail;
	*axis.has_trail = true;
}

struct EdgeField {
	const char *field;
	int mnu::Position::*value;
	bool mnu::Position::*has;
};
constexpr EdgeField kEdges[] = {
        {"position.left", &mnu::Position::left, &mnu::Position::has_left},
        {"position.top", &mnu::Position::top, &mnu::Position::has_top},
        {"position.right", &mnu::Position::right, &mnu::Position::has_right},
        {"position.bottom", &mnu::Position::bottom, &mnu::Position::has_bottom},
};

Edit edge_edit(EditOperation operation, const NodeAddress &window, const char *field, int value, uint64_t gesture) {
	Edit edit;
	edit.operation = operation;
	edit.address = window;
	edit.field = field;
	edit.value = int64_t(value);
	edit.gesture = gesture;
	return edit;
}

} // namespace

const char *layout_handle_token(LayoutHandle handle) {
	for (const HandleToken &entry : kHandleTokens)
		if (entry.handle == handle) return entry.token;
	return "move";
}

bool layout_handle_from_token(const std::string &token, LayoutHandle &out) {
	for (const HandleToken &entry : kHandleTokens)
		if (token == entry.token) {
			out = entry.handle;
			return true;
		}
	return false;
}

mnu::RectEdges layout_drag_target(const LayoutStart &start, const LayoutDrag &drag) {
	mnu::RectEdges target = start.local;
	if (drag.handle == LayoutHandle::Move) {
		const int left = snap(start.local.left + drag.dx, start.parent_x, drag.grid);
		const int top = snap(start.local.top + drag.dy, start.parent_y, drag.grid);
		target.left = left;
		target.right = start.local.right + (left - start.local.left);
		target.top = top;
		target.bottom = start.local.bottom + (top - start.local.top);
		return target;
	}
	if (moves_left(drag.handle))
		target.left = std::min(snap(start.local.left + drag.dx, start.parent_x, drag.grid), target.right - 1);
	if (moves_right(drag.handle))
		target.right = std::max(snap(start.local.right + drag.dx, start.parent_x, drag.grid), target.left + 1);
	if (moves_top(drag.handle))
		target.top = std::min(snap(start.local.top + drag.dy, start.parent_y, drag.grid), target.bottom - 1);
	if (moves_bottom(drag.handle))
		target.bottom = std::max(snap(start.local.bottom + drag.dy, start.parent_y, drag.grid), target.top + 1);
	return target;
}

bool plan_layout_drag(const LayoutStart &start, const LayoutDrag &drag, const LayoutSolve &solve, mnu::Position &out) {
	const mnu::RectEdges target = layout_drag_target(start, drag);
	if (target.right <= target.left || target.bottom <= target.top) return false;
	mnu::Position candidate = start.authored;
	const bool resize = drag.handle != LayoutHandle::Move;
	bool moved_x, moved_y;
	if (!resize) {
		moved_x = target.left != start.local.left;
		moved_y = target.top != start.local.top;
		shift_axis(x_axis(candidate), target.left - start.local.left);
		shift_axis(y_axis(candidate), target.top - start.local.top);
	} else {
		moved_x = moves_left(drag.handle) || moves_right(drag.handle);
		moved_y = moves_top(drag.handle) || moves_bottom(drag.handle);
		if (moves_left(drag.handle)) { candidate.left = target.left; candidate.has_left = true; }
		if (moves_right(drag.handle)) { candidate.right = target.right; candidate.has_right = true; }
		if (moves_top(drag.handle)) { candidate.top = target.top; candidate.has_top = true; }
		if (moves_bottom(drag.handle)) { candidate.bottom = target.bottom; candidate.has_bottom = true; }
	}
	// The runtime's solve says where the candidate lands; an axis it misses is written
	// whole (both edges at the target: no art, text or 0 left to size it). The axes solve
	// apart, so this lands; four written edges are the last resort.
	mnu::RectEdges solved = solve(candidate);
	if (moved_x && (solved.left != target.left || solved.right != target.right))
		write_axis(x_axis(candidate), target.left, target.right);
	if (moved_y && (solved.top != target.top || solved.bottom != target.bottom))
		write_axis(y_axis(candidate), target.top, target.bottom);
	solved = solve(candidate);
	if (solved.left != target.left || solved.right != target.right || solved.top != target.top ||
	    solved.bottom != target.bottom) {
		write_axis(x_axis(candidate), target.left, target.right);
		write_axis(y_axis(candidate), target.top, target.bottom);
	}
	out = candidate;
	return true;
}

bool authored_position(const Document &document, const NodeAddress &window, mnu::Position &out) {
	out = mnu::Position();
	for (const EdgeField &edge : kEdges) {
		Value value;
		if (!document.get(window, edge.field, value)) return false;
		const int64_t *number = std::get_if<int64_t>(&value);
		if (!number) return false;
		out.*edge.value = int(*number);
		out.*edge.has = document.present(window, edge.field);
	}
	return true;
}

std::vector<Edit> layout_edits(const NodeAddress &window, const mnu::Position &from, const mnu::Position &to,
                               uint64_t gesture) {
	std::vector<Edit> edits;
	for (const EdgeField &edge : kEdges) {
		const int was = from.*edge.value, now = to.*edge.value;
		const bool written = from.*edge.has, writes = to.*edge.has;
		if (writes) {
			if (!written) edits.push_back(edge_edit(EditOperation::Write, window, edge.field, was, gesture));
			if (now != was) edits.push_back(edge_edit(EditOperation::Set, window, edge.field, now, gesture));
		} else if (written) {
			// An edge an earlier step of this drag wrote: its value as it was, then left out.
			if (now != was) edits.push_back(edge_edit(EditOperation::Set, window, edge.field, now, gesture));
			edits.push_back(edge_edit(EditOperation::Clear, window, edge.field, now, gesture));
		}
	}
	return edits;
}

bool layout_start(const MnuDocument &document, const NodeAddress &window, const menu::MenuFrameCompiler &compiler,
                  const menu::MenuFrameState &state, LayoutStart &out, int *index) {
	const int at = document.window_index(window);
	if (at < 0 || at >= compiler.widget_count()) return false;
	mnu::RectEdges rect{};
	if (!authored_position(document, window, out.authored) || !compiler.widget_rect(at, state, &rect)) return false;
	mnu::RectEdges parent{};
	out.parent_x = out.parent_y = 0;
	if (compiler.widget_rect(compiler.widget_parent(at), state, &parent)) {
		out.parent_x = parent.left;
		out.parent_y = parent.top;
	}
	out.local = {rect.left - out.parent_x, rect.top - out.parent_y, rect.right - out.parent_x, rect.bottom - out.parent_y};
	if (index) *index = at;
	return true;
}

bool layout_drag_edits(const Document &document, const NodeAddress &window, int index,
                       const menu::MenuFrameCompiler &compiler, const LayoutStart &start, const LayoutDrag &drag,
                       uint64_t gesture, std::vector<Edit> &out) {
	out.clear();
	if (index < 0 || index >= compiler.widget_count()) return false;
	const LayoutSolve solve = [&](const mnu::Position &candidate) {
		mnu::RectEdges rect{};
		compiler.solve_local_rect(index, candidate, &rect);
		return rect;
	};
	mnu::Position to, now;
	if (!plan_layout_drag(start, drag, solve, to) || !authored_position(document, window, now)) return false;
	out = layout_edits(window, now, to, gesture);
	return true;
}

NodeAddress window_holding(const MnuDocument &document, const NodeAddress &record, NodeId screen) {
	constexpr NodeKind window = node_kind(MenuKind::Window);
	if (!screen || record.row != screen || !record.child) return NodeAddress();
	NodeAddress address = record;
	if (address.kind != window)
		for (const NodeAddress &owner : document.ancestors(record))
			if (owner.kind == window && owner.child) address = owner;
	return address.kind == window ? address : NodeAddress();
}

std::vector<NodeAddress> windows_holding(const MnuDocument &document, const std::vector<NodeAddress> &records,
                                         NodeId screen) {
	std::vector<NodeAddress> out;
	for (const NodeAddress &record : records) {
		const NodeAddress window = window_holding(document, record, screen);
		if (window.child && std::find(out.begin(), out.end(), window) == out.end()) out.push_back(window);
	}
	return out;
}

bool layout_group_start(const MnuDocument &document, const NodeAddress &lead, const std::vector<NodeAddress> &windows,
                        const menu::MenuFrameCompiler &compiler, const menu::MenuFrameState &state, LayoutGroup &out) {
	out = LayoutGroup();
	out.lead.window = lead;
	if (!layout_start(document, lead, compiler, state, out.lead.start, &out.lead.index)) return false;
	for (const NodeAddress &window : document.outermost(windows)) {
		LayoutMember member;
		member.window = window;
		if (!layout_start(document, window, compiler, state, member.start, &member.index)) return false;
		out.members.push_back(member);
	}
	return !out.members.empty();
}

bool layout_group_drag_edits(const Document &document, const LayoutGroup &group, const menu::MenuFrameCompiler &compiler,
                             const LayoutDrag &drag, uint64_t gesture, std::vector<Edit> &out) {
	out.clear();
	if (drag.handle != LayoutHandle::Move) return false;
	const mnu::RectEdges target = layout_drag_target(group.lead.start, drag);
	if (target.right <= target.left || target.bottom <= target.top) return false;
	// The step every member takes: the lead's, snapped as the lead snaps.
	const LayoutDrag step{LayoutHandle::Move, target.left - group.lead.start.local.left,
	                      target.top - group.lead.start.local.top, 0};
	for (const LayoutMember &member : group.members) {
		std::vector<Edit> edits;
		if (!layout_drag_edits(document, member.window, member.index, compiler, member.start, step, gesture, edits))
			continue;
		out.insert(out.end(), edits.begin(), edits.end());
	}
	return true;
}

std::vector<NodeAddress> selected_windows(const MnuDocument &document, const NodeAddress &primary,
                                          const std::vector<NodeAddress> &selected, NodeId screen) {
	std::vector<NodeAddress> out = windows_holding(document, selected, screen);
	const NodeAddress held = window_holding(document, primary, screen);
	if (held.child && std::find(out.begin(), out.end(), held) == out.end()) out.push_back(held);
	return out;
}

bool layout_press(const MnuDocument &document, const NodeAddress &window, LayoutHandle handle,
                  const std::vector<NodeAddress> &selected, const menu::MenuFrameCompiler &compiler,
                  const menu::MenuFrameState &state, LayoutPress &out) {
	out = LayoutPress();
	out.handle = handle;
	if (handle != LayoutHandle::Move) {
		out.windows = {window};
		out.group.lead.window = window;
		return layout_start(document, window, compiler, state, out.group.lead.start, &out.group.lead.index);
	}
	const bool among = std::find(selected.begin(), selected.end(), window) != selected.end();
	out.windows = among ? selected : std::vector<NodeAddress>{window};
	return layout_group_start(document, window, out.windows, compiler, state, out.group);
}

bool layout_press_edits(const Document &document, const LayoutPress &press, const menu::MenuFrameCompiler &compiler,
                        int dx, int dy, int grid, uint64_t gesture, std::vector<Edit> &out) {
	const LayoutDrag drag{press.handle, dx, dy, grid};
	const LayoutMember &lead = press.group.lead;
	if (press.handle != LayoutHandle::Move)
		return layout_drag_edits(document, lead.window, lead.index, compiler, lead.start, drag, gesture, out);
	return layout_group_drag_edits(document, press.group, compiler, drag, gesture, out);
}

} // namespace opennova::editor
