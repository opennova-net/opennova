#include <editor/preview/menu_arrange.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <editor/documents/mnu_document.h>
#include <editor/model/document.h>

namespace opennova::editor {

namespace {

struct OpRow {
	ArrangeOp op;
	const char *token;
	const char *label;
};
constexpr OpRow kOpRows[] = {
        {ArrangeOp::AlignLeft, "align_left", "Align left edges"},
        {ArrangeOp::AlignRight, "align_right", "Align right edges"},
        {ArrangeOp::AlignTop, "align_top", "Align top edges"},
        {ArrangeOp::AlignBottom, "align_bottom", "Align bottom edges"},
        {ArrangeOp::AlignHorizontalCenters, "align_horizontal_centers", "Align horizontal centers"},
        {ArrangeOp::AlignVerticalCenters, "align_vertical_centers", "Align vertical centers"},
        {ArrangeOp::DistributeHorizontally, "distribute_horizontally", "Distribute horizontally"},
        {ArrangeOp::DistributeVertically, "distribute_vertically", "Distribute vertically"},
        {ArrangeOp::BringToFront, "bring_to_front", "Bring to front"},
        {ArrangeOp::BringForward, "bring_forward", "Bring forward"},
        {ArrangeOp::SendBackward, "send_backward", "Send backward"},
        {ArrangeOp::SendToBack, "send_to_back", "Send to back"},
};

// Floor division (half of an odd difference rounds toward the lower edge).
int floor_half(int value) { return value >= 0 ? value / 2 : -((-value + 1) / 2); }

// A window's rect on the screen: its rect in its parent moved by the parent's origin.
mnu::RectEdges absolute(const LayoutStart &start) {
	return {start.local.left + start.parent_x, start.local.top + start.parent_y, start.local.right + start.parent_x,
	        start.local.bottom + start.parent_y};
}

bool contains(const std::vector<NodeId> &ids, NodeId id) { return std::find(ids.begin(), ids.end(), id) != ids.end(); }

// Distribute along one axis: `lead` and `trail` pick the axis's edges of a rect.
void distribute(const std::vector<mnu::RectEdges> &rects, int mnu::RectEdges::*lead, int mnu::RectEdges::*trail,
                std::vector<int> &moves) {
	std::vector<size_t> order(rects.size());
	for (size_t i = 0; i < order.size(); ++i) order[i] = i;
	std::stable_sort(order.begin(), order.end(),
	                 [&](size_t a, size_t b) { return rects[a].*lead < rects[b].*lead; });
	const mnu::RectEdges &first = rects[order.front()], &last = rects[order.back()];
	double sizes = 0.0;
	for (const mnu::RectEdges &rect : rects) sizes += double(rect.*trail - rect.*lead);
	const double gap = (double(last.*trail - first.*lead) - sizes) / double(rects.size() - 1);
	double cursor = double(first.*lead);
	for (const size_t i : order) {
		moves[i] = int(std::lround(cursor)) - rects[i].*lead;
		cursor += double(rects[i].*trail - rects[i].*lead) + gap;
	}
}

} // namespace

const char *arrange_op_token(ArrangeOp op) {
	for (const OpRow &row : kOpRows)
		if (row.op == op) return row.token;
	return "";
}

bool arrange_op_from_token(const std::string &token, ArrangeOp &out) {
	for (const OpRow &row : kOpRows)
		if (token == row.token) {
			out = row.op;
			return true;
		}
	return false;
}

const char *arrange_op_label(ArrangeOp op) {
	for (const OpRow &row : kOpRows)
		if (row.op == op) return row.label;
	return "";
}

size_t arrange_minimum(ArrangeOp op) {
	if (op == ArrangeOp::DistributeHorizontally || op == ArrangeOp::DistributeVertically) return 3;
	return arrange_moves(op) ? 2 : 1;
}

bool arrange_moves(ArrangeOp op) {
	return op != ArrangeOp::BringToFront && op != ArrangeOp::BringForward && op != ArrangeOp::SendBackward &&
	       op != ArrangeOp::SendToBack;
}

bool plan_arrange(ArrangeOp op, const std::vector<ArrangeWindow> &windows, size_t primary,
                  std::vector<mnu::Position> &out, size_t *stuck) {
	out.clear();
	if (stuck) *stuck = SIZE_MAX;
	const size_t count = windows.size();
	if (!arrange_moves(op) || count < arrange_minimum(op) || primary >= count) return false;
	std::vector<mnu::RectEdges> rects(count);
	for (size_t i = 0; i < count; ++i) rects[i] = absolute(windows[i].start);
	// How far each window moves on the screen.
	std::vector<int> dx(count, 0), dy(count, 0);
	const mnu::RectEdges &to = rects[primary];
	for (size_t i = 0; i < count; ++i) {
		const mnu::RectEdges &r = rects[i];
		switch (op) {
		case ArrangeOp::AlignLeft: dx[i] = to.left - r.left; break;
		case ArrangeOp::AlignRight: dx[i] = to.right - r.right; break;
		case ArrangeOp::AlignTop: dy[i] = to.top - r.top; break;
		case ArrangeOp::AlignBottom: dy[i] = to.bottom - r.bottom; break;
		case ArrangeOp::AlignHorizontalCenters: dx[i] = floor_half((to.left + to.right) - (r.left + r.right)); break;
		case ArrangeOp::AlignVerticalCenters: dy[i] = floor_half((to.top + to.bottom) - (r.top + r.bottom)); break;
		default: break;
		}
	}
	if (op == ArrangeOp::DistributeHorizontally) distribute(rects, &mnu::RectEdges::left, &mnu::RectEdges::right, dx);
	if (op == ArrangeOp::DistributeVertically) distribute(rects, &mnu::RectEdges::top, &mnu::RectEdges::bottom, dy);
	// Each window written relative to where the window holding it lands.
	out.resize(count);
	for (size_t i = 0; i < count; ++i) {
		const ArrangeWindow &window = windows[i];
		const int holder = window.holder >= 0 && size_t(window.holder) < count && size_t(window.holder) != i ? window.holder : -1;
		const int local_x = dx[i] - (holder >= 0 ? dx[size_t(holder)] : 0);
		const int local_y = dy[i] - (holder >= 0 ? dy[size_t(holder)] : 0);
		out[i] = window.start.authored;
		if (!local_x && !local_y) continue;
		// A window with no area takes no move (the planner refuses it), so neither it nor
		// what it holds would land where the op puts them: nothing is arranged.
		if (!plan_layout_drag(window.start, LayoutDrag{LayoutHandle::Move, local_x, local_y, 0}, window.solve, out[i])) {
			out.clear();
			if (stuck) *stuck = i;
			return false;
		}
	}
	return true;
}

std::vector<NodeId> arrange_order(ArrangeOp op, const std::vector<NodeId> &siblings, const std::vector<NodeId> &chosen) {
	std::vector<NodeId> order = siblings;
	switch (op) {
	case ArrangeOp::BringToFront:
	case ArrangeOp::SendToBack: {
		std::vector<NodeId> moved, rest;
		for (const NodeId id : siblings) (contains(chosen, id) ? moved : rest).push_back(id);
		order = op == ArrangeOp::BringToFront ? rest : moved;
		const std::vector<NodeId> &tail = op == ArrangeOp::BringToFront ? moved : rest;
		order.insert(order.end(), tail.begin(), tail.end());
		break;
	}
	case ArrangeOp::BringForward:
		// From the front: a chosen one trades places with the one after it unless that one
		// is chosen too, so a run of chosen ones moves forward as a block.
		for (size_t i = order.size(); i-- > 1;)
			if (contains(chosen, order[i - 1]) && !contains(chosen, order[i])) std::swap(order[i - 1], order[i]);
		break;
	case ArrangeOp::SendBackward:
		for (size_t i = 1; i < order.size(); ++i)
			if (contains(chosen, order[i]) && !contains(chosen, order[i - 1])) std::swap(order[i - 1], order[i]);
		break;
	default: break;
	}
	return order;
}

std::vector<std::pair<NodeId, size_t>> reorder_moves(std::vector<NodeId> from, const std::vector<NodeId> &to) {
	std::vector<std::pair<NodeId, size_t>> moves;
	if (from.size() != to.size()) return moves;
	// The records that stay: the longest run of `from` already in `to`'s order.
	const size_t count = from.size();
	std::vector<size_t> rank(count), length(count, 1), before(count, SIZE_MAX);
	for (size_t i = 0; i < count; ++i) {
		const auto at = std::find(to.begin(), to.end(), from[i]);
		if (at == to.end()) return moves;
		rank[i] = size_t(at - to.begin());
	}
	size_t best = 0;
	for (size_t i = 0; i < count; ++i) {
		for (size_t j = 0; j < i; ++j)
			if (rank[j] < rank[i] && length[j] + 1 > length[i]) {
				length[i] = length[j] + 1;
				before[i] = j;
			}
		if (length[i] > length[best]) best = i;
	}
	std::vector<NodeId> staying;
	for (size_t i = count ? best : SIZE_MAX; i != SIZE_MAX; i = before[i]) staying.push_back(from[i]);
	// The rest, in `to`'s order, each right after the record before it there (the first at
	// the front): a record never moves between one and the record placed after it.
	for (size_t index = 0; index < count; ++index) {
		const NodeId id = to[index];
		if (contains(staying, id)) continue;
		from.erase(std::find(from.begin(), from.end(), id));
		const size_t position =
		        index == 0 ? 0 : size_t(std::find(from.begin(), from.end(), to[index - 1]) - from.begin()) + 1;
		from.insert(from.begin() + std::ptrdiff_t(position), id);
		moves.emplace_back(id, position);
	}
	return moves;
}

bool arrange_edits(const MnuDocument &document, const std::vector<NodeAddress> &windows, const NodeAddress &primary,
                   ArrangeOp op, const menu::MenuFrameCompiler &compiler, const menu::MenuFrameState &state,
                   std::vector<Edit> &out, std::string *why) {
	out.clear();
	auto refuse = [&](const std::string &reason) {
		if (why) *why = reason;
		return false;
	};
	constexpr NodeKind window_kind = node_kind(MenuKind::Window);
	std::vector<NodeAddress> chosen;
	for (const NodeAddress &window : windows)
		if (std::find(chosen.begin(), chosen.end(), window) == chosen.end()) chosen.push_back(window);
	if (chosen.size() < arrange_minimum(op))
		return refuse(std::string(arrange_op_label(op)) + " takes " + std::to_string(arrange_minimum(op)) +
		              " windows or more.");
	for (const NodeAddress &window : chosen)
		if (window.kind != window_kind || !window.child || window.row != chosen.front().row)
			return refuse("Arrange takes windows of one screen.");

	if (!arrange_moves(op)) {
		// Each owner's windows reordered among their siblings, in one batch.
		std::vector<NodeAddress> owners;
		for (const NodeAddress &window : chosen) {
			Document::Placement at;
			if (!document.placement(window, at)) return refuse("A window to arrange is not in the menu.");
			if (std::find(owners.begin(), owners.end(), at.owner) == owners.end()) owners.push_back(at.owner);
		}
		for (const NodeAddress &owner : owners) {
			std::vector<NodeId> siblings, picked;
			for (const Document::Collection &collection : document.collections_of(owner))
				if (collection.spec.kind == window_kind) siblings = collection.ids;
			for (const NodeAddress &window : chosen)
				if (contains(siblings, window.child)) picked.push_back(window.child);
			for (const auto &move : reorder_moves(siblings, arrange_order(op, siblings, picked))) {
				Edit edit;
				edit.operation = EditOperation::Move;
				edit.address = {owner.row, window_kind, move.first};
				edit.position = move.second;
				out.push_back(edit);
			}
		}
		return true;
	}

	const auto primary_at = std::find(chosen.begin(), chosen.end(), primary);
	if (primary_at == chosen.end()) return refuse("The window to align to is not among the windows to arrange.");
	std::vector<ArrangeWindow> items(chosen.size());
	for (size_t i = 0; i < chosen.size(); ++i) {
		int index = -1;
		if (!layout_start(document, chosen[i], compiler, state, items[i].start, &index))
			return refuse("A window to arrange is not one the preview shows (" + document.record_name(chosen[i]) + ").");
		items[i].solve = [&compiler, index](const mnu::Position &candidate) {
			mnu::RectEdges rect{};
			compiler.solve_local_rect(index, candidate, &rect);
			return rect;
		};
		// The nearest window of the list that holds it (the direct owner is last).
		const std::vector<NodeAddress> owners = document.ancestors(chosen[i]);
		for (auto owner = owners.rbegin(); owner != owners.rend() && items[i].holder < 0; ++owner) {
			const auto held_by = std::find(chosen.begin(), chosen.end(), *owner);
			if (held_by != chosen.end()) items[i].holder = int(held_by - chosen.begin());
		}
	}
	std::vector<mnu::Position> positions;
	size_t stuck = SIZE_MAX;
	if (!plan_arrange(op, items, size_t(primary_at - chosen.begin()), positions, &stuck)) {
		if (stuck < chosen.size())
			return refuse(document.record_name(chosen[stuck]) + " has no size on the screen, so " +
			              arrange_op_label(op) + " cannot move it.");
		return refuse(std::string(arrange_op_label(op)) + " cannot arrange these windows.");
	}
	for (size_t i = 0; i < chosen.size(); ++i) {
		const std::vector<Edit> edits = layout_edits(chosen[i], items[i].start.authored, positions[i], 0);
		out.insert(out.end(), edits.begin(), edits.end());
	}
	return true;
}

} // namespace opennova::editor
