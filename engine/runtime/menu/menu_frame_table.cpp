// The table widget interior for the menu frame compiler: header labels and
// dividers, the visible-row walk with per-row-state cell passes, the shared
// header/body row heights, and row hit-testing.
// [orig: CUITable_Render @ 0x6411d0; CTableWnd_RecalcLayout @ 0x63f1a0]

#include <runtime/menu/menu_frame_internal.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::menu {

void MenuFrameCompiler::table_row_heights_(const WidgetNode &node,
		int *header_height,
		int *body_row_height) const {
	int em_w = 0;
	int em_h = 0;
	measure_text(node, "W", &em_w, &em_h);
	if (header_height != nullptr) {
		*header_height = em_h;
	}
	if (body_row_height != nullptr) {
		const mnu::TableData &table = node.window->table_data;
		*body_row_height = table.has_min_item_height && table.min_item_height > 0
				? table.min_item_height
				: em_h;
	}
}

int MenuFrameCompiler::table_row_at(int index, const MenuFrameState &state,
		float mx, float my, float sx, float sy) const {
	if (index < 0 || index >= static_cast<int>(nodes_.size())) {
		return -1;
	}
	const WidgetNode &node = nodes_[static_cast<size_t>(index)];
	const mnu::TableData &table = node.window->table_data;
	if (table.column.headers.empty()) {
		return -1;
	}
	const MenuWidgetState *ws = state_for(state, index);
	if (ws == nullptr || ws->table_rows.empty()) {
		return -1;
	}
	mnu::RectEdges rect;
	if (!widget_rect(index, state, &rect)) {
		return -1;
	}
	int header_h = 0;
	int row_h = 0;
	table_row_heights_(node, &header_h, &row_h);
	if (row_h <= 0) {
		return -1;
	}
	if (mx < emit_x(rect.left, sx) || mx >= emit_x(rect.right, sx)) {
		return -1;
	}
	// Retail stores at least one visible row even when the body is shorter
	// than a full row, then derives the child scrollbar page/range from it.
	// [orig: CTableWnd_RecalcLayout @ 0x63f1a0, clamp @ 0x63f276]
	const int visible =
			row_h > 0 ? std::max((rect.bottom - rect.top - header_h) / row_h, 1) : 0;
	mnu::RectEdges scrollbar_rect;
	if (static_cast<int>(ws->table_rows.size()) > visible &&
			resolve_scrollbar_rect(node, ScrollbarKind::Embedded, rect, 0,
					rect.bottom - rect.top, 22, &scrollbar_rect) &&
			mx >= emit_x(scrollbar_rect.left, sx) &&
			mx < emit_x(scrollbar_rect.right, sx) &&
			my >= emit_x(scrollbar_rect.top, sy) &&
			my < emit_x(scrollbar_rect.bottom, sy)) {
		return -1;
	}
	const int first = ws->scroll_row > 0 ? ws->scroll_row : 0;
	int y = rect.top + header_h;
	for (size_t r = static_cast<size_t>(first); r < ws->table_rows.size(); ++r) {
		if (y + row_h > rect.bottom) {
			break;
		}
		if (my >= emit_x(y, sy) && my < emit_x(y + row_h, sy)) {
			return static_cast<int>(r);
		}
		y += row_h;
	}
	return -1;
}

// The witnessed table interior [orig: CUITable_Render @ 0x6411d0; the full
// walk: docs/mnu/menu-re.md "Table render"]. The compiled path draws the
// header labels, the character-profiled taper dividers, and the seeded data
// rows as text cells. Image/substitution/custom cells and per-row appearance
// records are deferred with the Control-tree renderer (D-MNU-13 follow-up);
// no shipped .mnu drives them through the compiled path yet.
void MenuFrameCompiler::emit_table(int index, const WidgetNode &node,
		const mnu::RectEdges &rect, const WalkScale &s,
		const MenuWidgetState *ws) {
	(void)index;
	const mnu::TableData &table = node.window->table_data;
	const std::vector<mnu::TableHeader> &headers = table.column.headers;
	if (headers.empty()) {
		return;
	}
	// The "W" measure supplies the default row/header heights [orig: the
	// font_cache_measure_text_default("W") probe; authored min_item_height
	// wins when present].
	int header_h = 0;
	int row_h = 0;
	table_row_heights_(node, &header_h, &row_h);
	const int gap = table.column.has_spacing ? table.column.spacing : 0;
	// Cells draw with the ROW's own state, never the widget hover visual:
	// the header always pushes state 0, and a selected row is state 3
	// [orig: CUITable_Render header push 0 @ 0x641446, cell text state =
	//  row state @ 0x64189a..0x6418da; CTableWnd_SetRowSelected @ 0x63f5f0
	//  keeps states 0/1/3].
	const uint32_t color = node.colors[kStateDefault];
	// Header labels + the taper rule divider under each column with >16px of
	// headroom [orig: the header walk; divider color 0xFF7F7F7F]. The rule
	// PROFILE string is not authored in the XML model (the runtime sets it),
	// so the compiled divider draws the plain full-taper line row.
	int x = rect.left;
	for (const mnu::TableHeader &h : headers) {
		const int width = h.has_width ? h.width : 0;
		if (width <= 0) {
			continue;
		}
		const std::string label = resolve_text_value(h.type, h.text);
		int text_w = 0;
		int text_h = 0;
		measure_text(node, label, &text_w, &text_h);
		int tx = x;
		if (h.justify == "CENTER") {
			tx = x + (width - text_w) / 2;
		} else if (h.justify == "RIGHT") {
			tx = x + width - text_w;
		}
		emit_glyph_run(node, label, tx, rect.top, s, color, -1);
		if (width - text_w > 16) {
			// One divider segment centered in the headroom band [orig:
			// draw_rule_line @ 0x6410a0 — 0xFF7F7F7F].
			const int seg_left = x + text_w + 1;
			const int seg_right = x + width - 1;
			const int seg_y = rect.top + header_h / 2;
			push_line(MenuLine{ emit_x(seg_left, s.x), emit_x(seg_y, s.y),
					emit_x(seg_right, s.x), emit_x(seg_y, s.y),
					0xFF7F7F7Fu });
		}
		x += width + gap;
	}
	if (ws == nullptr || ws->table_rows.empty()) {
		return;
	}
	// Data rows: the scroll window is first-visible + as many rows as fit
	// below the header [orig: the visible-row walk]. Each cell draws the
	// ITEMS appearance pass for the row's state before its text — the
	// default-state outline is the cell grid, the selected-state color is
	// the selection bar [orig: per-cell backgrounds from the row-state
	// appearance record @ 0x641642..0x6416b6].
	const int first = ws->scroll_row > 0 ? ws->scroll_row : 0;
	int y = rect.top + header_h;
	for (size_t r = static_cast<size_t>(first); r < ws->table_rows.size();
			++r) {
		if (y + row_h > rect.bottom) {
			break;
		}
		bool selected = ws->selected_item == static_cast<int>(r);
		for (const int32_t sel : ws->selected_items) {
			if (sel == static_cast<int32_t>(r)) {
				selected = true;
				break;
			}
		}
		const int row_state = selected ? kStateSelected : kStateDefault;
		const std::vector<std::string> &row = ws->table_rows[r];
		x = rect.left;
		for (size_t c = 0; c < headers.size(); ++c) {
			const mnu::TableHeader &h = headers[c];
			const int width = h.has_width ? h.width : 0;
			if (width <= 0) {
				continue;
			}
			if (node.items_states[row_state].present) {
				const mnu::RectEdges cell{ x, y, x + width, y + row_h };
				emit_state_pass(cell, s, node.items_states[row_state]);
			}
			if (c < row.size() && !row[c].empty()) {
				emit_glyph_run(node, row[c], x, y, s,
						node.colors[row_state], -1);
			}
			x += width + gap;
		}
		y += row_h;
	}
}

} // namespace opennova::menu
