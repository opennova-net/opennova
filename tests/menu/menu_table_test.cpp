// The TABLE widget layer of the menu frame compiler: the SUBST images beside a
// matching cell text, the CUSTOM_DRAW cells' custom-draw event and the cell
// draw a handler calls back into (with its own row walk), the cell text
// alignment, the row colour override, hidden rows, the hit test's row and
// column, and a widget's clip viewport.
// [orig: CUITable_Render @ 0x6411d0; CTableWnd_DrawCell @ 0x640be0;
//  CTableWnd_SetCellText @ 0x63edf0; CTableWnd_CalculateAlignedTextRect
//  @ 0x63ec50; CTableWnd_DrawAlignedTexture @ 0x6409e0; CTableWnd_HitTest
//  @ 0x63fe90; CWnd_ApplyClipViewport @ 0x6472a0]
// Witness record: docs/mnu/menu-re.md "Table render".

#include <runtime/menu/menu_frame.h>
#include <formats/mnu/mnu.h>

#include <cstdio>
#include <string>
#include <vector>

#include "common/test_font.h"

using namespace opennova::fnt;
using namespace opennova::menu;

namespace {

// A text colour as the menus' text sink submits it: the RGB halved on a
// modulate-2x device, which the font page's MODULATE2X doubles back on the
// device (D-HUD-51) [orig: CFontCache_DrawTextScaled @0x6531e7..0x6531eb].
constexpr uint32_t text_rgb(uint32_t rgb) {
	return (rgb >> 1) & 0x7F7F7Fu;
}

int failures = 0;

#define CHECK(cond, msg)                                                       \
	do {                                                                       \
		if (!(cond)) {                                                         \
			std::fprintf(stderr, "FAIL: %s\n", msg);                           \
			++failures;                                                        \
		}                                                                      \
	} while (0)

opennova::mnu::Document parse_or_die(const char *xml) {
	opennova::mnu::Document doc;
	std::string err;
	if (!opennova::mnu::parse(std::string(xml), doc, err)) {
		std::fprintf(stderr, "FAIL: fixture parse: %s\n", err.c_str());
		++failures;
	}
	return doc;
}

int32_t slot_of(const MenuFrameCompiler &c, const char *name) {
	const auto &names = c.texture_names();
	for (size_t i = 0; i < names.size(); ++i)
		if (names[i] == name) return static_cast<int32_t>(i);
	return kMenuTexNone;
}

MenuTableRow row_of(std::initializer_list<std::string> cells, int32_t value0 = 0) {
	MenuTableRow row;
	row.cells.assign(cells.begin(), cells.end());
	row.values.push_back(value0);
	return row;
}

// A cmap.mnu-shaped table: a scaled checkbox column (CUSTOM_DRAW first, so
// custom drawn), a plain BITMAP_DRAW checkbox column, a centred text column.
const char *kTableXml = R"(
<SCREEN>
  <NAME>TBL</NAME>
  <WINDOW type="window" name="ROOT">
    <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>
    <FONT><NAME>f.fnt</NAME><DEFAULT_FG>AAAAAA</DEFAULT_FG><DISABLED_FG>555555</DISABLED_FG><SELECTED_FG>00FF00</SELECTED_FG></FONT>
    <WINDOW type="window" name="TAB">
      <POSITION><LEFT>20</LEFT><TOP>60</TOP></POSITION>
      <WINDOW type="table" name="LIST">
        <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>300</RIGHT><BOTTOM>200</BOTTOM></POSITION>
        <COLUMN count="4" spacing="0">
          <HEADER justify="CENTER" vjustify="CENTER" column="0" width="50" type="id">Recruit</HEADER>
          <HEADER justify="CENTER" vjustify="CENTER" column="1" width="50">Box</HEADER>
          <HEADER justify="CENTER" vjustify="CENTER" column="2" width="150">Name</HEADER>
          <BODY justify="CENTER" vjustify="CENTER" column="0" CUSTOM_DRAW BITMAP_DRAW SCALE_BITMAP></BODY>
          <BODY justify="CENTER" vjustify="CENTER" column="1" BITMAP_DRAW></BODY>
          <BODY justify="RIGHT" vjustify="TOP" column="2"></BODY>
          <SUBST column="0" value="0" FILE>alphachk0.tga</SUBST>
          <SUBST column="0" value="1" FILE>alphachk1.tga</SUBST>
          <SUBST column="1" value="1" FILE>alphachk1.tga</SUBST>
        </COLUMN>
        <ITEMS MULTISELECT>
          <APPEARANCE type="outline" state="default">445566</APPEARANCE>
          <APPEARANCE type="color" state="selected">112233</APPEARANCE>
        </ITEMS>
        <MIN_ITEM_HEIGHT>20</MIN_ITEM_HEIGHT>
      </WINDOW>
    </WINDOW>
  </WINDOW>
</SCREEN>
)";

struct RecordingCanvas : MenuTableCellCanvas {
	struct Call {
		char kind;
		int row, column, flags;
		uint32_t argb;
		float a, b, c, d;
	};
	std::vector<Call> calls;
	MenuTableCellCanvas *inner = nullptr;
	void draw_cell(int row, int column, int flags) override {
		calls.push_back({'d', row, column, flags, 0, 0, 0, 0, 0});
		if (inner != nullptr) inner->draw_cell(row, column, flags);
	}
	void clear_rect(uint32_t argb, float l, float t, float r, float b) override {
		calls.push_back({'c', 0, 0, 0, argb, l, t, r, b});
		if (inner != nullptr) inner->clear_rect(argb, l, t, r, b);
	}
	void line(uint32_t argb, float x0, float y0, float x1, float y1) override {
		calls.push_back({'l', 0, 0, 0, argb, x0, y0, x1, y1});
		if (inner != nullptr) inner->line(argb, x0, y0, x1, y1);
	}
};

// SUBST: a cell text matching a FILE row draws that image, scaled to the
// cell's height and centred (the square the SCALE_BITMAP arithmetic makes);
// an unmatched text in an image column draws nothing; the custom column
// raises its event for the header and each row with the device rect, the
// row state and the column's cell value, and draws nothing itself.
void test_subst_images_and_custom_events(const fnt_font_t *font) {
	opennova::mnu::Document doc = parse_or_die(kTableXml);
	MenuFrameCompiler c;
	c.register_font("f.fnt", font);
	c.configure(doc.first_screen());
	const int32_t chk0 = slot_of(c, "alphachk0.tga");
	const int32_t chk1 = slot_of(c, "alphachk1.tga");
	CHECK(chk0 >= 0 && chk1 >= 0, "configure interns the SUBST FILE images");
	c.set_texture_size(chk0, 16, 16);
	c.set_texture_size(chk1, 16, 16);
	const int table = c.widget_index("LIST");
	std::vector<MenuTableCellEvent> events;
	c.set_table_cell_painter(table, [&](const MenuTableCellEvent &e, MenuTableCellCanvas &) {
		events.push_back(e);
	});
	MenuWidgetState ws;
	ws.index = table;
	ws.table_rows.push_back(row_of({"0", "1", "alpha"}, 7));
	ws.table_rows.push_back(row_of({"1", "-", "beta"}, 8));
	ws.table_rows[1].state = kTableRowSelected;
	MenuFrameState state;
	state.widgets.push_back(ws);
	const MenuDrawList &dl = c.compile(state, 2.0f, 2.0f);
	// Header 16 px (the font's "W"), rows 20 px, the table at (20, 60).
	CHECK(events.size() == 3, "one custom event for the header and one per row");
	if (events.size() == 3) {
		CHECK(events[0].row == -1 && events[0].column == 0 && events[0].state == 0 &&
						events[0].value == 0,
				"the header event is row -1, state 0, value 0");
		CHECK(events[0].left == 40.0f && events[0].top == 120.0f && events[0].right == 140.0f &&
						events[0].bottom == 152.0f,
				"the header event's rect is the scaled header cell");
		CHECK(events[1].row == 0 && events[1].state == 0 && events[1].value == 7 &&
						events[1].top == 152.0f && events[1].bottom == 192.0f,
				"a body event carries the row, its state, the cell value and the body cell");
		CHECK(events[2].row == 1 && events[2].state == kTableRowSelected && events[2].value == 8,
				"the selected row's event carries state 3");
	}
	// Column 1 (plain BITMAP_DRAW, unscaled): row 0's "1" draws alphachk1
	// at native 16x16 centred in the 50x20 cell; row 1's "-" draws nothing.
	int chk1_quads = 0;
	bool native_centred = false;
	for (const MenuQuad &q : dl.quads) {
		if (q.texture != chk1) continue;
		++chk1_quads;
		// cell x 70..120, y 76..96 (design) -> 16x16 at (87, 78) -> x2.
		if (q.x0 == 174.0f && q.x1 == 206.0f && q.y0 == 156.0f && q.y1 == 188.0f)
			native_centred = true;
	}
	CHECK(chk1_quads == 1, "only the matching SUBST text draws an image");
	CHECK(native_centred, "an unscaled SUBST image draws native size, centred");
	CHECK(slot_of(c, "alphachk0.tga") >= 0, "the unused image stays interned");
}

// A column code set up over the record a count kept is the authored column with the
// init's label, width and justification: its BITMAP_DRAW cell type, its SUBST rows
// and its offsets stay, so a matching cell still draws its image, centred in the
// init's width. Over a record a growing count started over it is a text column with
// no SUBST rows. [orig: CTableWnd_InitRow @0x63f9c0 — no write to +108 or
// +152..+172; CTableWnd_ResizeColumnCount @0x63f6c0 — the grow path's copy @0x63f724]
void test_code_columns_over_kept_and_new_records(const fnt_font_t *font) {
	opennova::mnu::Document doc = parse_or_die(kTableXml);
	MenuFrameCompiler c;
	c.register_font("f.fnt", font);
	c.configure(doc.first_screen());
	const int32_t chk1 = slot_of(c, "alphachk1.tga");
	c.set_texture_size(chk1, 16, 16);
	const int table = c.widget_index("LIST");
	MenuTableColumn init;
	init.label = "Tick";
	init.width = 100;
	init.justify = init.body_justify = 1;
	init.vjustify = init.body_vjustify = 16;
	MenuWidgetState ws;
	ws.index = table;
	ws.table_rows.push_back(row_of({"", "1", "x"}));
	ws.has_table_columns = true;
	for (int i = 0; i < 4; ++i) {
		MenuTableColumn kept;
		kept.kept = true;
		kept.defined = false;
		ws.table_columns.push_back(kept);
	}
	ws.table_columns[1] = init;
	ws.table_columns[1].kept = true;
	MenuFrameState state;
	state.widgets.push_back(ws);
	// Column 1 now x 70..170 (the init's width); row 0 y 76..96: the native 16x16
	// image centred at (112, 78).
	bool kept_image = false;
	for (const MenuQuad &q : c.compile(state, 1.0f, 1.0f).quads)
		if (q.texture == chk1 && q.x0 == 112.0f && q.x1 == 128.0f && q.y0 == 78.0f && q.y1 == 94.0f)
			kept_image = true;
	CHECK(kept_image, "an init over a kept record keeps its BITMAP_DRAW and SUBST rows at the init's width");
	// Five records: the count grew, every record started over.
	MenuTableColumn zeroed;
	zeroed.justify = zeroed.vjustify = zeroed.body_justify = zeroed.body_vjustify = 0;
	zeroed.ascending = false;
	zeroed.defined = false;
	state.widgets[0].table_columns.assign(5, zeroed);
	state.widgets[0].table_columns[0] = init;
	state.widgets[0].table_columns[0].width = 50;
	state.widgets[0].table_columns[1] = init;
	const MenuDrawList &grown = c.compile(state, 1.0f, 1.0f);
	bool image = false;
	for (const MenuQuad &q : grown.quads)
		if (q.texture == chk1) image = true;
	bool text = false;
	for (const auto &g : grown.glyphs)
		if (g.y_top >= 76.0f && g.y_top < 96.0f && g.x_top_left >= 70.0f && g.x_top_left < 170.0f) text = true;
	CHECK(!image && text, "an init over a record the count started over is a text column");
}

// The custom-draw handler's cell draw: flags 1 the row state's ITEMS pass,
// flags 2 the content, at CTableWnd_DrawCell's OWN rect — the first visible
// row at the table's top (one header height above the row the render lays
// out), the header's label in the header band.
void test_draw_cell_rect_and_passes(const fnt_font_t *font) {
	opennova::mnu::Document doc = parse_or_die(kTableXml);
	MenuFrameCompiler c;
	c.register_font("f.fnt", font);
	c.configure(doc.first_screen());
	const int32_t chk1 = slot_of(c, "alphachk1.tga");
	c.set_texture_size(chk1, 16, 16);
	const int table = c.widget_index("LIST");
	c.set_table_cell_painter(table, [&](const MenuTableCellEvent &e, MenuTableCellCanvas &canvas) {
		if (e.row < 0) return;
		canvas.draw_cell(e.row, e.column, 1);
		canvas.draw_cell(e.row, e.column, 2);
	});
	MenuWidgetState ws;
	ws.index = table;
	ws.table_rows.push_back(row_of({"1", "", ""}));
	ws.table_rows.push_back(row_of({"-", "", ""}));
	MenuFrameState state;
	state.widgets.push_back(ws);
	const MenuDrawList &dl = c.compile(state, 1.0f, 1.0f);
	// Row 0's custom cell: the outline grid (ITEMS default) and the scaled
	// checkbox, at the table's top (y 60..80), not the body row (76..96).
	bool outline_at_top = false;
	for (const MenuLine &l : dl.lines)
		if ((l.color & 0xFFFFFFu) == 0x445566u && l.y0 == 60.0f && l.x0 == 20.0f)
			outline_at_top = true;
	CHECK(outline_at_top, "draw_cell's appearance pass lands at the table's top for the first row");
	bool image_at_top = false;
	for (const MenuQuad &q : dl.quads)
		if (q.texture == chk1 && q.y0 == 60.0f && q.y1 == 80.0f && q.x0 == 35.0f && q.x1 == 55.0f)
			image_at_top = true;
	CHECK(image_at_top, "the scaled SUBST image fills the cell height, centred, at the quirk rect");
	// Row 1's "-" (no SUBST row) draws as text in the second band (y 80..100).
	bool dash_text = false;
	for (const auto &g : dl.glyphs)
		if (g.y_top >= 80.0f && g.y_top < 100.0f && g.x_top_left >= 20.0f && g.x_top_left < 70.0f)
			dash_text = true;
	CHECK(dash_text, "a custom cell without an image draws its text");
}

// The text cells align by the BODY justification; the row colour override
// replaces the row state's colour; a hidden row takes no band.
void test_text_alignment_color_and_hidden_rows(const fnt_font_t *font) {
	opennova::mnu::Document doc = parse_or_die(kTableXml);
	MenuFrameCompiler c;
	c.register_font("f.fnt", font);
	c.configure(doc.first_screen());
	const int table = c.widget_index("LIST");
	MenuWidgetState ws;
	ws.index = table;
	ws.table_rows.push_back(row_of({"", "", "x"}));
	ws.table_rows.push_back(row_of({"", "", "hid"}));
	ws.table_rows[1].flags = 0x2u;
	ws.table_rows.push_back(row_of({"", "", "y"}));
	ws.table_rows[2].flags = kTableRowFlagColor;
	ws.table_rows[2].color = 0xFF123456u;
	MenuFrameState state;
	state.widgets.push_back(ws);
	const MenuDrawList &dl = c.compile(state, 1.0f, 1.0f);
	// Column 2: x 120..270, RIGHT + TOP: "x" ends at the cell's right edge.
	float x_right = 0.0f;
	bool colored = false;
	bool second_band = false;
	for (const auto &g : dl.glyphs) {
		if ((g.color & 0xFFFFFFu) == text_rgb(0x123456u)) {
			colored = true;
			if (g.y_top >= 95.0f && g.y_top < 116.0f) second_band = true;
		}
		if (g.y_top >= 75.0f && g.y_top < 95.0f && g.x_top_right > x_right) x_right = g.x_top_right;
	}
	CHECK(x_right > 250.0f && x_right <= 270.0f, "a RIGHT cell ends at its cell's right edge");
	CHECK(colored, "the row colour override tints the row's text");
	CHECK(second_band, "a hidden row takes no band: the next row draws in the second band");
}

// The hit test: the header gives row -1 and the column, a body point the
// absolute row (hidden rows skipped) and its column.
void test_hit_test(const fnt_font_t *font) {
	opennova::mnu::Document doc = parse_or_die(kTableXml);
	MenuFrameCompiler c;
	c.register_font("f.fnt", font);
	c.configure(doc.first_screen());
	const int table = c.widget_index("LIST");
	MenuWidgetState ws;
	ws.index = table;
	ws.table_rows.push_back(row_of({"a"}));
	ws.table_rows.push_back(row_of({"b"}));
	ws.table_rows[1].flags = 0x8u;
	ws.table_rows.push_back(row_of({"c"}));
	MenuFrameState state;
	state.widgets.push_back(ws);
	c.compile(state, 1.0f, 1.0f);
	int row = 0, col = 0;
	CHECK(c.table_hit(table, state, 90.0f, 65.0f, 1.0f, 1.0f, &row, &col) && row == -1 && col == 1,
			"a header point is row -1 with its column");
	CHECK(c.table_hit(table, state, 150.0f, 100.0f, 1.0f, 1.0f, &row, &col) && row == 2 && col == 2,
			"the second body band maps past the hidden row to row 2");
	CHECK(c.table_hit(table, state, 5.0f, 5.0f, 1.0f, 1.0f, &row, &col) && row == -1 && col == -1,
			"a miss of the table is no row and no column");
	CHECK(!c.table_hit(table, state, 150.0f, 200.0f, 1.0f, 1.0f, &row, &col),
			"a band past the last row fails");
}

// A widget's clip viewport cuts its own image to the scaled clip rect (+1).
void test_clip_viewport(const fnt_font_t *font) {
	const char *xml = R"(
<SCREEN>
  <NAME>CLIP</NAME>
  <WINDOW type="window" name="ROOT">
    <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>
    <WINDOW type="button" name="CLOSE">
      <POSITION><LEFT>90</LEFT><TOP>10</TOP><RIGHT>110</RIGHT><BOTTOM>30</BOTTOM></POSITION>
      <APPEARANCE type="image" state="default">close.tga</APPEARANCE>
    </WINDOW>
  </WINDOW>
</SCREEN>
)";
	opennova::mnu::Document doc = parse_or_die(xml);
	MenuFrameCompiler c;
	c.register_font("f.fnt", font);
	c.configure(doc.first_screen());
	const int32_t tex = slot_of(c, "close.tga");
	c.set_texture_size(tex, 16, 16);
	MenuWidgetState ws;
	ws.index = c.widget_index("CLOSE");
	ws.has_clip = true;
	ws.clip = {0, 0, 99, 100};
	MenuFrameState state;
	state.widgets.push_back(ws);
	const MenuDrawList &dl = c.compile(state, 1.0f, 1.0f);
	bool clipped = false;
	for (const MenuQuad &q : dl.quads)
		if (q.texture == tex && q.x0 == 90.0f && q.x1 == 100.0f && q.u1 == 0.5f) clipped = true;
	CHECK(clipped, "the image is cut at the viewport's right edge (99 + 1) with its UVs");
}

// The custom-draw slot: the slot widget's CUSTOM appearance pass marks the op
// index its handler draws at (after its COLOR pass, before its children); a
// slot before the overlay split pulls the split to it; a hidden slot widget
// marks nothing. [orig: CUIElement_Draw @ 0x64a8a0 — the CUSTOM (&4) pass
// after COLOR / IMAGE / OUTLINE; CMap_OnChatMsgsCustomDraw @ 0x5482d0]
void test_custom_draw_slot(const fnt_font_t *font) {
	const char *xml = R"(
<SCREEN>
  <NAME>SLOT</NAME>
  <WINDOW type="window" name="ROOT">
    <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>
    <WINDOW type="window" name="BEFORE">
      <APPEARANCE type="color" state="default">FF0000</APPEARANCE>
      <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>10</RIGHT><BOTTOM>10</BOTTOM></POSITION>
    </WINDOW>
    <WINDOW type="window" name="CHAT_MSGS">
      <APPEARANCE type="color" state="default">00FF00</APPEARANCE>
      <APPEARANCE type="custom" state="default"></APPEARANCE>
      <POSITION><LEFT>0</LEFT><TOP>20</TOP><RIGHT>10</RIGHT><BOTTOM>30</BOTTOM></POSITION>
      <WINDOW type="window" name="CHILD">
        <APPEARANCE type="color" state="default">0000FF</APPEARANCE>
        <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>5</RIGHT><BOTTOM>5</BOTTOM></POSITION>
      </WINDOW>
    </WINDOW>
  </WINDOW>
</SCREEN>
)";
	opennova::mnu::Document doc = parse_or_die(xml);
	MenuFrameCompiler c;
	c.register_font("f.fnt", font);
	c.configure(doc.first_screen());
	MenuFrameState state;
	state.custom_slot_index = c.widget_index("CHAT_MSGS");
	const MenuDrawList &dl = c.compile(state, 1.0f, 1.0f);
	CHECK(dl.custom_slot_op == 2, "the slot follows BEFORE's fill and its own COLOR pass");
	CHECK(dl.overlay_op_start == dl.custom_slot_op, "a slot before the split pulls the split to it");
	if (dl.custom_slot_op == 2 && dl.draw_ops.size() > 2) {
		const MenuDrawList::DrawOp &op = dl.draw_ops[2];
		CHECK(op.kind == MenuDrawList::DrawOp::Kind::Quad &&
						(dl.quads[static_cast<size_t>(op.index)].color & 0xFFFFFFu) == 0x0000FFu,
				"the child draws after the slot");
	}
	MenuWidgetState hidden;
	hidden.index = state.custom_slot_index;
	hidden.hide = true;
	state.widgets.push_back(hidden);
	const MenuDrawList &dl2 = c.compile(state, 1.0f, 1.0f);
	CHECK(dl2.custom_slot_op == -1, "a hidden slot widget marks nothing");
	state.widgets.clear();
	state.custom_slot_index = -1;
	const MenuDrawList &dl3 = c.compile(state, 1.0f, 1.0f);
	CHECK(dl3.custom_slot_op == -1 && dl3.overlay_op_start == static_cast<int32_t>(dl3.draw_ops.size()),
			"no slot widget, no slot");
}

} // namespace

int main() {
	fnt_font_t font = test_font::uniform_test_font();
	test_subst_images_and_custom_events(&font);
	test_code_columns_over_kept_and_new_records(&font);
	test_draw_cell_rect_and_passes(&font);
	test_text_alignment_color_and_hidden_rows(&font);
	test_hit_test(&font);
	test_clip_viewport(&font);
	test_custom_draw_slot(&font);
	fnt_free(&font);
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("menu_table_test: all checks passed\n");
	return 0;
}
