// Scrollbar planning and painting for the menu frame compiler. This internal
// module keeps authored/fallback rect resolution, original range arithmetic,
// child painter order, and combo-popup integration behind the compiler's two
// private scrollbar operations.
// [orig: CScrollWnd_Render @ 0x64c5c0; CUIScrollbar_CalcThumbRect @ 0x64cba0]

#include "menu/menu_frame_internal.h"

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
	const int track_slot =
			track_state >= 0 && track_state < 4 && visual->track[track_state].present
			? track_state
			: kStateDefault;
	const StatePass &track = visual->track[track_slot];
	const StatePass &shuttle = visual->shuttle[kStateDefault];
	const StatePass &up = visual->up[kStateDefault];
	const StatePass &down = visual->down[kStateDefault];
	const int extent = std::max(visual->part_extent, 0);
	const int axis =
			visual->vertical ? rect.bottom - rect.top : rect.right - rect.left;
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

	mnu::RectEdges track_rect;
	mnu::RectEdges up_rect;
	mnu::RectEdges down_rect;
	mnu::RectEdges shuttle_rect;
	if (visual->vertical) {
		track_rect = { rect.left, rect.top + extent, rect.right,
			rect.bottom - extent };
		up_rect = { rect.left, rect.top, rect.right, rect.top + extent };
		down_rect = { rect.left, rect.bottom - extent, rect.right, rect.bottom };
		shuttle_rect = { rect.left, rect.top + shuttle_offset, rect.right,
			rect.top + shuttle_offset + shuttle_length };
	} else {
		track_rect = { rect.left + extent, rect.top, rect.right - extent,
			rect.bottom };
		up_rect = { rect.left, rect.top, rect.left + extent, rect.bottom };
		down_rect = { rect.right - extent, rect.top, rect.right, rect.bottom };
		shuttle_rect = { rect.left + shuttle_offset, rect.top,
			rect.left + shuttle_offset + shuttle_length, rect.bottom };
	}
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
	const StatePass &bg = node.popup_states[kStateDefault];
	if (bg.has_color) {
		emit_rect_quad(popup, s, bg.color, kMenuTexNone, false, 1.0f, 1.0f);
	}
	if (bg.texture >= 0) {
		emit_state_texture(popup, s, bg);
	}
	if (bg.has_outline) {
		emit_outline(popup, s, bg.outline);
	}
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
		const StatePass(&row_states)[4] = w.list_box.items.present
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
