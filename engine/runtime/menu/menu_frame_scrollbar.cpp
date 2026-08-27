// Scrollbar planning and painting for the menu frame compiler. This internal
// module keeps authored/fallback rect resolution, original range arithmetic,
// child painter order, and combo-popup integration behind the compiler's two
// private scrollbar operations.
// [orig: CScrollWnd_Render @ 0x64c5c0; CUIScrollbar_CalcThumbRect @ 0x64cba0]

#include <runtime/menu/menu_frame_internal.h>

#include <algorithm>
#include <cstdint>

namespace opennova::menu {

namespace {

mnu::RectEdges offset_rect(const mnu::RectEdges &rect, int dx, int dy) {
	return { rect.left + dx, rect.top + dy, rect.right + dx, rect.bottom + dy };
}

} // namespace

bool MenuFrameCompiler::resolve_scrollbar_rect(
		const WidgetNode &node, ScrollbarKind kind, const mnu::RectEdges &owner,
		int fallback_top, int fallback_height, int fallback_width,
		mnu::RectEdges *out) const {
	if (out == nullptr) {
		return false;
	}
	const WidgetNode::ScrollbarVisual *visual = nullptr;
	switch (kind) {
		case ScrollbarKind::Standalone:
			visual = &node.scrollbar;
			break;
		case ScrollbarKind::Embedded:
			visual = &node.embedded_scrollbar;
			break;
		case ScrollbarKind::Popup:
			visual = &node.popup_scrollbar;
			break;
	}
	if (visual == nullptr || !visual->present) {
		return false;
	}
	if (kind == ScrollbarKind::Standalone) {
		*out = owner;
		return owner.right > owner.left && owner.bottom > owner.top;
	}

	int left = 0;
	int top = fallback_top;
	int width = std::max(fallback_width, 0);
	int height = std::max(fallback_height, 0);
	if (visual->has_position) {
		const mnu::Position &position = visual->position;
		const int authored_width =
				position.has_right ? std::max(position.right - position.left, 0) : 0;
		if (authored_width > 0) {
			left = position.left;
			top = position.top;
			width = authored_width;
			height = position.has_bottom ? std::max(position.bottom - position.top, 0)
										 : std::max(fallback_height, 0);
		} else {
			// A zero-width parsed child uses the owner's rightmost 22px/full
			// height fallback supplied by its list/edit/table owner.
			// [orig: CListWnd_CreateScrollChild @ 0x6444c0;
			// CMEditWnd_CreateScrollChild @ 0x661260;
			// CTableWnd_Init @ 0x640790]
			left = std::max(owner.right - owner.left - width, 0);
		}
	} else {
		left = std::max(owner.right - owner.left - width, 0);
	}
	*out = { owner.left + left, owner.top + top, owner.left + left + width,
		owner.top + top + height };
	return width > 0 && height > 0;
}

// The scroll's own appearance plus shuttle/up/down children. COLOR and OUTLINE
// use the full parent rect; IMAGE is the middle track inset by the one authored
// part extent. Children then paint shuttle -> up -> down. Range/page/value are
// the original CScrollWnd fields: page is inclusive (visible count - 1), thumb
// length is track * (page + 1) / (page - min + max + 1), and position is
// normalized from min over max-min.
// [orig: CScrollWnd_Render @ 0x64c5c0; COLOR sink @ 0x64ce70; IMAGE sink
// @ 0x64cf70; CUIScrollbar_CreateChildWindows @ 0x64d330;
// CUIScrollbar_CalcThumbRect @ 0x64cba0; CScrollWnd_SetPageSize @ 0x64ce10;
// CScrollWnd_SetRangeAndClamp @ 0x64d490]
void MenuFrameCompiler::emit_scrollbar(const WidgetNode &node,
		ScrollbarKind kind,
		const mnu::RectEdges &rect,
		const WalkScale &s, int range_min,
		int range_max, int page, int value,
		int track_state) {
	const WidgetNode::ScrollbarVisual *visual = nullptr;
	switch (kind) {
		case ScrollbarKind::Standalone:
			visual = &node.scrollbar;
			break;
		case ScrollbarKind::Embedded:
			visual = &node.embedded_scrollbar;
			break;
		case ScrollbarKind::Popup:
			visual = &node.popup_scrollbar;
			break;
	}
	if (visual == nullptr || !visual->present) {
		return;
	}
	ScrollParts parts;
	if (!solve_scroll_parts_(node, kind, rect, range_min, range_max, page,
				value, &parts)) {
		return;
	}
	const int track_slot =
			track_state >= 0 && track_state < 4 && visual->track[track_state].present
			? track_state
			: kStateDefault;
	const StatePass &track = visual->track[track_slot];
	const StatePass &shuttle = visual->shuttle[kStateDefault];
	const StatePass &up = visual->up[kStateDefault];
	const StatePass &down = visual->down[kStateDefault];
	const mnu::RectEdges &track_rect = parts.track;
	const mnu::RectEdges &up_rect = parts.up;
	const mnu::RectEdges &down_rect = parts.down;
	const mnu::RectEdges &shuttle_rect = parts.shuttle;
	if (track.has_color) {
		emit_rect_quad(rect, s, track.color, kMenuTexNone, false, 1.0f, 1.0f);
	}
	emit_state_texture(track_rect, s, track);
	if (track.has_outline) {
		emit_outline(rect, s, track.outline);
	}
	emit_state_pass(shuttle_rect, s, shuttle);
	emit_state_pass(up_rect, s, up);
	emit_state_pass(down_rect, s, down);
}

// Shared scrollbar part geometry [orig: CUIScrollbar_CalcThumbRect @ 0x64cba0;
// CScrollWnd_UpdateThumbPosition @ 0x64cd50 - thumb origin = track_base +
// (value - min) * ratio; thumb length = track * (page + 1) /
// (page - min + max + 1), min 20px].
bool MenuFrameCompiler::solve_scroll_parts_(const WidgetNode &node,
		ScrollbarKind kind, const mnu::RectEdges &rect,
		int range_min, int range_max, int page, int value,
		ScrollParts *out) const {
	if (out == nullptr) {
		return false;
	}
	const WidgetNode::ScrollbarVisual *visual_ptr = nullptr;
	switch (kind) {
		case ScrollbarKind::Standalone:
			visual_ptr = &node.scrollbar;
			break;
		case ScrollbarKind::Embedded:
			visual_ptr = &node.embedded_scrollbar;
			break;
		case ScrollbarKind::Popup:
			visual_ptr = &node.popup_scrollbar;
			break;
	}
	if (visual_ptr == nullptr || !visual_ptr->present) {
		return false;
	}
	const WidgetNode::ScrollbarVisual &visual = *visual_ptr;
	const int extent = std::max(visual.part_extent, 0);
	const int axis =
			visual.vertical ? rect.bottom - rect.top : rect.right - rect.left;
	const int track_length = std::max(axis - 2 * extent, 0);
	int shuttle_length = track_length;
	const int range_length = std::max(range_max - range_min, 0);
	if (range_length > 0) {
		const int64_t numerator =
				static_cast<int64_t>(track_length) * (static_cast<int64_t>(page) + 1);
		const int64_t denominator =
				static_cast<int64_t>(page) - range_min + range_max + 1;
		shuttle_length = denominator != 0
				? static_cast<int>(numerator / denominator)
				: track_length;
		shuttle_length = std::min(shuttle_length, track_length);
		shuttle_length = std::max(shuttle_length, 20);
	}
	const int travel = track_length - shuttle_length;
	int shuttle_offset = extent;
	if (range_length > 0) {
		const int clamped_value = std::clamp(value, range_min, range_max);
		shuttle_offset +=
				static_cast<int>(static_cast<int64_t>(clamped_value - range_min) *
						travel / range_length);
	}
	if (visual.vertical) {
		out->track = { rect.left, rect.top + extent, rect.right,
			rect.bottom - extent };
		out->up = { rect.left, rect.top, rect.right, rect.top + extent };
		out->down = { rect.left, rect.bottom - extent, rect.right, rect.bottom };
		out->shuttle = { rect.left, rect.top + shuttle_offset, rect.right,
			rect.top + shuttle_offset + shuttle_length };
	} else {
		out->track = { rect.left + extent, rect.top, rect.right - extent,
			rect.bottom };
		out->up = { rect.left, rect.top, rect.left + extent, rect.bottom };
		out->down = { rect.right - extent, rect.top, rect.right, rect.bottom };
		out->shuttle = { rect.left + shuttle_offset, rect.top,
			rect.left + shuttle_offset + shuttle_length, rect.bottom };
	}
	out->vertical = visual.vertical;
	out->extent = extent;
	out->travel = travel;
	out->shuttle_offset = shuttle_offset;
	out->range_min = range_min;
	out->range_max = range_max;
	return true;
}

// Dispatch by widget type: a standalone Scroll widget scrolls its authored
// range; a Table/List embedded scrollbar scrolls first-visible ROWS (range
// 0..rows-visible, page = visible - 1, value = scroll_row); an OPEN combo's
// popup scrollbar scrolls the popup rows the same way [orig: the table
// SCROLLBAR delegate @ 0x643b22; CMEditWnd page = visibleLines - 1;
// CScrollWnd_SetRangeAndClamp @ 0x64d490; CListWnd child walk @ 0x643f30].
bool MenuFrameCompiler::solve_scroll_for_widget_(int index,
		const MenuFrameState &state, ScrollParts *out) const {
	if (index < 0 || index >= static_cast<int>(nodes_.size())) {
		return false;
	}
	const WidgetNode &node = nodes_[static_cast<size_t>(index)];
	if (node.window == nullptr) {
		return false;
	}
	mnu::RectEdges rect;
	if (!widget_rect(index, state, &rect)) {
		return false;
	}
	const MenuWidgetState *ws = state_for(state, index);
	const mnu::WindowType type = node.window->type;
	if (type == mnu::WindowType::Scroll && node.scrollbar.present) {
		const int range_min =
				ws != nullptr && ws->has_scroll_range ? ws->scroll_min : 0;
		const int range_max =
				ws != nullptr && ws->has_scroll_range ? ws->scroll_max : 0;
		const int page = ws != nullptr && ws->has_scroll_range ? ws->scroll_page : 10;
		const int value = ws != nullptr && ws->has_scroll_range ? ws->scroll_value : 0;
		return solve_scroll_parts_(node, ScrollbarKind::Standalone, rect,
				range_min, range_max, page, value, out);
	}
	if (type == mnu::WindowType::Combo) {
		// The popup's scrollbar child exists only while the dropdown is open
		// (retail's child lives on the popup CListWnd) — geometry identical
		// to emit_combo_popup's so hits land on the drawn parts.
		if (ws == nullptr || !ws->popup_open || !node.popup_scrollbar.present) {
			return false;
		}
		int rows = 0;
		int visible = 0;
		if (!scroll_row_span_(index, state, &rows, &visible) ||
				rows <= visible) {
			return false;
		}
		mnu::RectEdges popup;
		if (!combo_popup_rect(index, state, &popup)) {
			return false;
		}
		mnu::RectEdges scrollbar_rect;
		if (!resolve_scrollbar_rect(node, ScrollbarKind::Popup, popup, 0,
					popup.bottom - popup.top, 22, &scrollbar_rect)) {
			return false;
		}
		const int value = std::max(ws->scroll_row, 0);
		return solve_scroll_parts_(node, ScrollbarKind::Popup, scrollbar_rect,
				0, std::max(rows - visible, 0), std::max(visible - 1, 0),
				value, out);
	}
	if (!node.embedded_scrollbar.present) {
		return false;
	}
	int rows = 0;
	int visible = 0;
	if (!scroll_row_span_(index, state, &rows, &visible) || rows <= visible) {
		return false;
	}
	mnu::RectEdges scrollbar_rect;
	if (!resolve_scrollbar_rect(node, ScrollbarKind::Embedded, rect, 0,
				rect.bottom - rect.top, 22, &scrollbar_rect)) {
		return false;
	}
	const int value = ws != nullptr ? std::max(ws->scroll_row, 0) : 0;
	return solve_scroll_parts_(node, ScrollbarKind::Embedded, scrollbar_rect, 0,
			std::max(rows - visible, 0), std::max(visible - 1, 0), value, out);
}

// Rows + visible rows for the embedded-scrollbar owners (Table counts its
// body under the header; List/Multi/LanList by row height).
bool MenuFrameCompiler::scroll_row_span_(int index, const MenuFrameState &state,
		int *rows, int *visible) const {
	if (rows == nullptr || visible == nullptr || index < 0 ||
			index >= static_cast<int>(nodes_.size())) {
		return false;
	}
	const WidgetNode &node = nodes_[static_cast<size_t>(index)];
	if (node.window == nullptr) {
		return false;
	}
	const MenuWidgetState *ws = state_for(state, index);
	switch (node.window->type) {
		case mnu::WindowType::Table: {
			mnu::RectEdges rect;
			if (!widget_rect(index, state, &rect)) {
				return false;
			}
			int header_h = 0;
			int row_h = 0;
			table_row_heights_(node, &header_h, &row_h);
			if (row_h <= 0) {
				return false;
			}
			*rows = ws != nullptr ? static_cast<int>(ws->table_rows.size()) : 0;
			*visible = std::max(
					(rect.bottom - rect.top - header_h) / row_h, 1);
			return true;
		}
		case mnu::WindowType::List:
		case mnu::WindowType::Multi:
		case mnu::WindowType::LanList: {
			*rows = item_count(index, state);
			*visible = std::max(list_visible_rows(index, state), 1);
			return true;
		}
		case mnu::WindowType::Combo: {
			// Rows of the OPEN dropdown (the popup CListWnd): the same
			// row/visible model against the authored LIST_BOX rect, matching
			// emit_combo_popup's draw gate.
			if (ws == nullptr || !ws->popup_open) {
				return false;
			}
			mnu::RectEdges popup;
			if (!combo_popup_rect(index, state, &popup)) {
				return false;
			}
			const int row_h = row_height_(node);
			if (row_h <= 0) {
				return false;
			}
			*rows = item_count(index, state);
			*visible = std::max((popup.bottom - popup.top) / row_h, 0);
			return true;
		}
		default:
			return false;
	}
}

// The scrollable widget whose scrollbar PARTS contain the point — shipped
// menus author scrollbar rects OUTSIDE the owner's widget rect (options.mnu
// CONTROL_MAPPING: table-relative 452..472 on a 451-wide table), so the
// plain rect claim misses them (the same class as the D-MNU-16 outside
// spin arrows).
int MenuFrameCompiler::scroll_owner_at(const MenuFrameState &state,
		float mouse_x, float mouse_y, float scale_x, float scale_y) const {
	const float mx = scale_x > 0.0f ? mouse_x / scale_x : mouse_x;
	const float my = scale_y > 0.0f ? mouse_y / scale_y : mouse_y;
	for (int i = 0; i < static_cast<int>(nodes_.size()); ++i) {
		if (!widget_shown_(i, state)) {
			continue;
		}
		ScrollParts parts;
		if (!solve_scroll_for_widget_(i, state, &parts)) {
			continue;
		}
		auto inside = [&](const mnu::RectEdges &r) {
			return mx >= r.left && mx < r.right && my >= r.top && my < r.bottom;
		};
		if (inside(parts.up) || inside(parts.down) || inside(parts.shuttle) ||
				inside(parts.track)) {
			return i;
		}
	}
	return -1;
}

int MenuFrameCompiler::scroll_row_limit(int index,
		const MenuFrameState &state) const {
	int rows = 0;
	int visible = 0;
	if (!scroll_row_span_(index, state, &rows, &visible)) {
		return 0;
	}
	return std::max(rows - visible, 0);
}

int MenuFrameCompiler::scroll_page_rows(int index,
		const MenuFrameState &state) const {
	int rows = 0;
	int visible = 0;
	if (!scroll_row_span_(index, state, &rows, &visible)) {
		return 0;
	}
	return std::max(visible - 1, 1);
}

// The witnessed CScrollWnd interaction map [orig: CScrollWnd_HandleEvent
// @ 0x64d050 - SCROLLWND_UP/DOWN click = value -/+ step (ctor default 1
// @ 0x64c4cf); a track press pages toward the click (value -/+ page,
// SetPageSize @ 0x64ce10; ctor default 10 @ 0x64c4d9); a shuttle press
// captures and drags].
int MenuFrameCompiler::scroll_hit_at(int index, const MenuFrameState &state,
		float mouse_x, float mouse_y, float scale_x, float scale_y) const {
	ScrollParts parts;
	if (!solve_scroll_for_widget_(index, state, &parts)) {
		return kScrollHitNone;
	}
	const float mx = scale_x > 0.0f ? mouse_x / scale_x : mouse_x;
	const float my = scale_y > 0.0f ? mouse_y / scale_y : mouse_y;
	auto inside = [&](const mnu::RectEdges &r) {
		return mx >= r.left && mx < r.right && my >= r.top && my < r.bottom;
	};
	if (inside(parts.up)) {
		return kScrollHitUp;
	}
	if (inside(parts.down)) {
		return kScrollHitDown;
	}
	if (inside(parts.shuttle)) {
		return kScrollHitShuttle;
	}
	if (inside(parts.track)) {
		const float axis_pos = parts.vertical ? my : mx;
		const float shuttle_start = parts.vertical
				? static_cast<float>(parts.shuttle.top)
				: static_cast<float>(parts.shuttle.left);
		return axis_pos < shuttle_start ? kScrollHitTrackBefore
										: kScrollHitTrackAfter;
	}
	return kScrollHitNone;
}

// Press anchor: shuttle origin minus the mouse along the axis (design units)
// [orig: the L-down leg @ 0x64d1cb..0x64d217 - this[763] = shuttle_origin -
// track_base - mouse].
int MenuFrameCompiler::scroll_drag_anchor(int index,
		const MenuFrameState &state, float mouse_x, float mouse_y,
		float scale_x, float scale_y) const {
	ScrollParts parts;
	if (!solve_scroll_for_widget_(index, state, &parts)) {
		return 0;
	}
	const float mx = scale_x > 0.0f ? mouse_x / scale_x : mouse_x;
	const float my = scale_y > 0.0f ? mouse_y / scale_y : mouse_y;
	const int axis_pos = static_cast<int>(parts.vertical ? my : mx);
	return parts.shuttle_offset - axis_pos;
}

// Drag: the would-be shuttle offset (mouse + anchor) maps back to a value
// through the travel ratio, clamped [orig: the capture-move leg
// @ 0x64d231..0x64d2aa - value = min + offset / ratio, clamped;
// CScrollWnd_UpdateThumbPosition @ 0x64cd50 is the forward map].
int MenuFrameCompiler::scroll_drag_value(int index,
		const MenuFrameState &state, float mouse_x, float mouse_y,
		float scale_x, float scale_y, int anchor) const {
	ScrollParts parts;
	if (!solve_scroll_for_widget_(index, state, &parts)) {
		return 0;
	}
	const int range_length = std::max(parts.range_max - parts.range_min, 0);
	if (range_length <= 0 || parts.travel <= 0) {
		return parts.range_min;
	}
	const float mx = scale_x > 0.0f ? mouse_x / scale_x : mouse_x;
	const float my = scale_y > 0.0f ? mouse_y / scale_y : mouse_y;
	const int axis_pos = static_cast<int>(parts.vertical ? my : mx);
	const int offset = axis_pos + anchor - parts.extent;
	const double ratio =
			static_cast<double>(parts.travel) / static_cast<double>(range_length);
	// The original's double->int is an ftol truncation, not a rounding.
	const int value = parts.range_min +
			static_cast<int>(static_cast<double>(offset) / ratio);
	return std::clamp(value, parts.range_min, parts.range_max);
}

void MenuFrameCompiler::emit_row_scrollbar_(int index, const WidgetNode &node,
		const mnu::RectEdges &rect, const WalkScale &s,
		const MenuFrameState &state, const MenuWidgetState *ws) {
	int rows = 0;
	int visible = 0;
	if (!scroll_row_span_(index, state, &rows, &visible) || rows <= visible) {
		return;
	}
	mnu::RectEdges scrollbar_rect;
	if (!resolve_scrollbar_rect(node, ScrollbarKind::Embedded, rect, 0,
				rect.bottom - rect.top, 22, &scrollbar_rect)) {
		return;
	}
	emit_scrollbar(node, ScrollbarKind::Embedded, scrollbar_rect, s, 0,
			std::max(rows - visible, 0), std::max(visible - 1, 0),
			ws != nullptr ? std::max(ws->scroll_row, 0) : 0, kStateDefault);
}

// The witnessed CScrollWnd interaction, run ahead of the claim walk [orig:
// CScrollWnd_HandleEvent @ 0x64d050 — SCROLLWND_UP/DOWN click = value -/+
// step (ctor default 1 @ 0x64c4cf); a track press pages toward the click
// (value -/+ page, SetPageSize @ 0x64ce10, ctor default 10 @ 0x64c4d9); a
// shuttle press captures an anchor and drags through the travel ratio
// (@ 0x64d1cb..0x64d2aa)]. The pressed part keeps the mouse until release,
// like retail's child-BUTTON capture, so a press that began on a scrollbar
// can never become a click on another widget. A disabled owner still claims
// (blocking widgets beneath) but takes no action. Standalone Scroll widgets
// change their authored-range value; embedded row owners change scroll_row.
bool MenuFrameCompiler::scroll_pump_mouse_(MenuFrameState &io_state,
		float mouse_x, float mouse_y, bool button_down, float scale_x,
		float scale_y, MouseClaim *claim, int restrict_index) {
	const bool press_edge = button_down && !scroll_pump_.button_was_down;
	scroll_pump_.button_was_down = button_down;
	if (!button_down) {
		scroll_pump_.captured_index = -1;
		scroll_pump_.latched_index = -1;
		return false; // the release sample flows to the normal claim walk
	}
	auto value_of = [&](int index) {
		const MenuWidgetState *ws = state_for(io_state, index);
		if (nodes_[static_cast<size_t>(index)].window->type ==
				mnu::WindowType::Scroll) {
			return ws != nullptr && ws->has_scroll_range ? ws->scroll_value : 0;
		}
		return ws != nullptr ? std::max(ws->scroll_row, 0) : 0;
	};
	auto apply = [&](int index, int value) {
		const bool standalone = nodes_[static_cast<size_t>(index)].window->type ==
				mnu::WindowType::Scroll;
		MenuWidgetState *row = nullptr;
		for (MenuWidgetState &candidate : io_state.widgets) {
			if (candidate.index == index) {
				row = &candidate;
				break;
			}
		}
		if (row == nullptr) {
			MenuWidgetState fresh;
			fresh.index = index;
			io_state.widgets.push_back(fresh);
			row = &io_state.widgets.back();
		}
		if (standalone) {
			if (!row->has_scroll_range) {
				return; // no seeded range: the deterministic zero-range hold
			}
			value = std::clamp(value, row->scroll_min, row->scroll_max);
			if (value == row->scroll_value) {
				return;
			}
			row->scroll_value = value;
		} else {
			value = std::clamp(value, 0, scroll_row_limit(index, io_state));
			if (value == std::max(row->scroll_row, 0)) {
				return;
			}
			row->scroll_row = value;
		}
		claim->scroll_value_changed = true;
		claim->scroll_value = value;
	};
	if (scroll_pump_.captured_index >= 0) {
		const int index = scroll_pump_.captured_index;
		claim->hovered = index;
		claim->scroll_index = index;
		// A capture whose owner stopped solving (its popup closed under the
		// held mouse) keeps the claim but moves nothing — retail's capture
		// dies with the child window and the held press goes nowhere.
		ScrollParts parts;
		if (solve_scroll_for_widget_(index, io_state, &parts)) {
			apply(index, scroll_drag_value(index, io_state, mouse_x, mouse_y,
					scale_x, scale_y, scroll_pump_.drag_anchor));
		}
		return true;
	}
	if (scroll_pump_.latched_index >= 0) {
		claim->hovered = scroll_pump_.latched_index;
		claim->scroll_index = scroll_pump_.latched_index;
		return true; // held after an arrow/track press: no auto-repeat
	}
	if (!press_edge) {
		return false;
	}
	// A popup-exclusive pump restricts owner resolution to the open combo
	// [orig: dispatch_mouse_event @ 0x63ab00 g_ui_open_popup_wnd — while a
	// popup is open only the popup window sees the event].
	const int index = restrict_index >= 0
			? (scroll_hit_at(restrict_index, io_state, mouse_x, mouse_y,
					   scale_x, scale_y) != kScrollHitNone
							  ? restrict_index
							  : -1)
			: scroll_owner_at(io_state, mouse_x, mouse_y, scale_x, scale_y);
	if (index < 0) {
		return false;
	}
	claim->hovered = index;
	claim->scroll_index = index;
	if (widget_disabled(index, io_state)) {
		scroll_pump_.latched_index = index;
		return true;
	}
	int page = 10;
	if (nodes_[static_cast<size_t>(index)].window->type ==
			mnu::WindowType::Scroll) {
		const MenuWidgetState *ws = state_for(io_state, index);
		if (ws != nullptr && ws->has_scroll_range) {
			page = ws->scroll_page;
		}
	} else {
		page = scroll_page_rows(index, io_state);
	}
	switch (scroll_hit_at(index, io_state, mouse_x, mouse_y, scale_x,
			scale_y)) {
		case kScrollHitUp:
			apply(index, value_of(index) - 1);
			scroll_pump_.latched_index = index;
			break;
		case kScrollHitDown:
			apply(index, value_of(index) + 1);
			scroll_pump_.latched_index = index;
			break;
		case kScrollHitTrackBefore:
			apply(index, value_of(index) - page);
			scroll_pump_.latched_index = index;
			break;
		case kScrollHitTrackAfter:
			apply(index, value_of(index) + page);
			scroll_pump_.latched_index = index;
			break;
		case kScrollHitShuttle:
			scroll_pump_.captured_index = index;
			scroll_pump_.drag_anchor = scroll_drag_anchor(index, io_state,
					mouse_x, mouse_y, scale_x, scale_y);
			break;
		default:
			scroll_pump_.latched_index = index;
			break;
	}
	return true;
}

// One wheel tick against the row-scroll model — a DELIBERATE divergence
// (D-MNU-18): retail JO ships no functioning menu wheel scroll. The witnessed
// plumbing: WM_MOUSEWHEEL accumulates HIWORD(wParam) and fires one callback
// tick per +/-120 with direction masks 0x100/0x200 [orig:
// Input_DispatchMouseEvent @ 0x761571..0x7615d0], the shell bridge collapses
// BOTH masks into the direction-less widget event 0x100000B that no widget
// handler consumes [orig: Menu_ShellMouseCallback @ 0x54b8c6;
// dispatch_mouse_event @ 0x63ab72], and the in-game bridge (the armory's)
// drops the ticks entirely [orig: Menu_InGameMouseCallback @ 0x568760]. By
// the 2026-08-12 maintainer decision the reimpl scrolls anyway: one tick =
// one row (the CScrollWnd arrow step), routed the way the witnessed dispatch
// routes every mouse event — the open popup exclusively [orig:
// g_ui_open_popup_wnd gate @ 0x63abb5], else the front-most row owner under
// the point (the reverse child walk @ 0x63abd3).
bool MenuFrameCompiler::pump_mouse_wheel(MenuFrameState &io_state,
		float mouse_x, float mouse_y, int steps, float scale_x, float scale_y,
		MouseClaim *claim) {
	if (screen_ == nullptr || nodes_.empty() || steps == 0 ||
			claim == nullptr) {
		return false;
	}
	auto apply_row = [&](int index) {
		claim->hovered = index;
		claim->scroll_index = index;
		MenuWidgetState *row = nullptr;
		for (MenuWidgetState &candidate : io_state.widgets) {
			if (candidate.index == index) {
				row = &candidate;
				break;
			}
		}
		if (row == nullptr) {
			MenuWidgetState fresh;
			fresh.index = index;
			io_state.widgets.push_back(fresh);
			row = &io_state.widgets.back();
		}
		const int value = std::clamp(std::max(row->scroll_row, 0) + steps, 0,
				scroll_row_limit(index, io_state));
		if (value == std::max(row->scroll_row, 0)) {
			return;
		}
		row->scroll_row = value;
		claim->scroll_value_changed = true;
		claim->scroll_value = value;
	};
	// The open popup owns the mouse exclusively — every tick scrolls it,
	// wherever the cursor sits.
	for (int i = 0; i < static_cast<int>(nodes_.size()); ++i) {
		const mnu::Window *w = nodes_[static_cast<size_t>(i)].window;
		if (w == nullptr || w->type != mnu::WindowType::Combo) {
			continue;
		}
		const MenuWidgetState *ws = state_for(io_state, i);
		if (ws != nullptr && ws->popup_open) {
			apply_row(i);
			return true;
		}
	}
	// Front-most row owner under the point (draw order = pre-order; later
	// nodes paint over earlier, so the reverse scan finds the front-most).
	const float mx = scale_x > 0.0f ? mouse_x / scale_x : mouse_x;
	const float my = scale_y > 0.0f ? mouse_y / scale_y : mouse_y;
	for (int i = static_cast<int>(nodes_.size()) - 1; i >= 0; --i) {
		const mnu::Window *w = nodes_[static_cast<size_t>(i)].window;
		if (w == nullptr || !widget_shown_(i, io_state)) {
			continue;
		}
		switch (w->type) {
			case mnu::WindowType::List:
			case mnu::WindowType::Multi:
			case mnu::WindowType::LanList:
			case mnu::WindowType::Table:
				break;
			default:
				continue;
		}
		mnu::RectEdges rect;
		if (!widget_rect(i, io_state, &rect)) {
			continue;
		}
		if (mx < rect.left || mx >= rect.right || my < rect.top ||
				my >= rect.bottom) {
			continue;
		}
		if (scroll_row_limit(i, io_state) <= 0) {
			return false; // the front-most owner fits: nothing scrolls
		}
		apply_row(i);
		return true;
	}
	return false;
}

// The open-dropdown pump: the popup's scrollbar child sees the sample ahead
// of row picking, restricted to the open combo — while a popup is open only
// the popup window receives events, and its scrollbar child claims before
// the row strip [orig: dispatch_mouse_event @ 0x63ab00 g_ui_open_popup_wnd
// gate; CListWnd child walk @ 0x643f30; CScrollWnd_HandleEvent @ 0x64d050].
// The claim reports whether the scrollbar owns the sample; row hover/pick
// stays with the caller when it does not.
MenuFrameCompiler::MouseClaim MenuFrameCompiler::pump_popup_mouse(
		MenuFrameState &io_state, int index, float mouse_x, float mouse_y,
		bool button_down, float scale_x, float scale_y) {
	MouseClaim claim;
	if (screen_ == nullptr || index < 0 ||
			index >= static_cast<int>(nodes_.size())) {
		return claim;
	}
	claim.cursor = screen_cursor_;
	scroll_pump_mouse_(io_state, mouse_x, mouse_y, button_down, scale_x,
			scale_y, &claim, index);
	return claim;
}

// [orig: CComboWnd_Render @ 0x65c05b..0x65c083 — this[183] = row_text(list,
// selected_row(list)); CStaticWnd_DrawLabel; restore. Text-only: image/color
// rows contribute their stored text (possibly empty).]
std::string MenuFrameCompiler::combo_face_text(const WidgetNode &node,
		const MenuWidgetState *ws) const {
	const int selected = ws != nullptr ? ws->selected_item : 0;
	if (ws != nullptr && ws->has_items) {
		if (selected < 0 || selected >= static_cast<int>(ws->items.size())) {
			return std::string();
		}
		return ws->items[static_cast<size_t>(selected)];
	}
	const mnu::Window &w = *node.window;
	const std::vector<WidgetNode::ItemVisual> &rows =
			w.list_box.items.present ? node.popup_items : node.items;
	if (selected < 0 || selected >= static_cast<int>(rows.size())) {
		return std::string();
	}
	return rows[static_cast<size_t>(selected)].text;
}

// The combo LIST_BOX popup at its authored combo-relative rect. Keeping this
// beside scrollbar planning makes the popup child painter order local: rows
// paint before the authored scrollbar child.
// [orig: CComboWnd_Render @ 0x65bfd0; CListWnd_DrawItems @ 0x643f30]
void MenuFrameCompiler::emit_combo_popup(const WidgetNode &node,
		const mnu::RectEdges &rect, const WalkScale &s,
		const MenuWidgetState *ws) {
	const mnu::Window &w = *node.window;
	if (!w.list_box.present) {
		return;
	}
	const mnu::RectEdges local = mnu::position_rect(
			w.list_box.position.has_left, w.list_box.position.left,
			w.list_box.position.has_top, w.list_box.position.top,
			w.list_box.position.has_right, w.list_box.position.right,
			w.list_box.position.has_bottom, w.list_box.position.bottom, 0, 0);
	const mnu::RectEdges popup = offset_rect(local, rect.left, rect.top);
	// Popup background appearances (default state).
	emit_state_pass(popup, s, node.popup_states[kStateDefault]);
	// Rows: runtime-seeded rows win; else the authored nested collection wins
	// even when empty, else the top-level items (the documented D-MNU-7/8
	// model).
	const bool runtime_rows = ws != nullptr && ws->has_items;
	const std::vector<WidgetNode::ItemVisual> &rows =
			w.list_box.items.present ? node.popup_items : node.items;
	const int row_count = runtime_rows ? static_cast<int>(ws->items.size())
									   : static_cast<int>(rows.size());
	const int row_h = row_height_(node);
	if (row_h <= 0) {
		return;
	}
	const int selected = ws != nullptr ? ws->selected_item : -1;
	const int hovered = ws != nullptr ? ws->hover_item : -1;
	const int edge =
			w.list_box.string_data.has_edge ? w.list_box.string_data.edge : 0;
	const int first = ws != nullptr ? std::max(ws->scroll_row, 0) : 0;
	const int visible_rows = std::max((popup.bottom - popup.top) / row_h, 0);
	mnu::RectEdges scrollbar_rect;
	const bool show_scrollbar =
			row_count > visible_rows &&
			resolve_scrollbar_rect(node, ScrollbarKind::Popup, popup, 0,
					popup.bottom - popup.top, 22, &scrollbar_rect);
	const int content_right =
			show_scrollbar && node.popup_scrollbar.edge_pad > 0
			? std::max(popup.left, popup.right - node.popup_scrollbar.edge_pad)
			: popup.right;
	int y = popup.top;
	for (int i = first; i < row_count; ++i) {
		if (y + row_h > popup.bottom) {
			break;
		}
		const mnu::RectEdges row{ popup.left, y, content_right, y + row_h };
		int style = -1;
		if (i == selected) {
			style = kStateSelected;
		} else if (i == hovered) {
			style = kStateMouseover;
		}
		// A conditional whose operands are arrays decays both to pointers, so the
		// result cannot bind to a StatePass(&)[4]; the pointer form is the same
		// object and indexes identically.
		const StatePass *row_states = w.list_box.items.present
				? node.popup_items_states
				: node.items_states;
		if (style >= 0 && row_states[style].present) {
			const StatePass &pass = row_states[style];
			const mnu::RectEdges hi{ row.left + 1, row.top, row.right - 1, row.bottom };
			emit_state_pass(hi, s, pass);
		}
		if (runtime_rows) {
			const std::string &text = ws->items[static_cast<size_t>(i)];
			if (!text.empty()) {
				const int color_state = style >= 0 ? style : kStateDefault;
				emit_glyph_run(node, text, row.left + edge, row.top, s,
						node.colors[color_state], -1);
			}
		} else {
			const WidgetNode::ItemVisual &item = rows[static_cast<size_t>(i)];
			if (item.kind == WidgetNode::ItemVisual::kText &&
					!item.text.empty()) {
				const int color_state = style >= 0 ? style : kStateDefault;
				emit_glyph_run(node, item.text, row.left + edge, row.top, s,
						node.colors[color_state], -1);
			}
		}
		y += row_h;
	}
	if (show_scrollbar) {
		emit_scrollbar(node, ScrollbarKind::Popup, scrollbar_rect, s, 0,
				std::max(row_count - visible_rows, 0), visible_rows - 1,
				first, kStateDefault);
	}
}

} // namespace opennova::menu
