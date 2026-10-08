// Scrollbar planning and painting for the menu frame compiler. This internal
// module keeps authored/fallback rect resolution, original range arithmetic,
// child painter order, and combo-popup integration behind the compiler's two
// private scrollbar operations.
// [orig: CScrollWnd_Render @ 0x64c5c0; CUIScrollbar_CalcThumbRect @ 0x64cba0]

#include <runtime/menu/menu_frame_internal.h>

#include <algorithm>
#include <cstdint>

namespace opennova::menu {

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
	out->window = rect;
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
			// The rows not hidden against the pitch-based visible count
			// [orig: CTableWnd_RecalcLayout @ 0x63f1a0].
			*rows = table_live_rows_(ws);
			*visible = table_visible_rows_(node, rect, table_columns_(node, ws));
			return true;
		}
		case mnu::WindowType::List:
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

// A scroll value written by the scrollbar's interaction: a standalone Scroll
// widget's authored-range value (none written without a seeded range: the
// deterministic zero-range hold), an embedded owner's first visible row; a
// change rides the claim [orig: CScrollWnd_HandleEvent @ 0x64d126..0x64d15c —
// the value clamped, the thumb laid out again, 0x4000001 with the value].
void MenuFrameCompiler::apply_scroll_value_(MenuFrameState &io_state, int index,
		int value, MouseClaim *claim) {
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
			return;
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
	claim->scroll_index = index;
	claim->scroll_value_changed = true;
	claim->scroll_value = value;
}

int MenuFrameCompiler::scroll_value_(int index, const MenuFrameState &state) const {
	const MenuWidgetState *ws = state_for(state, index);
	if (nodes_[static_cast<size_t>(index)].window->type == mnu::WindowType::Scroll) {
		return ws != nullptr && ws->has_scroll_range ? ws->scroll_value : 0;
	}
	return ws != nullptr ? std::max(ws->scroll_row, 0) : 0;
}

// The scrollbar's window under the point, front first: its buttons (the last
// made, SCROLLWND_DOWN, first), else the scroll window itself, its track
// (kMenuPumpPartScroll); 0 none [orig: CScrollWnd_Construct @ 0x64c450;
// CUIScrollbar_CreateChildWindows @ 0x64d330 makes the shuttle, the up arrow,
// then the down arrow; the pump's and the press's child walks run last to
// first, CWnd_ProcessMouseEvent @ 0x647cc8, CWnd_DispatchMouseEventToChildren
// @ 0x6479a1]. The raw mouse against the scaled rects, as the claim walk's.
int MenuFrameCompiler::scroll_window_part_(const ScrollParts &parts, float mx,
		float my, float sx, float sy) {
	auto holds = [&](const mnu::RectEdges &r) {
		return mx >= emit_x(r.left, sx) && mx < emit_x(r.right, sx) &&
				my >= emit_x(r.top, sy) && my < emit_x(r.bottom, sy);
	};
	if (holds(parts.down)) {
		return kMenuPumpPartScrollDown;
	}
	if (holds(parts.up)) {
		return kMenuPumpPartScrollUp;
	}
	if (holds(parts.shuttle)) {
		return kMenuPumpPartScrollShuttle;
	}
	return holds(parts.window) ? kMenuPumpPartScroll : 0;
}

// Whether a widget's scrollbar is a window the pump and the press reach: a
// SCROLL widget's own buttons, or the shown scroll window of a LIST, LAN_LIST
// or TABLE (shown while its rows overflow [orig: list_insert_row @ 0x645125;
// CTableWnd_RecalcLayout @ 0x63f1a0]). A combo's dropdown is the dropdown
// pump's (pump_popup_mouse).
bool MenuFrameCompiler::scroll_windows_(int index, const MenuFrameState &state,
		ScrollParts *out) const {
	switch (nodes_[static_cast<size_t>(index)].window->type) {
		case mnu::WindowType::Scroll:
		case mnu::WindowType::List:
		case mnu::WindowType::LanList:
		case mnu::WindowType::Table:
			return solve_scroll_for_widget_(index, state, out);
		default:
			return false;
	}
}

// A scrollbar window's own press handler [orig: CScrollWnd_HandleEvent
// @ 0x64d050]: the scroll window's own (its track's: the press reached it with
// none of its buttons under the point) pages toward the point, value - page
// when the shuttle starts past it, else + page, and captures nothing
// (@ 0x64d087..0x64d10e; the page +0xC00, CScrollWnd_SetPageSize @ 0x64ce10,
// ctor default 10 @ 0x64c4d9; an embedded owner's the visible rows less one);
// the shuttle's anchors its drag, shuttle origin less the track base less the
// point along the axis (SCROLLWND_SHUTTLE 0x1000002, @ 0x64d1cb..0x64d217);
// an arrow's does nothing on the press (its step is its click,
// click_scroll_window). False for a window that is not a scrollbar's.
bool MenuFrameCompiler::press_scroll_window(MenuFrameState &io_state,
		const MenuPumpWindow &window, float mouse_x, float mouse_y,
		float scale_x, float scale_y, MouseClaim *claim) {
	if (claim == nullptr || window.index < 0 || window.index >= document_nodes_) {
		return false;
	}
	const WidgetNode &node = nodes_[static_cast<size_t>(window.index)];
	const bool standalone = node.window->type == mnu::WindowType::Scroll;
	const bool track = window.part == kMenuPumpPartScroll || (standalone && window.part == 0);
	if (!track && !menu_pump_part_scroll(window.part)) {
		return false;
	}
	if (track) {
		int page = 10;
		if (standalone) {
			const MenuWidgetState *ws = state_for(io_state, window.index);
			if (ws != nullptr && ws->has_scroll_range) {
				page = ws->scroll_page;
			}
		} else {
			page = scroll_page_rows(window.index, io_state);
		}
		ScrollParts parts;
		if (!solve_scroll_for_widget_(window.index, io_state, &parts)) {
			return true;
		}
		const float mx = scale_x > 0.0f ? mouse_x / scale_x : mouse_x;
		const float my = scale_y > 0.0f ? mouse_y / scale_y : mouse_y;
		// [orig: @ 0x64d0b9..0x64d0e8 — the shuttle's origin less the point
		// along the axis; more than 0 pages back]
		const float shuttle_start = parts.vertical ? static_cast<float>(parts.shuttle.top)
												   : static_cast<float>(parts.shuttle.left);
		const float point = parts.vertical ? my : mx;
		const int value = scroll_value_(window.index, io_state);
		apply_scroll_value_(io_state, window.index,
				shuttle_start - point > 0.0f ? value - page : value + page, claim);
		return true;
	}
	if (window.part == kMenuPumpPartScrollShuttle) {
		scroll_drag_.index = window.index;
		scroll_drag_.anchor = scroll_drag_anchor(window.index, io_state, mouse_x,
				mouse_y, scale_x, scale_y);
	}
	return true;
}

// A scrollbar window's click [orig: CScrollWnd_HandleEvent @ 0x64d2bb..0x64d320
// — SCROLLWND_UP's 0x3000001 steps value - step, SCROLLWND_DOWN's + step, the
// step +0xBFC, ctor default 1 @ 0x64c4cf; the track's and the shuttle's click
// go to the parent with the scroll window's name, @ 0x64d194, where nothing
// takes them]. True for a scrollbar's window (the click is the scrollbar's,
// never its owner's), false otherwise.
bool MenuFrameCompiler::click_scroll_window(MenuFrameState &io_state,
		const MenuPumpWindow &window, MouseClaim *claim) {
	if (claim == nullptr || window.index < 0 || window.index >= document_nodes_ ||
			!menu_pump_part_scroll(window.part)) {
		return false;
	}
	if (window.part == kMenuPumpPartScrollUp || window.part == kMenuPumpPartScrollDown) {
		const int value = scroll_value_(window.index, io_state);
		apply_scroll_value_(io_state, window.index,
				window.part == kMenuPumpPartScrollUp ? value - 1 : value + 1, claim);
	}
	return true;
}

// The shuttle's drag while its press holds the capture: each move maps the
// point back to a value [orig: CScrollWnd_HandleEvent @ 0x64d231..0x64d2aa —
// SCROLLWND_SHUTTLE's 0x1000001 while g_UIMouseCaptureWnd is the shuttle; the
// press's own sample is no move]. A capture whose owner stopped solving (its
// rows came to fit) moves nothing.
void MenuFrameCompiler::drag_scroll_shuttle_(MenuFrameState &io_state,
		const MenuPumpWindow &capture, float mouse_x, float mouse_y,
		float scale_x, float scale_y, MouseClaim *claim) {
	if (capture.part != kMenuPumpPartScrollShuttle || capture.index != scroll_drag_.index ||
			capture.index < 0 || capture.index >= document_nodes_) {
		return;
	}
	ScrollParts parts;
	if (!solve_scroll_for_widget_(capture.index, io_state, &parts)) {
		return;
	}
	apply_scroll_value_(io_state, capture.index,
			scroll_drag_value(capture.index, io_state, mouse_x, mouse_y, scale_x,
					scale_y, scroll_drag_.anchor),
			claim);
}

// One wheel tick against the row-scroll model — a DELIBERATE divergence
// (D-MNU-18): retail JO ships no functioning menu wheel scroll. The witnessed
// plumbing: WM_MOUSEWHEEL accumulates HIWORD(wParam) and fires one callback
// tick per +/-120 with direction masks 0x100/0x200 [orig:
// Input_DispatchMouseEvent @ 0x761571..0x7615d0], the shell bridge collapses
// BOTH masks into the direction-less widget event 0x100000B that no widget
// handler consumes [orig: Menu_ShellMouseCallback @ 0x54b8c6;
// UI_DispatchMouseEvent @ 0x63ab72], and the in-game bridge (the armory's)
// drops the ticks entirely [orig: Menu_InGameMouseCallback @ 0x568760]. By
// the 2026-08-12 maintainer decision the reimpl scrolls anyway: one tick =
// one row (the CScrollWnd arrow step), routed the way the witnessed dispatch
// routes every mouse event — the open popup exclusively [orig:
// g_UIOpenPopupWnd gate @ 0x63abb5], else the front-most row owner under
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
		if (w == nullptr || !widget_shown_(i, io_state) ||
				!in_subtree_(i, io_state.popup_root)) {
			continue;
		}
		switch (w->type) {
			case mnu::WindowType::List:
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

// The open-dropdown pump: the dropdown's scrollbar sees the sample ahead of
// its rows, the dropdown alone pumped [orig: UI_DispatchMouseEvent @ 0x63ab00
// g_UIOpenPopupWnd gate; CListWnd child walk @ 0x643f30; CScrollWnd_HandleEvent
// @ 0x64d050] (which retail pump serves the dropdown's list is open: D-MNU-11/12,
// docs/mnu/menu-re.md "Not ported, or open"). Its windows keep the game's rule
// as any scrollbar's (D-MNU-32): a press on an arrow holds it until the
// release, which steps when it comes over the arrow it held there the sample
// before; a press on the track pages and holds nothing; a press on the shuttle
// holds it and drags until the release. The claim's scroll_index reports a
// sample the scrollbar took (its press, or one its held window holds), whose
// row hover and pick stay with the caller when it does not.
MenuFrameCompiler::MouseClaim MenuFrameCompiler::pump_popup_mouse(
		MenuFrameState &io_state, int index, float mouse_x, float mouse_y,
		bool button_down, float scale_x, float scale_y) {
	MouseClaim claim;
	if (screen_ == nullptr || index < 0 || index >= document_nodes_) {
		return claim;
	}
	claim.cursor = first_root_cursor_();
	PopupScroll &held = popup_scroll_;
	const bool press = button_down && !held.down;
	held.down = button_down;
	ScrollParts parts;
	const int part = solve_scroll_for_widget_(index, io_state, &parts)
			? scroll_window_part_(parts, mouse_x, mouse_y, scale_x, scale_y)
			: 0;
	if (!button_down) {
		// The release lets the held window go: an arrow it comes over, held
		// the sample before, is clicked.
		const int was = held.part;
		const bool clicked = was == part && held.under;
		held = PopupScroll();
		if (clicked) {
			click_scroll_window(io_state, MenuPumpWindow{ index, was }, &claim);
		}
		return claim;
	}
	if (held.part != 0) {
		held.under = part == held.part;
		if (held.part == kMenuPumpPartScrollShuttle && !press) {
			drag_scroll_shuttle_(io_state, MenuPumpWindow{ index, held.part }, mouse_x,
					mouse_y, scale_x, scale_y, &claim);
		}
		claim.scroll_index = index;
		return claim;
	}
	if (!press || part == 0) {
		return claim;
	}
	press_scroll_window(io_state, MenuPumpWindow{ index, part }, mouse_x, mouse_y,
			scale_x, scale_y, &claim);
	if (part != kMenuPumpPartScroll) {
		held.part = part;
		held.under = true;
	}
	held.down = true;
	claim.scroll_index = index;
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
			(w.list_box && w.list_box->items.present) ? node.popup_items : node.items;
	if (selected < 0 || selected >= static_cast<int>(rows.size())) {
		return std::string();
	}
	return rows[static_cast<size_t>(selected)].text;
}

// A list-like widget's row as it displays: the runtime rows when seeded, else
// the authored row after the string-table lookup (a combo's LIST_BOX rows
// when authored) — the text the list row carries [orig: sub_6447C0 @0x6447c0
// reads the row's text, resolved at the ITEM parse].
std::string MenuFrameCompiler::item_display_text(int index, const MenuFrameState &state,
		int row) const {
	if (index < 0 || index >= static_cast<int>(nodes_.size())) return std::string();
	const WidgetNode &node = nodes_[static_cast<size_t>(index)];
	if (node.window == nullptr) return std::string();
	const MenuWidgetState *ws = state_for(state, index);
	if (ws != nullptr && ws->has_items) {
		return row >= 0 && row < static_cast<int>(ws->items.size())
				? ws->items[static_cast<size_t>(row)]
				: std::string();
	}
	const mnu::Window &w = *node.window;
	const std::vector<WidgetNode::ItemVisual> &rows =
			(w.list_box && w.list_box->items.present) ? node.popup_items : node.items;
	return row >= 0 && row < static_cast<int>(rows.size()) ? rows[static_cast<size_t>(row)].text
															  : std::string();
}

// The combo LIST_BOX popup at its authored combo-relative rect. Keeping this
// beside scrollbar planning makes the popup child painter order local: rows
// paint before the authored scrollbar child.
// [orig: CComboWnd_Render @ 0x65bfd0; CListWnd_DrawItems @ 0x643f30]
void MenuFrameCompiler::emit_combo_popup(const WidgetNode &node,
		const mnu::RectEdges &rect, const WalkScale &s,
		const MenuWidgetState *ws) {
	const mnu::Window &w = *node.window;
	if (!w.list_box) {
		return;
	}
	const mnu::RectEdges local = mnu::position_rect(
			w.list_box->position.has_left, w.list_box->position.left,
			w.list_box->position.has_top, w.list_box->position.top,
			w.list_box->position.has_right, w.list_box->position.right,
			w.list_box->position.has_bottom, w.list_box->position.bottom, 0, 0);
	const mnu::RectEdges popup = offset_rect(local, rect.left, rect.top);
	// Popup background appearances (default state).
	emit_state_pass(popup, s, node.popup_states[kStateDefault]);
	// Rows: runtime-seeded rows win; else the authored nested collection wins
	// even when empty, else the top-level items (the documented D-MNU-7/8
	// model).
	const bool runtime_rows = ws != nullptr && ws->has_items;
	const std::vector<WidgetNode::ItemVisual> &rows =
			(w.list_box && w.list_box->items.present) ? node.popup_items : node.items;
	const int row_count = runtime_rows ? static_cast<int>(ws->items.size())
									   : static_cast<int>(rows.size());
	const int row_h = row_height_(node);
	if (row_h <= 0) {
		return;
	}
	const int selected = ws != nullptr ? ws->selected_item : -1;
	const int hovered = ws != nullptr ? ws->hover_item : -1;
	const int edge =
			w.list_box->string_data.has_edge ? w.list_box->string_data.edge : 0;
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
		const StatePass *row_states = (w.list_box && w.list_box->items.present)
				? node.popup_items_states
				: node.items_states;
		if (style >= 0 && row_states[style].present) {
			const StatePass &pass = row_states[style];
			const mnu::RectEdges hi{ row.left + 1, row.top, row.right - 1, row.bottom };
			emit_state_pass(hi, s, pass);
		}
		// The text lays out in the whole row, the edge pad only shortening its span
		// (emit_list_row_text_); a row the game added takes the LIST_BOX's ITEMS layout.
		const mnu::RectEdges text_row{ popup.left, row.top, popup.right, row.bottom };
		const int edge_pad = show_scrollbar ? node.popup_scrollbar.edge_pad : 0;
		const int color_state = style >= 0 ? style : kStateDefault;
		if (runtime_rows) {
			const WidgetNode::RowLayout &layout = node.popup_row_layout;
			emit_list_row_text_(node, text_row, ws->items[static_cast<size_t>(i)], layout.align(),
					layout.x, layout.y, edge, edge_pad, s, node.colors[color_state]);
		} else {
			const WidgetNode::ItemVisual &item = rows[static_cast<size_t>(i)];
			if (item.kind == WidgetNode::ItemVisual::kText) {
				emit_list_row_text_(node, text_row, item.text, item.layout.align(), item.layout.x,
						item.layout.y, edge, edge_pad, s, node.colors[color_state]);
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
