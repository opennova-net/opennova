// MenuFrameCompiler pins: the witnessed .mnu screen draw walk emits the typed
// draw list — widget draw order, state-driven appearance/color selection, text
// placement, the edit caret, the frame pieces, and the per-element int
// truncation. [orig: CUIElement_Draw @ 0x64a8a0; CStaticWnd_Render @ 0x657b10;
//  CStaticWnd_DrawLabel @ 0x656fb0; draw_text_with_cursor @ 0x6533b0;
//  CEditWnd_Render @ 0x6619e0; CRadioWnd_Render @ 0x656e20; CCheckWnd_Render
//  @ 0x64ae20; CListWnd_DrawItems @ 0x643f30]
// Witness record: docs/mnu/menu-re.md ("Widget render dispatch").

#include <menu/menu_edit.h>
#include <menu/menu_frame.h>
#include <mnu/mnu.h>

#include <cstdio>
#include <cstring>
#include <string>

using opennova::menu::MenuDrawList;
using opennova::menu::MenuFrameCompiler;
using opennova::menu::MenuFrameState;
using opennova::menu::MenuQuad;
using opennova::menu::MenuWidgetState;
using opennova::menu::kMenuTexNone;

namespace {

int failures = 0;

#define CHECK(cond, msg)                                                       \
	do {                                                                       \
		if (!(cond)) {                                                         \
			std::fprintf(stderr, "FAIL: %s\n", msg);                           \
			++failures;                                                        \
		}                                                                      \
	} while (0)

// A synthetic 1-page font: every glyph 8x16 px, spacing 2, design width 800
// (scale 1) — glyph advance 9, measured width strips the trailing pad.
fnt_font_t make_font() {
	fnt_font_t font{};
	fnt_init_blank(&font, 1, 2);
	font.design_width = 800;
	for (uint32_t i = 0; i < FNT_GLYPH_COUNT; ++i) {
		font.glyphs[i].page = 0;
		font.glyphs[i].uv.u0 = 0.0f;
		font.glyphs[i].uv.v0 = 0.0f;
		font.glyphs[i].uv.u1 = 8.0f / 256.0f;
		font.glyphs[i].uv.v1 = 16.0f / 256.0f;
	}
	return font;
}

mnu::Document parse_or_die(const char *xml) {
	mnu::Document doc;
	std::string err;
	if (!mnu::parse(std::string(xml), doc, err)) {
		std::fprintf(stderr, "FAIL: fixture parse: %s\n", err.c_str());
		++failures;
	}
	return doc;
}

int32_t slot_of(const MenuFrameCompiler &c, const char *name) {
	const auto &names = c.texture_names();
	for (size_t i = 0; i < names.size(); ++i) {
		if (names[i] == name) {
			return static_cast<int32_t>(i);
		}
	}
	return kMenuTexNone;
}

int count_quads_with_texture(const MenuDrawList &dl, int32_t slot) {
	int n = 0;
	for (const MenuQuad &q : dl.quads) {
		if (q.texture == slot) {
			++n;
		}
	}
	return n;
}

// --- the shared fixture ------------------------------------------------------

const char *kScreenXml = R"(
<SCREEN>
  <NAME>TEST</NAME>
  <WINDOW type="window" name="MAIN" DRAW_FRAME>
    <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>
    <FONT>
      <NAME>test.fnt</NAME>
      <DEFAULT_FG>AABBCC</DEFAULT_FG>
      <MOUSEOVER_FG>FF0000</MOUSEOVER_FG>
      <SELECTED_FG>00FF00</SELECTED_FG>
      <DISABLED_FG>808080</DISABLED_FG>
    </FONT>
    <FRAME>
      <STENCIL size="8">border.tga</STENCIL>
      <BRUSH>tile.tga</BRUSH>
    </FRAME>
    <APPEARANCE type="color" state="default">102030</APPEARANCE>
    <WINDOW type="button" name="OK">
      <POSITION><LEFT>10</LEFT><TOP>20</TOP><RIGHT>110</RIGHT><BOTTOM>40</BOTTOM></POSITION>
      <APPEARANCE type="image" state="default">ok_idle.tga</APPEARANCE>
      <APPEARANCE type="image" state="mouseover">ok_hover.tga</APPEARANCE>
      <STRING justify="CENTER" vjustify="CENTER">OK</STRING>
    </WINDOW>
    <WINDOW type="static" name="TITLE">
      <POSITION><LEFT>200</LEFT><TOP>50</TOP><RIGHT>400</RIGHT><BOTTOM>70</BOTTOM></POSITION>
      <APPEARANCE type="outline" state="default">FFFFFF</APPEARANCE>
      <STRING justify="LEFT" edge="5">HELLO</STRING>
    </WINDOW>
  </WINDOW>
</SCREEN>
)";

void test_draw_order_and_state_selection(const fnt_font_t *font) {
	mnu::Document doc = parse_or_die(kScreenXml);
	MenuFrameCompiler c;
	c.configure(doc.first_screen(), font);
	const int32_t border = slot_of(c, "border.tga");
	const int32_t brush = slot_of(c, "tile.tga");
	const int32_t ok_idle = slot_of(c, "ok_idle.tga");
	const int32_t ok_hover = slot_of(c, "ok_hover.tga");
	CHECK(border >= 0 && brush >= 0 && ok_idle >= 0 && ok_hover >= 0,
			"configure interns every referenced texture");
	c.set_texture_size(border, 32, 32);
	c.set_texture_size(brush, 64, 64);
	c.set_texture_size(ok_idle, 100, 20);
	c.set_texture_size(ok_hover, 100, 20);

	MenuFrameState state;
	const MenuDrawList &dl = c.compile(state, 1.0f, 1.0f);

	// Container order: appearance (color fill) BEFORE the frame
	// [orig: CUIElement_Draw @ 0x64a8a0], then the children in authored
	// order (button art before the static outline).
	CHECK(dl.widgets_drawn == 3, "three widgets draw");
	CHECK(!dl.quads.empty(), "quads emitted");
	CHECK(dl.quads[0].texture == kMenuTexNone &&
					(dl.quads[0].color & 0xFFFFFFu) == 0x102030u,
			"the root color fill is the first quad");
	// The frame: brush fill + 8 stencil pieces follow the fill.
	CHECK(dl.quads.size() >= 10, "frame quads follow");
	CHECK(dl.quads[1].texture == brush && dl.quads[1].tiled,
			"the frame brush tiles right after the fill");
	int stencil_quads = 0;
	for (size_t i = 2; i < 10; ++i) {
		if (dl.quads[i].texture == border) {
			++stencil_quads;
		}
	}
	CHECK(stencil_quads == 8, "eight stencil border pieces");
	CHECK((dl.quads[1].color & 0xFFFFFFu) == 0x7F7F7Fu,
			"frame quads modulate 0x7F7F7F");
	CHECK(dl.quads[1].u1 > 12.0f && dl.quads[1].v1 > 9.0f,
			"the brush fill carries tile repeat counts");
	// Default state: the idle art draws, the hover art does not.
	CHECK(count_quads_with_texture(dl, ok_idle) == 1,
			"default state draws the default appearance");
	CHECK(count_quads_with_texture(dl, ok_hover) == 0,
			"hover art absent by default");
	// The static outline emits 4 lines.
	CHECK(dl.lines.size() == 4, "the outline draws four 1px lines");
	// Text: both labels emit glyphs with the default color.
	CHECK(!dl.glyphs.empty(), "glyph quads emitted");
	CHECK((dl.glyphs[0].color & 0xFFFFFFu) == 0xAABBCCu,
			"default text color from the inherited FONT");

	// Hover flips the button to the MOUSEOVER appearance and color pair
	// [orig: state 2 -> the mouseover slot + colorSet[2..3]].
	MenuWidgetState hover;
	hover.index = 1; // pre-order: 0=MAIN, 1=OK, 2=TITLE
	hover.hovered = true;
	state.widgets.push_back(hover);
	const MenuDrawList &dl2 = c.compile(state, 1.0f, 1.0f);
	CHECK(count_quads_with_texture(dl2, ok_hover) == 1,
			"hover draws the mouseover appearance");
	CHECK(count_quads_with_texture(dl2, ok_idle) == 0,
			"hover replaces the default appearance");
	bool found_red = false;
	for (const auto &g : dl2.glyphs) {
		if ((g.color & 0xFFFFFFu) == 0xFF0000u) {
			found_red = true;
		}
	}
	CHECK(found_red, "hover text uses the mouseover fg");

	// Pressed (state 3) has no authored appearance: the availability
	// fallback draws the DEFAULT slot [orig: CWnd_SetVisualState @ 0x646340].
	state.widgets[0].hovered = false;
	state.widgets[0].pressed = true;
	const MenuDrawList &dl3 = c.compile(state, 1.0f, 1.0f);
	CHECK(count_quads_with_texture(dl3, ok_idle) == 1,
			"missing selected appearance falls back to default");
	bool found_green = false;
	for (const auto &g : dl3.glyphs) {
		if ((g.color & 0xFFFFFFu) == 0x00FF00u) {
			found_green = true;
		}
	}
	CHECK(found_green, "pressed text still uses the selected fg pair");

	// Disabled (state 1): no disabled appearance -> default art, disabled fg.
	state.widgets[0].pressed = false;
	state.widgets[0].disabled = true;
	const MenuDrawList &dl4 = c.compile(state, 1.0f, 1.0f);
	bool found_gray = false;
	for (const auto &g : dl4.glyphs) {
		if ((g.color & 0xFFFFFFu) == 0x808080u) {
			found_gray = true;
		}
	}
	CHECK(found_gray, "disabled text uses the disabled fg");

	// A hidden widget skips its draw but the sibling still renders.
	MenuWidgetState hide;
	hide.index = 1;
	hide.hide = true;
	MenuFrameState hidden_state;
	hidden_state.widgets.push_back(hide);
	const MenuDrawList &dl5 = c.compile(hidden_state, 1.0f, 1.0f);
	CHECK(dl5.widgets_drawn == 2, "hidden widget drops out of the walk");
	CHECK(count_quads_with_texture(dl5, ok_idle) == 0,
			"hidden widget draws no art");
	CHECK(dl5.lines.size() == 4, "the sibling still draws");
}

void test_text_placement_and_truncation(const fnt_font_t *font) {
	mnu::Document doc = parse_or_die(kScreenXml);
	MenuFrameCompiler c;
	c.configure(doc.first_screen(), font);
	MenuFrameState state;
	const MenuDrawList &dl = c.compile(state, 1.0f, 1.0f);
	// The OK button: rect (10,20)-(110,40) in MAIN at (0,0); "OK" measures
	// 2*9-1 = 17 wide, line height 16. CENTER/CENTER -> x = (100>>1) -
	// (17>>1) + 10 = 52, y = (20>>1) - (16>>1) + 20 = 22. The glyph run
	// starts at the drawer's -0.5 vertex offset.
	CHECK(!dl.glyphs.empty(), "glyphs present");
	CHECK(dl.glyphs[0].x_top_left == 51.5f, "centered text x");
	CHECK(dl.glyphs[0].y_top == 21.5f,
			"centered text y (the drawer's -0.5 vertex offset)");
	// TITLE: LEFT justify with edge 5 -> x = 200 + 5; the run follows OK's
	// two glyphs.
	CHECK(dl.glyphs.size() >= 7, "both labels laid out");
	CHECK(dl.glyphs[2].x_top_left == 204.5f, "left text starts at rect + edge");

	// Truncation: a long value in a 100px-wide button with edge 0 keeps the
	// largest prefix strictly below the span [orig: @ 0x657199].
	MenuWidgetState long_text;
	long_text.index = 1;
	long_text.has_text = true;
	long_text.text = "ABCDEFGHIJKLMNOP"; // 16 glyphs -> 143px, avail 100
	MenuFrameState st2;
	st2.widgets.push_back(long_text);
	const MenuDrawList &dl2 = c.compile(st2, 1.0f, 1.0f);
	// Count glyphs on the OK button's run (the first font run).
	CHECK(!dl2.font_runs.empty(), "font runs recorded");
	// width(n) = 9n-1 >= 100 first at n=12 (107) -> 11 glyphs drawn.
	CHECK(dl2.font_runs[0].count == 11,
			"overflowing text truncates to the fitting prefix");
}

void test_scale_truncation(const fnt_font_t *font) {
	mnu::Document doc = parse_or_die(kScreenXml);
	MenuFrameCompiler c;
	c.configure(doc.first_screen(), font);
	const int32_t ok_idle = slot_of(c, "ok_idle.tga");
	c.set_texture_size(ok_idle, 100, 20);
	MenuFrameState state;
	// 1024x768 -> scale (1.28, 1.28): the OK rect (10,20)-(110,40) scales to
	// trunc(12.8)=12, trunc(25.6)=25, trunc(140.8)=140, trunc(51.2)=51
	// [orig: the per-element int truncation @ 0x647d40].
	const MenuDrawList &dl = c.compile(state, 1.28f, 1.28f);
	const MenuQuad *ok = nullptr;
	for (const MenuQuad &q : dl.quads) {
		if (q.texture == ok_idle) {
			ok = &q;
		}
	}
	CHECK(ok != nullptr, "button art present");
	if (ok != nullptr) {
		CHECK(ok->x0 == 12.0f && ok->y0 == 25.0f && ok->x1 == 140.0f &&
						ok->y1 == 51.0f,
				"scaled rects truncate to int per element");
	}
}

void test_radio_checkbox_forcing(const fnt_font_t *font) {
	const char *xml = R"(
<SCREEN>
  <NAME>T</NAME>
  <WINDOW type="window" name="ROOT">
    <FONT><NAME>f.fnt</NAME><DEFAULT_FG>111111</DEFAULT_FG><SELECTED_FG>00FF00</SELECTED_FG></FONT>
    <WINDOW type="radio" name="R1">
      <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>60</RIGHT><BOTTOM>16</BOTTOM></POSITION>
      <APPEARANCE type="image" state="default">r_off.tga</APPEARANCE>
      <APPEARANCE type="image" state="selected">r_on.tga</APPEARANCE>
      <STRING>R</STRING>
    </WINDOW>
    <WINDOW type="checkbox" name="C1">
      <POSITION><LEFT>0</LEFT><TOP>20</TOP><RIGHT>16</RIGHT><BOTTOM>36</BOTTOM></POSITION>
      <APPEARANCE type="image" state="default">c_off.tga</APPEARANCE>
      <STRING>C</STRING>
    </WINDOW>
  </WINDOW>
</SCREEN>
)";
	mnu::Document doc = parse_or_die(xml);
	MenuFrameCompiler c;
	c.configure(doc.first_screen(), font);
	const int32_t r_off = slot_of(c, "r_off.tga");
	const int32_t r_on = slot_of(c, "r_on.tga");
	const int32_t c_off = slot_of(c, "c_off.tga");
	c.set_texture_size(r_off, 16, 16);
	c.set_texture_size(r_on, 16, 16);
	c.set_texture_size(c_off, 16, 16);

	// Checked radio: state 3 forced for appearance AND label colors
	// [orig: CRadioWnd_Render @ 0x656e20].
	MenuWidgetState radio;
	radio.index = 1;
	radio.has_checked = true;
	radio.checked = true;
	MenuFrameState st;
	st.widgets.push_back(radio);
	// Checked checkbox with NO selected appearance: the direct slot-3 force
	// draws nothing (no fallback) [orig: CCheckWnd_Render @ 0x64ae20].
	MenuWidgetState check;
	check.index = 2;
	check.has_checked = true;
	check.checked = true;
	st.widgets.push_back(check);
	const MenuDrawList &dl = c.compile(st, 1.0f, 1.0f);
	CHECK(count_quads_with_texture(dl, r_on) == 1,
			"checked radio draws the selected art");
	CHECK(count_quads_with_texture(dl, r_off) == 0,
			"checked radio drops the default art");
	bool radio_green = false;
	for (const auto &g : dl.glyphs) {
		if ((g.color & 0xFFFFFFu) == 0x00FF00u) {
			radio_green = true;
			break;
		}
	}
	CHECK(radio_green, "checked radio label uses the selected fg");
	CHECK(count_quads_with_texture(dl, c_off) == 0,
			"checked checkbox with no selected slot draws NO appearance");

	// Unchecked checkbox: default art; the label pins right of the rect + 2
	// [orig: CCheckWnd_DrawLabel @ 0x64aa20].
	MenuFrameState st2;
	const MenuDrawList &dl2 = c.compile(st2, 1.0f, 1.0f);
	CHECK(count_quads_with_texture(dl2, c_off) == 1,
			"unchecked checkbox draws the default art");
	bool label_right = false;
	for (const auto &g : dl2.glyphs) {
		if (g.x_top_left == 17.5f && g.y_top == 19.5f) {
			label_right = true; // rect.right(16) + 2, both axes -0.5 offset
		}
	}
	CHECK(label_right, "checkbox label pins at rect.right + 2");
}

void test_edit_caret(const fnt_font_t *font) {
	const char *xml = R"(
<SCREEN>
  <NAME>T</NAME>
  <WINDOW type="window" name="ROOT">
    <FONT><NAME>f.fnt</NAME><DEFAULT_FG>111111</DEFAULT_FG><MOUSEOVER_FG>222222</MOUSEOVER_FG></FONT>
    <WINDOW type="edit" name="E1">
      <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>200</RIGHT><BOTTOM>16</BOTTOM></POSITION>
      <APPEARANCE type="color" state="default">000000</APPEARANCE>
      <APPEARANCE type="color" state="mouseover">303030</APPEARANCE>
      <STRING justify="LEFT">unused</STRING>
    </WINDOW>
  </WINDOW>
</SCREEN>
)";
	mnu::Document doc = parse_or_die(xml);
	MenuFrameCompiler c;
	c.configure(doc.first_screen(), font);

	MenuWidgetState edit;
	edit.index = 1;
	edit.focused = true;
	edit.has_text = true;
	edit.text = "AB";
	edit.caret = 1;
	MenuFrameState st;
	st.widgets.push_back(edit);
	st.time_ms = 0x300; // blink ON [orig: (tick & 0x3FF) > 0x200]

	const MenuDrawList &dl = c.compile(st, 1.0f, 1.0f);
	// Focus forces the MOUSEOVER appearance [orig: @ 0x661a0d].
	bool hover_fill = false;
	for (const MenuQuad &q : dl.quads) {
		if (q.texture == kMenuTexNone && (q.color & 0xFFFFFFu) == 0x303030u) {
			hover_fill = true;
		}
	}
	CHECK(hover_fill, "focused edit draws the mouseover appearance");
	// 2 text glyphs + the caret underscore.
	CHECK(dl.glyphs.size() == 3, "text plus one caret glyph");
	// The caret after 'A': left-run width 8, plus the (spacing-1)+1 gap = 2
	// for a non-empty left run, plus the same gap again for a mid-string
	// caret [orig: the two gated adds @ 0x6534dc / 0x653562] -> x 12,
	// vertex 11.5.
	CHECK(dl.glyphs[2].x_top_left == 11.5f,
			"caret x after the left run + the two gap terms");
	bool mouseover_text = (dl.glyphs[0].color & 0xFFFFFFu) == 0x222222u;
	CHECK(mouseover_text, "focused edit text uses the mouseover fg");

	// Blink OFF phase: no caret glyph.
	st.time_ms = 0x100;
	const MenuDrawList &dl2 = c.compile(st, 1.0f, 1.0f);
	CHECK(dl2.glyphs.size() == 2, "caret hidden in the blink-off phase");

	// An EMPTY focused edit still blinks the caret at the text anchor
	// [orig: the cursor leg runs for the empty string @ 0x6533b0].
	st.time_ms = 0x300;
	st.widgets[0].text = "";
	st.widgets[0].caret = 0;
	const MenuDrawList &dl_empty = c.compile(st, 1.0f, 1.0f);
	CHECK(dl_empty.glyphs.size() == 1, "empty focused edit draws the caret");
	st.widgets[0].text = "AB";
	st.widgets[0].caret = 1;

	// Unfocused: default appearance, no caret.
	st.widgets[0].focused = false;
	const MenuDrawList &dl3 = c.compile(st, 1.0f, 1.0f);
	bool default_fill = false;
	for (const MenuQuad &q : dl3.quads) {
		if (q.texture == kMenuTexNone && (q.color & 0xFFFFFFu) == 0x000000u) {
			default_fill = true;
		}
	}
	CHECK(default_fill, "unfocused edit draws the default appearance");
	CHECK(dl3.glyphs.size() == 2, "no caret without focus");
}

void test_list_rows_and_item_cell(const fnt_font_t *font) {
	const char *xml = R"(
<SCREEN>
  <NAME>T</NAME>
  <WINDOW type="window" name="ROOT">
    <FONT><NAME>f.fnt</NAME><DEFAULT_FG>111111</DEFAULT_FG><MOUSEOVER_FG>FF0000</MOUSEOVER_FG><SELECTED_FG>00FF00</SELECTED_FG></FONT>
    <WINDOW type="list" name="L1">
      <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>100</RIGHT><BOTTOM>64</BOTTOM></POSITION>
      <ITEMS>
        <APPEARANCE type="color" state="selected">204060</APPEARANCE>
        <ITEM type="ID" value="0">AAA</ITEM>
        <ITEM type="ID" value="1">BBB</ITEM>
        <ITEM type="ID" value="2">CCC</ITEM>
      </ITEMS>
      <LIST_BOX><MIN_ITEM_HEIGHT>20</MIN_ITEM_HEIGHT></LIST_BOX>
    </WINDOW>
    <WINDOW type="spinlist" name="S1">
      <POSITION><LEFT>0</LEFT><TOP>100</TOP><RIGHT>45</RIGHT><BOTTOM>120</BOTTOM></POSITION>
      <ITEMS>
        <ITEM type="color" value="0">FF8040</ITEM>
      </ITEMS>
    </WINDOW>
  </WINDOW>
</SCREEN>
)";
	mnu::Document doc = parse_or_die(xml);
	MenuFrameCompiler c;
	c.configure(doc.first_screen(), font);

	MenuWidgetState list;
	list.index = 1;
	list.selected_item = 1;
	MenuFrameState st;
	st.widgets.push_back(list);
	const MenuDrawList &dl = c.compile(st, 1.0f, 1.0f);
	// The selected row paints the ITEMS selected color into the inflated row
	// rect at row 1 (rows are 20 high) [orig: CListWnd_DrawItems @ 0x643f30].
	bool sel_fill = false;
	for (const MenuQuad &q : dl.quads) {
		if (q.texture == kMenuTexNone && (q.color & 0xFFFFFFu) == 0x204060u &&
				q.x0 == 1.0f && q.y0 == 20.0f && q.x1 == 99.0f &&
				q.y1 == 40.0f) {
			sel_fill = true;
		}
	}
	CHECK(sel_fill, "selected row fills the ITEMS selected color");
	// Row text colors: selected row green, others default.
	int green_runs = 0;
	int default_runs = 0;
	for (const auto &g : dl.glyphs) {
		if ((g.color & 0xFFFFFFu) == 0x00FF00u) {
			++green_runs;
		}
		if ((g.color & 0xFFFFFFu) == 0x111111u) {
			++default_runs;
		}
	}
	CHECK(green_runs == 3, "selected row text uses the selected fg");
	CHECK(default_runs == 6, "other rows use the default fg");

	// The spinlist color item draws a full-rect opaque swatch
	// [orig: CSpinListWnd_Render @ 0x64b220 — color | 0xFF000000].
	bool swatch = false;
	for (const MenuQuad &q : dl.quads) {
		if (q.texture == kMenuTexNone && q.color == 0xFFFF8040u &&
				q.y0 == 100.0f && q.y1 == 120.0f) {
			swatch = true;
		}
	}
	CHECK(swatch, "the spinlist color item draws the opaque swatch");
}

// The mouse pump [orig: scene_end_frame @ 0x63e600 ->
// widget_process_mouse_event @ 0x647a00]: front-most claim, disabled keeps
// state 1, hit+down -> pressed, hit+up -> hovered, misses clear.
void test_mouse_pump(const fnt_font_t *font) {
	mnu::Document doc = parse_or_die(kScreenXml);
	MenuFrameCompiler c;
	c.configure(doc.first_screen(), font);

	MenuFrameState state;
	// Over the OK button (design rect 10,20..110,40 under the root at 0,0),
	// button up: the BUTTON (index 1, drawn after its parent) claims —
	// front-most = last drawn — and lands hovered.
	MenuFrameCompiler::MouseClaim claim =
			c.pump_mouse(state, 50.0f, 30.0f, false, 1.0f, 1.0f);
	CHECK(claim.hovered == 1, "the front-most hit widget claims the mouse");
	const MenuWidgetState *row = nullptr;
	for (const MenuWidgetState &r : state.widgets) {
		if (r.index == 1) {
			row = &r;
		}
	}
	CHECK(row != nullptr && row->hovered && !row->pressed,
			"hit + button up lands visual state 2 (hovered)");

	// Same point, button held: pressed (state 3), hover cleared.
	claim = c.pump_mouse(state, 50.0f, 30.0f, true, 1.0f, 1.0f);
	CHECK(claim.hovered == 1, "the claim holds while the button is down");
	row = nullptr;
	for (const MenuWidgetState &r : state.widgets) {
		if (r.index == 1) {
			row = &r;
		}
	}
	CHECK(row != nullptr && row->pressed && !row->hovered,
			"hit + button down lands visual state 3 (pressed)");

	// Off every child, over the root container: the root claims and the
	// button's hover/press CLEARS (the per-frame claim).
	claim = c.pump_mouse(state, 500.0f, 500.0f, false, 1.0f, 1.0f);
	CHECK(claim.hovered == 0, "the container claims when no child hits");
	row = nullptr;
	for (const MenuWidgetState &r : state.widgets) {
		if (r.index == 1) {
			row = &r;
		}
	}
	CHECK(row != nullptr && !row->hovered && !row->pressed,
			"losing the claim clears the previous widget's hover/press");

	// Outside the screen entirely: no claim, cursor stays the default.
	claim = c.pump_mouse(state, 5000.0f, 5000.0f, false, 1.0f, 1.0f);
	CHECK(claim.hovered == -1, "a miss claims nothing");

	// A DISABLED claimant blocks widgets beneath but takes no hover/press
	// (visual state 1 wins).
	MenuWidgetState disabled_row;
	disabled_row.index = 1;
	disabled_row.disabled = true;
	state.widgets.clear();
	state.widgets.push_back(disabled_row);
	claim = c.pump_mouse(state, 50.0f, 30.0f, false, 1.0f, 1.0f);
	CHECK(claim.hovered == 1, "a disabled widget still owns the claim");
	CHECK(!state.widgets[0].hovered && !state.widgets[0].pressed,
			"a disabled claimant keeps visual state 1 - no hover/press");

	// A HIDDEN subtree never hits: hide the button, the point falls through
	// to the root container.
	state.widgets.clear();
	MenuWidgetState hidden_row;
	hidden_row.index = 1;
	hidden_row.hide = true;
	state.widgets.push_back(hidden_row);
	claim = c.pump_mouse(state, 50.0f, 30.0f, false, 1.0f, 1.0f);
	CHECK(claim.hovered == 0, "a hidden subtree never hits; the parent claims");

	// The scaled hit test: at 2x the button's design rect spans 20,40..220,80
	// in screen space, so a raw-screen point inside THAT claims it.
	state.widgets.clear();
	claim = c.pump_mouse(state, 200.0f, 60.0f, false, 2.0f, 2.0f);
	CHECK(claim.hovered == 1, "the hit test runs raw mouse against scaled rects");
}

// The table interior [orig: CUITable_Render @ 0x6411d0]: header labels, the
// divider in the >16px headroom band (0xFF7F7F7F), seeded data rows from the
// scroll window, clipped to the rect.
void test_table_interior(const fnt_font_t *font) {
	const char *xml = R"(
<SCREEN>
  <NAME>TBL</NAME>
  <WINDOW type="window" name="MAIN">
    <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>
    <FONT><NAME>t.fnt</NAME><DEFAULT_FG>FFFFFF</DEFAULT_FG></FONT>
    <WINDOW type="table" name="GRID">
      <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>400</RIGHT><BOTTOM>100</BOTTOM></POSITION>
      <COLUMN count="2">
        <HEADER column="0" width="200" justify="LEFT">ACTION</HEADER>
        <HEADER column="1" width="180" justify="LEFT">KEY</HEADER>
      </COLUMN>
    </WINDOW>
  </WINDOW>
</SCREEN>
)";
	mnu::Document doc = parse_or_die(xml);
	MenuFrameCompiler c;
	c.configure(doc.first_screen(), font);
	MenuFrameState state;
	MenuWidgetState grid;
	grid.index = 1;
	grid.table_rows = {{"FIRE", "MOUSE1"}, {"JUMP", "SPACE"}};
	state.widgets.push_back(grid);
	const MenuDrawList &dl = c.compile(state, 1.0f, 1.0f);
	CHECK(!dl.glyphs.empty(), "the table emits header + row glyph quads");
	bool divider = false;
	for (const auto &l : dl.lines) {
		if (l.color == 0xFF7F7F7Fu && l.y0 == l.y1) {
			divider = true;
		}
	}
	CHECK(divider, "each headroom band draws the 0xFF7F7F7F divider segment");
	// Second column starts after width 200: some glyph must anchor at x>=200.
	bool second_col = false;
	for (const auto &g : dl.glyphs) {
		if (g.x_top_left >= 200.0f) {
			second_col = true;
		}
	}
	CHECK(second_col, "the second column lays out past the first's width");
	// Scrolling to row 1 drops row 0's cells ("FIRE" disappears).
	const size_t full_glyphs = dl.glyphs.size();
	state.widgets[0].scroll_row = 1;
	const MenuDrawList &dl2 = c.compile(state, 1.0f, 1.0f);
	CHECK(dl2.glyphs.size() < full_glyphs,
			"the scroll window drops rows above first-visible");
}

// The marquee credits roll [orig: render_scrolling_credits @ 0x65ca00]:
// seeded lines draw centered, the roll advances with time_ms, and the whole
// roll resets after the last line passes the top.
void test_marquee_roll(const fnt_font_t *font) {
	const char *xml = R"(
<SCREEN>
  <NAME>MRQ</NAME>
  <WINDOW type="window" name="MAIN">
    <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>
    <FONT><NAME>t.fnt</NAME><DEFAULT_FG>FFFFFF</DEFAULT_FG></FONT>
    <WINDOW type="marquee" name="ROLL">
      <POSITION><LEFT>100</LEFT><TOP>100</TOP><RIGHT>700</RIGHT><BOTTOM>300</BOTTOM></POSITION>
    </WINDOW>
  </WINDOW>
</SCREEN>
)";
	mnu::Document doc = parse_or_die(xml);
	MenuFrameCompiler c;
	c.configure(doc.first_screen(), font);
	MenuFrameState state;
	MenuWidgetState roll;
	roll.index = 1;
	roll.marquee_lines = {"CREDITS", "", "OPENNOVA"};
	state.widgets.push_back(roll);
	// The roll enters from the BOTTOM: nothing draws at t=0.
	state.time_ms = 0;
	const MenuDrawList &dl0 = c.compile(state, 1.0f, 1.0f);
	CHECK(dl0.glyphs.empty(), "the roll starts below the rect (enters from the bottom)");
	// After the clock advances, the first line has scrolled into view.
	state.time_ms = 3000;
	const MenuDrawList &dl = c.compile(state, 1.0f, 1.0f);
	float first_y = -1.0f;
	for (const auto &g : dl.glyphs) {
		if (first_y < 0.0f || g.y_top < first_y) {
			first_y = g.y_top;
		}
	}
	CHECK(first_y >= 0.0f, "seeded credits lines draw");
	const size_t early_glyphs = dl.glyphs.size();
	(void)early_glyphs;
	// Further advance WITHIN one roll cycle (the whole-roll reset fires
	// once the offset exceeds the 3-line roll height): the roll scrolls UP.
	state.time_ms = 4400;
	const MenuDrawList &dl2 = c.compile(state, 1.0f, 1.0f);
	float second_y = 1.0e9f;
	for (const auto &g : dl2.glyphs) {
		if (g.y_top < second_y) {
			second_y = g.y_top;
		}
	}
	CHECK(!dl2.glyphs.empty() && second_y < first_y,
			"the roll advances upward with time_ms");
	// A reset restarts the roll below the rect: nothing draws again.
	state.widgets[0].marquee_reset = true;
	const MenuDrawList &dl3 = c.compile(state, 1.0f, 1.0f);
	CHECK(dl3.glyphs.empty(), "marquee_reset restarts the roll from the bottom");
}

// The witnessed edit-input operations [orig: edit_widget_insert_char
// @ 0x661ee0; edit_widget_handle_key_event @ 0x6623a0].
void test_edit_input_ops() {
	using opennova::menu::EditField;
	using opennova::menu::EditKeyResult;
	using opennova::menu::EditLimits;
	using opennova::menu::edit_apply_key;
	using opennova::menu::edit_insert_char;
	using opennova::menu::kEditKeyBackspace;
	using opennova::menu::kEditKeyDelete;
	using opennova::menu::kEditKeyEnd;
	using opennova::menu::kEditKeyEnter;
	using opennova::menu::kEditKeyHome;
	using opennova::menu::kEditKeyLeft;
	using opennova::menu::kEditKeyRight;

	EditField f;
	EditLimits lim;
	CHECK(edit_insert_char(f, lim, 'A') && f.text == "A" && f.caret == 1,
			"insert lands at the caret and advances it");
	f.caret = 0;
	CHECK(edit_insert_char(f, lim, 'B') && f.text == "BA",
			"insert respects a moved caret");
	CHECK(!edit_insert_char(f, lim, 0x0D) && !edit_insert_char(f, lim, 0x0A),
			"CR/LF reject");
	lim.read_only = true;
	CHECK(!edit_insert_char(f, lim, 'C'), "read-only rejects typing");
	lim.read_only = false;
	lim.max_len = 3;
	CHECK(edit_insert_char(f, lim, 'C', 5) && f.text.size() == 3,
			"max_len clamps the inserted run");
	lim.max_len = -1;

	EditField n;
	EditLimits nlim;
	nlim.numeric = true;
	nlim.min_value = 0;
	nlim.max_value = 100;
	CHECK(!edit_insert_char(n, nlim, 'x'), "numeric mode admits only digits");
	CHECK(edit_insert_char(n, nlim, '9') && edit_insert_char(n, nlim, '9'),
			"in-range digits insert");
	CHECK(!edit_insert_char(n, nlim, '9') && n.text == "99",
			"an out-of-range result rolls the WHOLE insert back");

	EditField k;
	k.text = "HELLO";
	k.caret = 5;
	CHECK(edit_apply_key(k, kEditKeyBackspace) == EditKeyResult::kChanged &&
					k.text == "HELL" && k.caret == 4,
			"backspace steps the caret back then deletes");
	CHECK(edit_apply_key(k, kEditKeyHome) == EditKeyResult::kNone &&
					k.caret == 0,
			"home zeroes the caret");
	CHECK(edit_apply_key(k, kEditKeyDelete) == EditKeyResult::kChanged &&
					k.text == "ELL",
			"delete removes at the caret");
	CHECK(edit_apply_key(k, kEditKeyEnd) == EditKeyResult::kNone &&
					k.caret == 3,
			"end lands on strlen");
	CHECK(edit_apply_key(k, kEditKeyLeft) == EditKeyResult::kNone &&
					k.caret == 2,
			"left steps back");
	CHECK(edit_apply_key(k, kEditKeyLeft, 1, true) == EditKeyResult::kNone &&
					k.caret == 2,
			"left under shift does not move (selection reserved)");
	CHECK(edit_apply_key(k, kEditKeyRight) == EditKeyResult::kNone &&
					k.caret == 3,
			"right steps forward, clamped to strlen");
	CHECK(edit_apply_key(k, kEditKeyEnter) == EditKeyResult::kCommit,
			"enter commits (the embedder releases focus)");
}

} // namespace

int main() {
	fnt_font_t font = make_font();
	test_draw_order_and_state_selection(&font);
	test_text_placement_and_truncation(&font);
	test_scale_truncation(&font);
	test_radio_checkbox_forcing(&font);
	test_edit_caret(&font);
	test_list_rows_and_item_cell(&font);
	test_mouse_pump(&font);
	test_table_interior(&font);
	test_marquee_roll(&font);
	test_edit_input_ops();
	fnt_free(&font);
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("menu_frame_compiler_test: all checks passed\n");
	return 0;
}
