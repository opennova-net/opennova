// The table widget interior for the menu frame compiler: the column set-up
// the XML makes, the header cells, the visible-row walk with its per-row-state
// cell passes, the SUBST images and the custom-draw cells, the cell draw a
// custom-draw handler calls back into, the row pitch and visible count, and the
// hit test.
// [orig: CUITable_Render @ 0x6411d0; CTableWnd_DrawCell @ 0x640be0;
//  CTableWnd_RecalcLayout @ 0x63f1a0; CTableWnd_HitTest @ 0x63fe90;
//  CTableWnd_ParseXMLContentDefinition @ 0x6427d0; docs/mnu/menu-re.md
//  "Table render"]

#include <runtime/menu/menu_frame_internal.h>

#include <base/io/strutil.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace opennova::menu {

namespace {

// HEADER / BODY JUSTIFY: LEFT 0, CENTER 1, RIGHT 2; another token is read as a
// number (the HEADER walk discards it, the BODY walk keeps it as the cell's x
// offset) and keeps the justification.
// [orig: CTableWnd_ParseXMLContentDefinition @ 0x6427d0 — the BODY JUSTIFY arm
//  (+144, the wcstol store +152), the HEADER arm's reset to 1 / 16]
int justify_word(const std::string &token, int current, int *numeric) {
	if (token.empty()) return current;
	if (strutil::iequals(token, "LEFT")) return 0;
	if (strutil::iequals(token, "CENTER")) return 1;
	if (strutil::iequals(token, "RIGHT")) return 2;
	if (numeric != nullptr) *numeric = static_cast<int>(std::strtol(token.c_str(), nullptr, 10));
	return current;
}

// VJUSTIFY: TOP 0, CENTER 16, BOTTOM 32; another token as above (+156).
int vjustify_word(const std::string &token, int current, int *numeric) {
	if (token.empty()) return current;
	if (strutil::iequals(token, "TOP")) return 0;
	if (strutil::iequals(token, "CENTER")) return 16;
	if (strutil::iequals(token, "BOTTOM")) return 32;
	if (numeric != nullptr) *numeric = static_cast<int>(std::strtol(token.c_str(), nullptr, 10));
	return current;
}

} // namespace

// The column set-up the COLUMN element makes: COUNT columns (1 without a
// COUNT of 1 or more), each zeroed (width 0: not drawn, not hit) until a
// HEADER sets it up; HEADER, BODY and SUBST share a running column index a
// COLUMN attribute moves. A HEADER sets the label, the carried WIDTH (100 at
// the COLUMN element), JUSTIFY 1 / VJUSTIFY 16 unless authored, the cells'
// justification the same; a BODY then sets the cells' justification (1 / 16
// unless authored, a numeric token the cell offset), the draw kind (the first
// authored of CUSTOM_DRAW 2 / BITMAP_DRAW 1 wins: the attribute walk runs last
// authored first) and SCALE_BITMAP; a SUBST adds a value -> image row, the
// lookup meeting the LAST authored first (the rows are prepended). The model
// keeps the three row kinds apart, so the running index restarts per kind
// (every shipped row authors its COLUMN).
// [orig: CTableWnd_ParseXMLContentDefinition @ 0x6427d0 — the BODY stores
//  +108 / +144 / +148 / +152 / +156 / +168 / +172, the SUBST node
//  {value, file, URL, FILE, next} prepended at column +164; init_table_row
//  @ 0x63f9c0; resize_column_count @ 0x63f6c0]
void MenuFrameCompiler::build_table_columns_(WidgetNode &node) {
	const mnu::TableColumn &xml = node.window->table_data.column;
	const int count = xml.has_count && xml.count >= 1 ? xml.count : 1;
	node.table_columns.assign(static_cast<size_t>(count), TableColumnSetup{});
	node.table_spacing = xml.has_spacing ? xml.spacing : 0;
	int running = 0;
	int carried_width = 100;
	for (const mnu::TableHeader &h : xml.headers) {
		if (h.has_column) running = h.column;
		if (h.has_width) carried_width = h.width;
		if (running < 0 || running >= count) continue;
		TableColumnSetup &c = node.table_columns[static_cast<size_t>(running)];
		c.width = carried_width;
		c.label = resolve_text_value(h.type, h.text).text;
		c.header_justify = justify_word(h.justify, 1, nullptr);
		c.header_vjustify = vjustify_word(h.vjustify, 16, nullptr);
		c.body_justify = c.header_justify;
		c.body_vjustify = c.header_vjustify;
	}
	running = 0;
	for (const mnu::TableBody &b : xml.bodies) {
		if (b.has_column) running = b.column;
		if (running < 0 || running >= count) continue; // retail writes past its array
		TableColumnSetup &c = node.table_columns[static_cast<size_t>(running)];
		c.body_x = 0;
		c.body_y = 0;
		c.body_justify = justify_word(b.justify, 1, &c.body_x);
		c.body_vjustify = vjustify_word(b.vjustify, 16, &c.body_y);
		c.cell_type = b.custom_draw ? kTableCellCustom : b.bitmap_draw ? kTableCellImage : kTableCellText;
		c.scale_bitmap = b.scale_bitmap;
	}
	running = 0;
	for (const mnu::TableSubst &sub : xml.substitutions) {
		if (sub.has_column) running = sub.column;
		if (running < 0 || running >= count) continue;
		TableColumnSetup::Subst row;
		row.value = sub.value;
		row.texture = sub.is_file ? intern_texture(sub.file) : kMenuTexNone;
		node.table_columns[static_cast<size_t>(running)].subst.push_back(row);
	}
}

// The image SetCellText puts beside a cell text: the column's SUBST rows,
// last authored first, the first whose value matches (stricmp) — a FILE row
// gives its texture, any other row none.
// [orig: CTableWnd_SetCellText @ 0x63edf0 — the +164 walk, the texture load
//  when the node's FILE / URL word is set]
int32_t MenuFrameCompiler::table_cell_image_(const WidgetNode &node, int column,
		const std::string &text) const {
	if (column < 0 || column >= static_cast<int>(node.table_columns.size())) return kMenuTexNone;
	const std::vector<TableColumnSetup::Subst> &rows =
			node.table_columns[static_cast<size_t>(column)].subst;
	for (auto it = rows.rbegin(); it != rows.rend(); ++it)
		if (strutil::iequals(it->value, text)) return it->texture;
	return kMenuTexNone;
}

// Header height: the "W" height (FIXED_HEADER_HEIGHT is not in this model and
// no shipped table authors it); body rows: MIN_ITEM_HEIGHT, the "W" height
// when it is not a positive height.
// [orig: CUITable_Render @ 0x6411d0 — +816 / +812 against the "W" measure;
//  CTableWnd_RecalcLayout @ 0x63f1f4 replaces a negative +812]
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

// The row pitch: one body row height, plus one per time the column widths
// overflow the table's width (the overflowing column's width is dropped).
// [orig: CTableWnd_RecalcLayout @ 0x63f1a0 — @0x63f220..0x63f24b]
int MenuFrameCompiler::table_pitch_(const WidgetNode &node, const mnu::RectEdges &rect,
		int row_h) const {
	int pitch = row_h;
	int accum = 0;
	const int width = rect.right - rect.left;
	for (const TableColumnSetup &c : node.table_columns) {
		accum += c.width;
		if (accum > width) {
			pitch += row_h;
			accum = 0;
		}
	}
	return pitch;
}

// The visible row count: the body height over the pitch, at least 1.
// [orig: CTableWnd_RecalcLayout @ 0x63f25a..0x63f276]
int MenuFrameCompiler::table_visible_rows_(const WidgetNode &node,
		const mnu::RectEdges &rect) const {
	int header_h = 0;
	int row_h = 0;
	table_row_heights_(node, &header_h, &row_h);
	if (row_h <= 0) return 1;
	const int pitch = table_pitch_(node, rect, row_h);
	return std::max((rect.bottom - rect.top - header_h) / pitch, 1);
}

// The rows the scroll range counts: every row not hidden.
// [orig: CTableWnd_RecalcLayout @ 0x63f285..0x63f29d]
int MenuFrameCompiler::table_live_rows_(const MenuWidgetState *ws) {
	if (ws == nullptr) return 0;
	int live = 0;
	for (const MenuTableRow &row : ws->table_rows)
		if (!row.hidden()) ++live;
	return live;
}

int MenuFrameCompiler::table_column_count(int index) const {
	if (index < 0 || index >= static_cast<int>(nodes_.size())) return 0;
	const WidgetNode &node = nodes_[static_cast<size_t>(index)];
	if (node.window == nullptr || node.window->type != mnu::WindowType::Table) return 0;
	return static_cast<int>(node.table_columns.size());
}

void MenuFrameCompiler::set_table_columns(int index,
		const std::vector<MenuTableColumnDef> &columns) {
	table_column_defs_[index] = columns;
	if (index < 0 || index >= static_cast<int>(nodes_.size())) return;
	WidgetNode &node = nodes_[static_cast<size_t>(index)];
	if (node.window == nullptr || node.window->type != mnu::WindowType::Table) return;
	build_table_columns_(node);
	apply_table_column_defs_(index, node);
}

// The records code set up over the authored layout (menu_table_row.h
// MenuTableColumnDef): a kept record is the authored column, any other the
// runtime's own from zero (a count that grows the table starts every record
// over, so the authored draw kinds, cell offsets, SUBST rows and scales go with
// it); an init over either sets the label, the width and the header
// justification, which the cells copy, and keeps the rest of the record.
// [orig: CTableWnd_ResizeColumnCount @0x63f6c0 — the grow path's copy
//  @0x63f724 takes the old count in bytes; CTableWnd_InitRow @0x63f9c0 —
//  +0x80 / +0x84 (-1 -> 1 / 0x10) copied to +0x90 / +0x94 @0x63fbdf..0x63fc03,
//  no write to +108 or +152..+172]
void MenuFrameCompiler::apply_table_column_defs_(int index, WidgetNode &node) const {
	const auto it = table_column_defs_.find(index);
	if (it == table_column_defs_.end() || it->second.empty()) return;
	const std::vector<MenuTableColumnDef> &defs = it->second;
	std::vector<TableColumnSetup> authored;
	authored.swap(node.table_columns);
	node.table_columns.assign(defs.size(), TableColumnSetup{});
	for (size_t c = 0; c < defs.size(); ++c) {
		const MenuTableColumnDef &def = defs[c];
		TableColumnSetup &col = node.table_columns[c];
		if (def.kept && c < authored.size()) col = authored[c];
		if (!def.defined) continue;
		col.width = def.width;
		col.label = def.label;
		col.header_justify = def.justify == -1 ? 1 : def.justify;
		col.header_vjustify = def.vjustify == -1 ? 16 : def.vjustify;
		col.body_justify = col.header_justify;
		col.body_vjustify = col.header_vjustify;
	}
}

void MenuFrameCompiler::set_table_cell_painter(int index, MenuTableCellPainter painter) {
	if (painter)
		table_painters_[index] = std::move(painter);
	else
		table_painters_.erase(index);
}

// CTableWnd_HitTest on the design point: a miss of the table is no row and no
// column. Above the header's bottom the column under x (from the table's left,
// SPACING between; a column whose right edge passes the table's fails the
// test); below it the row is the offset over the pitch, clamped to the last
// visible row, mapped past the scroll offset over the rows that are not
// hidden (a row past the last fails), and the column is the one whose span
// holds x (-1 when none; a width-0 column holds nothing but still steps
// SPACING). The design point is the raw point over the menu scale, truncated.
// [orig: CTableWnd_HitTest @ 0x63fe90; UI_DispatchMouseEvent @ 0x63ab00]
bool MenuFrameCompiler::table_hit(int index, const MenuFrameState &state, float mx, float my,
		float sx, float sy, int *out_row, int *out_column) const {
	*out_row = -1;
	*out_column = -1;
	if (index < 0 || index >= static_cast<int>(nodes_.size())) return false;
	const WidgetNode &node = nodes_[static_cast<size_t>(index)];
	if (node.window == nullptr || node.window->type != mnu::WindowType::Table) return false;
	mnu::RectEdges rect;
	if (!widget_rect(index, state, &rect)) return false;
	const MenuWidgetState *ws = state_for(state, index);
	const int px = sx > 0.0f ? static_cast<int>(mx / sx) : 0;
	const int py = sy > 0.0f ? static_cast<int>(my / sy) : 0;
	int header_h = 0;
	int row_h = 0;
	table_row_heights_(node, &header_h, &row_h);
	if (row_h <= 0) return false;
	const int pitch = table_pitch_(node, rect, row_h);
	// CWnd_HitTestPoint: outside the widget -> S_OK with nothing.
	if (px < rect.left || px >= rect.right || py < rect.top || py >= rect.bottom) return true;
	const int visible = table_visible_rows_(node, rect);
	int row = -1;
	if (py < rect.top + header_h) {
		int left = rect.left;
		for (size_t c = 0; c < node.table_columns.size(); ++c) {
			const int right = left + node.table_columns[c].width;
			if (right > rect.right) return false;
			if (px - left >= 0 && px - right < 0) {
				*out_column = static_cast<int>(c);
				return true;
			}
			left = right + node.table_spacing;
		}
	} else {
		row = (py - rect.top - header_h) / pitch;
		if (row > visible - 1) row = visible - 1;
	}
	// Map the visible index past the scroll offset over the live rows.
	const int first = ws != nullptr && ws->scroll_row > 0 ? ws->scroll_row : 0;
	const int count = ws != nullptr ? static_cast<int>(ws->table_rows.size()) : 0;
	int visible_index = -1 - first;
	int data_row = 0;
	for (; data_row < count; ++data_row) {
		if (!ws->table_rows[static_cast<size_t>(data_row)].hidden()) ++visible_index;
		if (visible_index == row) break;
	}
	if (data_row >= count) return false;
	*out_row = data_row;
	int left = rect.left;
	for (size_t c = 0; c < node.table_columns.size(); ++c) {
		const int right = left + node.table_columns[c].width;
		if (px - left >= 0 && px - right < 0) {
			*out_column = static_cast<int>(c);
			break;
		}
		left = right + node.table_spacing;
	}
	return true;
}

// CTableWnd_CalculateAlignedTextRect: the whole text's measure aligned in
// the cell (vertical 0 top, 16 centre, 32 bottom; horizontal 0 left, 1
// centre, 2 right, each centring by a halved difference), then drawn at the
// aligned corner plus the cell offset through the wrapped drawer's 0x20000
// mode against the cell's width: a line breaks at its last space (a space at
// the line's first character counts as none) or before the overflowing
// character, and the rest of the source line up to its LF is dropped.
// [orig: CTableWnd_CalculateAlignedTextRect @ 0x63ec50; CFontCache_DrawTextWrapped
//  @ 0x653710 in mode 0x20000 (docs/mnu/menu-re.md "the wrapped drawer")]
void MenuFrameCompiler::emit_table_text_(const WidgetNode &node, const std::string &text,
		int align, int dx, int dy, const mnu::RectEdges &cell, const WalkScale &s,
		uint32_t color) {
	if (text.empty()) return;
	int tw = 0;
	int th = 0;
	measure_text(node, text, &tw, &th);
	const int cw = cell.right - cell.left;
	const int ch = cell.bottom - cell.top;
	int left = cell.left;
	int top = cell.top;
	const int v = align & 0xF0;
	if (v == 16) {
		top += (ch - th) >> 1;
	} else if (v == 32) {
		top += ch - th;
	}
	const int h = align & 0xF;
	if (h == 1) {
		left += (cw - tw) >> 1;
	} else if (h == 2) {
		left += cw - tw;
	}
	const opennova::fnt::fnt_font_t *font = font_for(node);
	if (font == nullptr) return;
	hud::GameFont gf;
	gf.set_font(font);
	const int threshold = static_cast<int>(static_cast<float>(cw) * s.x);
	const int len = static_cast<int>(text.size());
	int y = top + dy;
	int line_start = 0;
	while (line_start <= len) {
		int last_space = 0;
		int i = line_start;
		int break_at = len;
		for (; i < len; ++i) {
			const char c = text[static_cast<size_t>(i)];
			if (c == '\n') {
				break_at = i;
				break;
			}
			if (c == ' ') last_space = i;
			int aw = 0;
			int ah = 0;
			gf.measure(text.substr(static_cast<size_t>(line_start),
								static_cast<size_t>(i - line_start) + 1)
							.c_str(),
					s.x, s.y, &aw, &ah);
			if (aw > threshold) {
				break_at = last_space != 0 ? last_space : i;
				break;
			}
		}
		const std::string line =
				text.substr(static_cast<size_t>(line_start), static_cast<size_t>(break_at - line_start));
		if (!line.empty()) emit_glyph_run(node, line, left + dx, y, s, color, -1);
		// Drop the rest of the source line up to its LF.
		int next = break_at;
		while (next < len && text[static_cast<size_t>(next)] != '\n') ++next;
		if (next >= len) return;
		int lw = 0;
		int lh = 0;
		gf.measure(line.empty() ? "W" : line.c_str(), 1.0f, 1.0f, &lw, &lh);
		y += lh;
		line_start = next + 1;
	}
}

// CTableWnd_DrawAlignedTexture: the texture at its native size aligned in the
// cell, or with SCALE_BITMAP fitted — the square the witnessed arithmetic
// makes (a cell taller than wide takes the cell's width as the height and
// keeps the cell's width, a wider cell takes the cell's height as the width
// and keeps the cell's height) — then moved by the cell offset (left/top
// only); drawn through the 0x7F7F7F modulate-2x material, full colour.
// [orig: CTableWnd_DrawAlignedTexture @ 0x6409e0 — the fitted height
//  `tex_h * (w / tex_h)` @0x640a59..0x640a6a, the fitted width
//  `tex_w * (h / tex_w)` @0x640aef..0x640b00, the offsets @0x640b38..0x640b44,
//  the 0xFF7F7F7F draw @0x640bb1]
void MenuFrameCompiler::emit_table_texture_(int32_t texture, int align, int dx, int dy,
		bool scale, const mnu::RectEdges &cell, const WalkScale &s) {
	if (texture < 0) return;
	const int rw = cell.right - cell.left;
	const int rh = cell.bottom - cell.top;
	if (rw == 0 || rh == 0) return;
	const std::pair<int, int> size = texture_sizes_[static_cast<size_t>(texture)];
	int th = size.second;
	int ew = size.first;
	mnu::RectEdges dst = cell;
	bool align_v = true;
	if (scale) {
		if (rh < rw)
			align_v = false;
		else
			th = rw;
	}
	if (align_v) {
		const int v = align & 0xF0;
		if (v == 16) {
			dst.top = ((dst.bottom - dst.top) >> 1) - (th >> 1) + dst.top;
		} else if (v == 32) {
			dst.top = dst.bottom - th;
		}
		dst.bottom = dst.top + th;
	}
	bool align_h = true;
	if (scale) {
		if (rh <= rw)
			ew = rh;
		else
			align_h = false;
	}
	if (align_h) {
		const int h = align & 0xF;
		if (h == 1) {
			dst.left = ((dst.right - dst.left) >> 1) - (ew >> 1) + dst.left;
		} else if (h == 2) {
			dst.left = dst.right - ew;
		}
		dst.right = dst.left + ew;
	}
	dst.left += dx;
	dst.top += dy;
	emit_rect_quad(dst, s, 0xFFFFFFFFu, texture, false, 1.0f, 1.0f);
}

// The row's text colour: the widget's colours with the row colour swapped
// into the row state's slot (flag bit 2), then state 1's slot for a flag-bit-0
// row, else the row state's.
// [orig: CUITable_Render @ 0x6416c9..0x64170c, the text state
//  @ 0x64189a..0x6418da; CTableWnd_DrawCell @ 0x640e46..0x640e79,
//  @ 0x64103a..0x641048]
uint32_t MenuFrameCompiler::table_row_color_(const WidgetNode &node, const MenuTableRow &row) {
	uint32_t colors[4];
	for (int i = 0; i < 4; ++i) colors[i] = node.colors[i];
	if ((row.flags & kTableRowFlagColor) != 0 && row.state >= 0 && row.state < 4)
		colors[row.state] = row.color;
	const int text_state = (row.flags & kTableRowFlagState1Color) != 0 ? 1 : row.state;
	return text_state >= 0 && text_state < 4 ? colors[text_state] : colors[0];
}

// The custom-draw handler's canvas over one table's walk.
class MenuFrameCompiler::TableCanvas : public MenuTableCellCanvas {
public:
	TableCanvas(MenuFrameCompiler &compiler, int index, const WidgetNode &node,
			const mnu::RectEdges &rect, const WalkScale &s, const MenuWidgetState *ws)
			: compiler_(compiler), index_(index), node_(node), rect_(rect), s_(s), ws_(ws) {}

	void draw_cell(int row, int column, int flags) override {
		compiler_.emit_table_cell_(index_, node_, rect_, s_, ws_, row, column, flags);
	}
	// A device clear writes the colour's RGB outright (the target's alpha is
	// not blended), so the fill is opaque.
	void clear_rect(uint32_t argb, float left, float top, float right, float bottom) override {
		MenuQuad quad;
		quad.x0 = left;
		quad.y0 = top;
		quad.x1 = right;
		quad.y1 = bottom;
		quad.color = 0xFF000000u | (argb & 0x00FFFFFFu);
		compiler_.push_quad(quad);
	}
	void line(uint32_t argb, float x0, float y0, float x1, float y1) override {
		compiler_.push_line({ x0, y0, x1, y1, argb });
	}

private:
	MenuFrameCompiler &compiler_;
	int index_;
	const WidgetNode &node_;
	mnu::RectEdges rect_;
	WalkScale s_;
	const MenuWidgetState *ws_;
};

// CTableWnd_DrawCell(row, column, flags): the cell rect by its OWN walk — the
// column's left from the table's left over every earlier column's width plus
// SPACING (width-0 columns included, no wrap); the header's band for row -1;
// a data row starts at the table's TOP and moves one body row height for each
// row that is not hidden and sits past the first visible one (the header
// height term is taken only at row index 0 past the first visible row, which
// never occurs, so the drawn row lands one header height above the row
// CUITable_Render lays out). Flags 1: the row state's ITEMS appearance (COLOR,
// IMAGE, OUTLINE) in that rect. Flags 2: the content — for a custom column
// the image when the row has one, else the text; for the header the label
// with state 0's colour.
// [orig: CTableWnd_DrawCell @ 0x640be0 — the column walk @0x640c94..0x640cb5,
//  the row walk @0x640ce0..0x640d4c (`cmp eax, [esi+324h]; jle` @0x640d10, the
//  row-0 header term @0x640d12..0x640d1a), flags 1 @0x640d50..0x640de4, flags 2
//  @0x640de9..0x641085]
void MenuFrameCompiler::emit_table_cell_(int index, const WidgetNode &node,
		const mnu::RectEdges &rect, const WalkScale &s, const MenuWidgetState *ws, int row,
		int column, int flags) {
	(void)index;
	if (column < 0 || column >= static_cast<int>(node.table_columns.size())) return;
	const TableColumnSetup &col = node.table_columns[static_cast<size_t>(column)];
	if (col.width == 0) return;
	int header_h = 0;
	int row_h = 0;
	table_row_heights_(node, &header_h, &row_h);
	mnu::RectEdges cell = rect;
	cell.bottom = cell.top + header_h;
	for (int c = 0; c < column; ++c)
		cell.left += node.table_spacing + node.table_columns[static_cast<size_t>(c)].width;
	cell.right = cell.left + col.width;
	const int count = ws != nullptr ? static_cast<int>(ws->table_rows.size()) : 0;
	if (row >= 0) {
		const int first = ws != nullptr && ws->scroll_row > 0 ? ws->scroll_row : 0;
		const int visible = table_visible_rows_(node, rect);
		int counter = 0;
		for (int i = 0; i <= row; ++i) {
			if (counter >= first + visible) break;
			if (i < count && !ws->table_rows[static_cast<size_t>(i)].hidden()) {
				if (counter > first) {
					const int dy = i != 0 ? row_h : header_h;
					cell.top += dy;
					cell.bottom += dy;
				}
				++counter;
			}
		}
		cell.bottom = cell.top + row_h;
	}
	const MenuTableRow *r =
			row >= 0 && row < count ? &ws->table_rows[static_cast<size_t>(row)] : nullptr;
	if ((flags & 1) != 0 && r != nullptr && r->state >= 0 && r->state < 4)
		emit_state_pass(cell, s, node.items_states[r->state]);
	if ((flags & 2) == 0) return;
	if (row < 0) {
		// The header: a custom column resolves to type 0 here, so types 0 / 2
		// draw the label, type 4 the label when it has one, type 1 nothing
		// [orig: @0x640e0b..0x640e2e, @0x640e9d..0x640ea8, @0x640f50, @0x640fcb].
		if (col.cell_type == kTableCellImage) return;
		emit_table_text_(node, col.label, col.header_justify | col.header_vjustify, 0, 0, cell,
				s, node.colors[kStateDefault]);
		return;
	}
	if (r == nullptr) return;
	const std::string &text = r->cell(column);
	int kind = col.cell_type;
	const int32_t image = table_cell_image_(node, column, text);
	if (kind == kTableCellCustom) kind = image >= 0 ? kTableCellImage : kTableCellText;
	const int align = col.body_justify | col.body_vjustify;
	if (kind == kTableCellImage || kind == kTableCellImageText) {
		if (image >= 0) {
			emit_table_texture_(image, align, col.body_x, col.body_y, col.scale_bitmap, cell, s);
			return;
		}
		if (kind == kTableCellImage) return;
	}
	emit_table_text_(node, text, align, col.body_x, col.body_y, cell, s, table_row_color_(node, *r));
}

// The witnessed table interior [orig: CUITable_Render @ 0x6411d0;
// docs/mnu/menu-re.md "Table render"]. Header: per column from the table's
// left, a width-0 column skipped with no pen advance, a column whose right
// edge passes the table's neither drawn nor advanced; types 0/1/4 draw the
// label aligned in the header cell with state 0's colour (the rule string
// beside it is the sort indicator [orig: CTableWnd_DrawRuleLine @ 0x6410a0],
// which nothing sets here, so an unsorted column draws the two-space rule:
// nothing); type 2 raises the custom-draw
// event. Data rows: the scroll window is the first visible row plus the
// visible count, hidden rows skipped; each row starts at the table's left and
// a column whose right edge would pass the table's wraps the row down a row
// height first. Per cell by type: the row state's ITEMS passes behind every
// non-custom cell, then 0 the text, 1 the SUBST image, 2 the custom-draw
// event, 4 the image else the text.
void MenuFrameCompiler::emit_table(int index, const WidgetNode &node,
		const mnu::RectEdges &rect, const WalkScale &s,
		const MenuWidgetState *ws) {
	int header_h = 0;
	int row_h = 0;
	table_row_heights_(node, &header_h, &row_h);
	const auto painter_it = table_painters_.find(index);
	const MenuTableCellPainter *painter =
			painter_it != table_painters_.end() ? &painter_it->second : nullptr;
	TableCanvas canvas(*this, index, node, rect, s, ws);
	auto raise_custom = [&](int row, int column, int32_t state, int32_t value,
								const mnu::RectEdges &cell) {
		if (painter == nullptr) return;
		MenuTableCellEvent event;
		event.row = row;
		event.column = column;
		event.state = state;
		event.value = value;
		event.left = emit_x(cell.left, s.x);
		event.top = emit_x(cell.top, s.y);
		event.right = emit_x(cell.right, s.x);
		event.bottom = emit_x(cell.bottom, s.y);
		(*painter)(event, canvas);
	};
	mnu::RectEdges cell = rect;
	cell.bottom = rect.top + header_h;
	int left = rect.left;
	for (size_t c = 0; c < node.table_columns.size(); ++c) {
		const TableColumnSetup &col = node.table_columns[c];
		if (col.width == 0) continue;
		cell.left = left;
		cell.right = left + col.width;
		if (cell.right > rect.right) continue;
		if (col.cell_type == kTableCellCustom) {
			raise_custom(-1, static_cast<int>(c), 0, 0, cell);
		} else {
			emit_table_text_(node, col.label, col.header_justify | col.header_vjustify, 0, 0, cell,
					s, node.colors[kStateDefault]);
		}
		left = cell.right + node.table_spacing;
	}
	if (ws == nullptr || ws->table_rows.empty() || row_h <= 0) return;
	const int first = ws->scroll_row > 0 ? ws->scroll_row : 0;
	const int visible = table_visible_rows_(node, rect);
	cell.top = rect.top + header_h;
	cell.bottom = cell.top + row_h;
	int visible_index = 0;
	for (size_t r = 0; r < ws->table_rows.size(); ++r) {
		if (visible_index >= first + visible) break;
		const MenuTableRow &row = ws->table_rows[r];
		if (row.hidden()) continue;
		if (visible_index >= first) {
			left = rect.left;
			for (size_t c = 0; c < node.table_columns.size(); ++c) {
				const TableColumnSetup &col = node.table_columns[c];
				if (col.width == 0) continue;
				if (left + col.width > rect.right) {
					cell.top += row_h;
					cell.bottom += row_h;
					left = rect.left;
				}
				cell.left = left;
				cell.right = left + col.width;
				// The row state's ITEMS passes behind a non-custom cell
				// [orig: @ 0x641642..0x6416b6].
				if (col.cell_type != kTableCellCustom && row.state >= 0 && row.state < 4)
					emit_state_pass(cell, s, node.items_states[row.state]);
				const std::string &text = row.cell(static_cast<int>(c));
				const int align = col.body_justify | col.body_vjustify;
				switch (col.cell_type) {
					case kTableCellText:
						emit_table_text_(node, text, align, col.body_x, col.body_y, cell, s,
								table_row_color_(node, row));
						break;
					case kTableCellImage:
						emit_table_texture_(table_cell_image_(node, static_cast<int>(c), text),
								align, col.body_x, col.body_y, col.scale_bitmap, cell, s);
						break;
					case kTableCellCustom:
						raise_custom(static_cast<int>(r), static_cast<int>(c), row.state,
								row.value(static_cast<int>(c)), cell);
						break;
					case kTableCellImageText: {
						const int32_t image = table_cell_image_(node, static_cast<int>(c), text);
						if (image >= 0)
							emit_table_texture_(image, align, col.body_x, col.body_y,
									col.scale_bitmap, cell, s);
						else
							emit_table_text_(node, text, align, col.body_x, col.body_y, cell, s,
									table_row_color_(node, row));
						break;
					}
					default:
						break;
				}
				left = cell.right + node.table_spacing;
			}
			cell.top += row_h;
			cell.bottom += row_h;
		}
		++visible_index;
	}
}

} // namespace opennova::menu
