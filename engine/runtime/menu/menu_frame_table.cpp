// The table widget interior for the menu frame compiler: the columns (the XML
// HEADER / BODY setups or the ones code installs), the header labels and the
// sort indicator, the body rows with their per-row-state cell passes, the
// shared header/body row heights, the visible-row count, and the table hit test.
// [orig: CUITable_Render @ 0x6411d0; CTableWnd_RecalcLayout @ 0x63f1a0;
//  CTableWnd_ParseXMLContentDefinition @ 0x6427d0; table_hit_test @ 0x63fe90]

#include <runtime/menu/menu_frame_internal.h>

#include <base/io/strutil.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace opennova::menu {

namespace {

// HEADER / BODY JUSTIFY: LEFT 0, CENTER 1, RIGHT 2; another token keeps the value
// [orig: CTableWnd_ParseXMLContentDefinition @ 0x6427d0 — the HEADER walk reads an
// unknown token with wcstol and discards it].
int justify_word(const std::string &token, int current) {
	if (strutil::iequals(token, "LEFT")) return 0;
	if (strutil::iequals(token, "CENTER")) return 1;
	if (strutil::iequals(token, "RIGHT")) return 2;
	return current;
}

// VJUSTIFY: TOP 0, CENTER 16, BOTTOM 32; another token keeps the value.
int vjustify_word(const std::string &token, int current) {
	if (strutil::iequals(token, "TOP")) return 0;
	if (strutil::iequals(token, "CENTER")) return 16;
	if (strutil::iequals(token, "BOTTOM")) return 32;
	return current;
}

} // namespace

// Header height: FIXED_HEADER_HEIGHT when it is 0 or more, else the "W" height;
// body rows: MIN_ITEM_HEIGHT, the "W" height when it is below 0 [orig:
// CUITable_Render @ 0x6411d0 — +816 / +812 against the "W" measure;
// CTableWnd_RecalcLayout @ 0x63f1f4 replaces a negative +812]. A MIN_ITEM_HEIGHT
// of 0 divides by zero in retail as the table is created (the editor refuses to
// write it); the runtime measures it as "W".
void MenuFrameCompiler::table_row_heights_(const WidgetNode &node,
		int *header_height,
		int *body_row_height) const {
	int em_w = 0;
	int em_h = 0;
	measure_text(node, "W", &em_w, &em_h);
	const mnu::TableData &table = node.window->table_data;
	if (header_height != nullptr) {
		*header_height = table.has_fixed_header_height && table.fixed_header_height >= 0
				? table.fixed_header_height
				: em_h;
	}
	if (body_row_height != nullptr) {
		*body_row_height = table.has_min_item_height && table.min_item_height > 0
				? table.min_item_height
				: em_h;
	}
}

// The column set a table draws. Code-installed columns replace the XML ones (the
// stat RESULTLIST resizes the table and sets every column up [orig:
// StatScreen_PopulateStatResultsList @ 0x562240 -> init_table_row @ 0x63f9c0]). The XML
// ones: COUNT columns (1 when there is no COUNT of 1 or more), each zeroed (width
// 0: not drawn) until a HEADER sets it up; a HEADER sets up the column its walk
// stands at (label, carried WIDTH and SORT, JUSTIFY 1 and VJUSTIFY 16 unless it
// authors them, the cells' justification the same), a BODY then sets the cells'
// justification and the cell type [orig: the HEADER arm -> init_table_row; the
// BODY stores @ 0x6427d0: +144 / +148 / +108]. The model keeps a COLUMN's HEADERs
// and BODYs apart: the HEADERs apply first (the order the writer writes them).
std::vector<MenuTableColumn> MenuFrameCompiler::table_columns_(
		const WidgetNode &node, const MenuWidgetState *ws) const {
	if (ws != nullptr && ws->has_table_columns) {
		return ws->table_columns;
	}
	const mnu::TableColumn &xml = node.window->table_data.column;
	const int count = xml.has_count && xml.count >= 1 ? xml.count : 1;
	MenuTableColumn zeroed;
	zeroed.justify = 0;
	zeroed.vjustify = 0;
	zeroed.body_justify = 0;
	zeroed.body_vjustify = 0;
	zeroed.ascending = false;
	std::vector<MenuTableColumn> columns(static_cast<size_t>(count), zeroed);
	const std::vector<mnu::TableHeaderSetup> setup = mnu::table_header_setup(xml);
	for (size_t i = 0; i < xml.headers.size(); ++i) {
		if (!setup[i].set_up) {
			continue;
		}
		const mnu::TableHeader &h = xml.headers[i];
		MenuTableColumn &c = columns[static_cast<size_t>(setup[i].column)];
		c.label = resolve_text_value(node.text_table, h.type, h.text).text;
		c.width = setup[i].width;
		c.justify = justify_word(h.justify, 1);
		c.vjustify = vjustify_word(h.vjustify, 16);
		c.body_justify = c.justify;
		c.body_vjustify = c.vjustify;
		c.numeric_sort = setup[i].numeric_sort;
		c.ascending = true;
	}
	for (const mnu::TableBody &b : xml.bodies) {
		const int index = b.has_column ? b.column : 0;
		if (index < 0 || index >= count) {
			continue; // retail writes past its column array here
		}
		MenuTableColumn &c = columns[static_cast<size_t>(index)];
		c.body_justify = justify_word(b.justify, 1);
		c.body_vjustify = vjustify_word(b.vjustify, 16);
		c.cell_type = b.display.empty()                     ? 0
				: strutil::iequals(b.display, "CUSTOM_DRAW") ? 2
				: strutil::iequals(b.display, "BITMAP_DRAW") ? 1
				: strutil::iequals(b.display, "BITMAP_TEXT") ? 4
															 : 0;
	}
	return columns;
}

// [orig: CTableWnd_RecalcLayout @ 0x63f1a0 — a row's height is the body row
// height once per line its columns wrap onto (the widths add up, a line more
// each time the sum passes the client width); the rows shown are the body's
// height over that, at least 1]
int MenuFrameCompiler::table_visible_rows_(const WidgetNode &node,
		const mnu::RectEdges &rect, const std::vector<MenuTableColumn> &columns) const {
	int header_h = 0;
	int row_h = 0;
	table_row_heights_(node, &header_h, &row_h);
	int total = row_h;
	int accum = 0;
	for (const MenuTableColumn &c : columns) {
		accum += c.width;
		if (accum > rect.right - rect.left) {
			total += row_h;
			accum = 0;
		}
	}
	if (total <= 0) {
		return 1;
	}
	return std::max((rect.bottom - rect.top - header_h) / total, 1);
}

// [orig: table_hit_test @ 0x63fe90] — the point in design space (the mouse over
// the scale, truncated like the pump's ftol); the header height FIXED_HEADER_HEIGHT
// (else "W"); the row pitch the body row height plus one per column-width wrap
// (every column's width counted, a line more each time the running sum passes the
// client width); a point above the header's bottom reads the header's columns
// (left to right from the table's left, SPACING between, a column whose right
// edge passes the table's fails the whole test); below it the row is the offset
// over the pitch clamped to the last visible row, then counted past the scroll
// offset (a row past the last fails); the column is the one whose span holds the
// x, -1 when none does (the body walk has no right-edge check). A column of width
// 0 holds nothing but still advances by SPACING. Rows are never hidden or locked
// here (the rows the runtime keeps carry no row flags: D-MNU-13 residue).
bool MenuFrameCompiler::table_hit(int index, const MenuFrameState &state,
		float mx, float my, float sx, float sy, int *row, int *column) const {
	*row = -1;
	*column = -1;
	if (index < 0 || index >= document_nodes_) {
		return false;
	}
	const WidgetNode &node = nodes_[static_cast<size_t>(index)];
	const MenuWidgetState *ws = state_for(state, index);
	mnu::RectEdges rect;
	if (!widget_rect(index, state, &rect)) {
		return false;
	}
	const int px = static_cast<int>(sx > 0.0f ? mx / sx : mx);
	const int py = static_cast<int>(sy > 0.0f ? my / sy : my);
	if (px < rect.left || px >= rect.right || py < rect.top || py >= rect.bottom) {
		return true; // CWnd_HitTestPoint misses: S_OK, row -1, column -1
	}
	const std::vector<MenuTableColumn> columns = table_columns_(node, ws);
	const int count = ws != nullptr ? static_cast<int>(ws->table_rows.size()) : 0;
	// The child scrollbar claims its strip before the table sees the press
	// [orig: CWnd_DispatchMouseEventToChildren @ 0x647900 — a hit child clears
	// the parent's own hit].
	const int visible = table_visible_rows_(node, rect, columns);
	mnu::RectEdges scrollbar_rect;
	if (count > visible &&
			resolve_scrollbar_rect(node, ScrollbarKind::Embedded, rect, 0,
					rect.bottom - rect.top, 22, &scrollbar_rect) &&
			px >= scrollbar_rect.left && px < scrollbar_rect.right &&
			py >= scrollbar_rect.top && py < scrollbar_rect.bottom) {
		return true;
	}
	int header_h = 0;
	int row_h = 0;
	table_row_heights_(node, &header_h, &row_h);
	int pitch = row_h;
	int accum = 0;
	for (const MenuTableColumn &c : columns) {
		accum += c.width;
		if (accum > rect.right - rect.left) {
			pitch += row_h;
			accum = 0;
		}
	}
	const mnu::TableData &table = node.window->table_data;
	const int gap = table.column.has_spacing ? table.column.spacing : 0;
	int hit_row = -1;
	if (py >= rect.top + header_h && pitch > 0) {
		hit_row = std::min((py - rect.top - header_h) / pitch, visible - 1);
	}
	if (hit_row < 0) {
		// The header strip: the column under the x, or on to the row walk.
		int left = rect.left;
		for (size_t c = 0; c < columns.size(); ++c) {
			const int right = left + columns[c].width;
			if (right > rect.right) {
				return false;
			}
			if (px >= left && px < right) {
				*column = static_cast<int>(c);
				return true;
			}
			left = right + gap;
		}
	}
	// The visible row counted past the scroll offset [orig: the row walk from
	// -1 - scroll, one per row not hidden]. A header point no column holds walks
	// on as visible row -1: the row just above a scrolled view, else nothing.
	if (count == 0) {
		return false;
	}
	const int first = ws->scroll_row > 0 ? ws->scroll_row : 0;
	const int absolute = first + hit_row;
	if (absolute < 0 || absolute >= count) {
		return false;
	}
	*row = absolute;
	int left = rect.left;
	for (size_t c = 0; c < columns.size(); ++c) {
		const int right = left + columns[c].width;
		if (px >= left && px < right) {
			*column = static_cast<int>(c);
			break;
		}
		left = right + gap;
	}
	return true;
}

// [orig: CTableWnd_CalculateAlignedTextRect @ 0x63ec50 — the full text measured at
// scale 1; horizontally LEFT (0) keeps the left edge (right = left + width), 1
// centres ((cell - width) >> 1 off both sides), 2 right (left += cell - width);
// vertically 0 keeps the top (bottom = top + height), 16 centres, 32 bottoms;
// then CFontCache_DrawTextWrapped @ 0x653710 from the aligned rect's top-left with the
// cell's width, in the 0x20000 mode]
mnu::RectEdges MenuFrameCompiler::emit_aligned_text_(const WidgetNode &node,
		const std::string &text, const mnu::RectEdges &cell, int justify,
		int vjustify, const WalkScale &s, uint32_t color) {
	int text_w = 0;
	int text_h = 0;
	measure_text(node, text, &text_w, &text_h);
	const int cell_w = cell.right - cell.left;
	const int cell_h = cell.bottom - cell.top;
	mnu::RectEdges aligned = cell;
	const int v = vjustify & 0xF0;
	if (v == 0) {
		aligned.bottom += text_h - cell_h;
	} else if (v == 16) {
		const int off = (cell_h - text_h) >> 1;
		aligned.top += off;
		aligned.bottom -= off;
	} else if (v == 32) {
		aligned.top += cell_h - text_h;
	}
	const int h = justify & 0xF;
	if (h == 0) {
		aligned.right += text_w - cell_w;
	} else if (h == 1) {
		const int off = (cell_w - text_w) >> 1;
		aligned.left += off;
		aligned.right -= off;
	} else if (h == 2) {
		aligned.left += cell_w - text_w;
	}
	if (!text.empty()) {
		const mnu::RectEdges pen{ aligned.left, aligned.top, aligned.left + cell_w,
			aligned.top + cell_h };
		emit_wrapped_text(node, pen, s, color, text, 0, -1, true);
	}
	return aligned;
}

// The witnessed table interior [orig: CUITable_Render @ 0x6411d0; the full
// walk: docs/mnu/menu-re.md "Table render"]. Image, SUBST and custom cells are the
// D-MNU-13 residue (a custom-drawn column draws neither its header label nor its
// cells here).
void MenuFrameCompiler::emit_table(int index, const WidgetNode &node,
		const mnu::RectEdges &rect, const WalkScale &s,
		const MenuWidgetState *ws) {
	(void)index;
	const std::vector<MenuTableColumn> columns = table_columns_(node, ws);
	int header_h = 0;
	int row_h = 0;
	table_row_heights_(node, &header_h, &row_h);
	const mnu::TableData &table = node.window->table_data;
	const int gap = table.column.has_spacing ? table.column.spacing : 0;
	const int sort_column = ws != nullptr ? ws->table_sort_column : -1;
	// The header row: a column of width 0 is skipped (no pen advance); one whose
	// right edge would pass the table's is neither drawn nor advanced; a negative
	// width draws into an inverted rect and moves the pen left; labels draw with
	// state 0's colors [orig: the header loop @ 0x64137c..0x641390; the header
	// push 0 @ 0x641446].
	int left = rect.left;
	for (size_t c = 0; c < columns.size(); ++c) {
		const MenuTableColumn &col = columns[c];
		if (col.width == 0) {
			continue;
		}
		const int right = left + col.width;
		if (right > rect.right) {
			continue;
		}
		if (col.cell_type == 0 || col.cell_type == 1 || col.cell_type == 4) {
			const mnu::RectEdges cell{ left, rect.top, right, rect.top + header_h };
			const mnu::RectEdges aligned = emit_aligned_text_(node, col.label, cell,
					col.justify, col.vjustify, s, node.colors[kStateDefault]);
			// The sort indicator: only the sorted column draws its taper, in the
			// 16px past the label when there is more than 16px of room [orig:
			// CTableWnd_SortByColumn @ 0x640900 sets "aacceegg" (ascending) or
			// "ggeeccaa" on the sorted column only; CTableWnd_DrawRuleLine @ 0x6410a0,
			// 0xFF7F7F7F].
			if (right - aligned.right > 16 && static_cast<int>(c) == sort_column) {
				const char *rule = col.ascending ? "aacceegg" : "ggeeccaa";
				// [orig: CTableWnd_DrawRuleLine @ 0x6410a0 — the rect scaled to device
				// ints; from strlen/2 rows below its top, one centred line per
				// character, (width - (c - 'a' + 1)) >> 1 in from each side, while
				// the row stays above its bottom]
				const int sl = static_cast<int>(emit_x(aligned.right + 1, s.x));
				const int sr = static_cast<int>(emit_x(aligned.right + 17, s.x));
				const int st = static_cast<int>(emit_x(aligned.top, s.y));
				const int sb = static_cast<int>(emit_x(aligned.bottom, s.y));
				int y = static_cast<int>(std::strlen(rule) >> 1);
				for (const char *ch = rule; *ch != '\0' && y + st < sb; ++ch, ++y) {
					const int wide = *ch - 'a' < 0 ? 1 : *ch - 'a' + 1;
					const int inset = (sr - sl - wide) >> 1;
					push_line(MenuLine{ static_cast<float>(sl + inset),
							static_cast<float>(st + y), static_cast<float>(sr - inset),
							static_cast<float>(st + y), 0xFF7F7F7Fu });
				}
			}
		}
		left = right + gap;
	}
	if (ws == nullptr || ws->table_rows.empty()) {
		return;
	}
	// The body: the rows from the first visible for the visible count, each from
	// the table's left edge; a column of width 0 is skipped, one that would pass
	// the right edge wraps the row down a row height; a cell draws the ITEMS
	// record of the row's state behind it (not a custom-drawn one) and its text
	// in the row state's color (the row's color override in its place) [orig:
	// the body loop @ 0x6415a0..0x641a2e; per-cell records @ 0x641642..0x6416b6;
	// the cell text state = the row state @ 0x64189a..0x6418da; the colour swap
	// on row+28 & 4].
	const int visible = table_visible_rows_(node, rect, columns);
	const int first = ws->scroll_row > 0 ? ws->scroll_row : 0;
	int y = rect.top + header_h;
	for (size_t r = static_cast<size_t>(first);
			r < ws->table_rows.size() && static_cast<int>(r) < first + visible; ++r) {
		bool selected = ws->selected_item == static_cast<int>(r);
		for (const int32_t sel : ws->selected_items) {
			if (sel == static_cast<int32_t>(r)) {
				selected = true;
				break;
			}
		}
		const int row_state = selected ? kStateSelected : kStateDefault;
		uint32_t text_color = node.colors[row_state];
		if (r < ws->table_row_colors.size() && ws->table_row_colors[r].set) {
			text_color = ws->table_row_colors[r].argb | 0xFF000000u;
		}
		const std::vector<std::string> &row = ws->table_rows[r];
		left = rect.left;
		for (size_t c = 0; c < columns.size(); ++c) {
			const MenuTableColumn &col = columns[c];
			if (col.width == 0) {
				continue;
			}
			if (left + col.width > rect.right) {
				y += row_h;
				left = rect.left;
			}
			const mnu::RectEdges cell{ left, y, left + col.width, y + row_h };
			if (col.cell_type != 2) {
				if (node.items_states[row_state].present) {
					emit_state_pass(cell, s, node.items_states[row_state]);
				}
				if ((col.cell_type == 0 || col.cell_type == 4) && c < row.size() &&
						!row[c].empty()) {
					emit_aligned_text_(node, row[c], cell, col.body_justify,
							col.body_vjustify, s, text_color);
				}
			}
			left = cell.right + gap;
		}
		y += row_h;
	}
}

} // namespace opennova::menu
