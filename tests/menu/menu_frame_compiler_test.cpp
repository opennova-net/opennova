// MenuFrameCompiler pins: the witnessed .mnu screen draw walk emits the typed
// draw list — widget draw order, state-driven appearance/color selection, text
// placement, the edit caret, the frame pieces, and the per-element int
// truncation. [orig: CUIElement_Draw @ 0x64a8a0; CStaticWnd_Render @ 0x657b10;
//  CStaticWnd_DrawLabel @ 0x656fb0; draw_text_with_cursor @ 0x6533b0;
//  CEditWnd_Render @ 0x6619e0; CRadioWnd_Render @ 0x656e20; CCheckWnd_Render
//  @ 0x64ae20; CListWnd_DrawItems @ 0x643f30]
// Witness record: docs/mnu/menu-re.md ("Widget render dispatch").

#include <runtime/menu/menu_edit.h>
#include <runtime/menu/menu_frame.h>
#include <formats/mnu/mnu.h>

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

int count_quads_with_texture2(const MenuDrawList &dl, int32_t slot) {
	int n = 0;
	for (const MenuQuad &q : dl.quads) {
		if (q.texture2 == slot) {
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
	opennova::mnu::Document doc = parse_or_die(kScreenXml);
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
	// The frame: the stencil atlas fill cell + 8 stencil/brush material
	// pieces follow the appearance fill. Retail copies stencil cell (3, 0)
	// into border_fill_material, then builds border_material from STENCIL and
	// BRUSH as two texture stages [orig: init_border_materials @ 0x646f70].
	CHECK(dl.quads.size() >= 10, "frame quads follow");
	CHECK(dl.quads[1].texture == border && dl.quads[1].tiled,
			"the stencil fill cell tiles right after the appearance fill");
	CHECK(dl.quads[1].texture2 == kMenuTexNone,
			"the fill material uses only the copied stencil cell");
	CHECK(dl.quads[1].u0 == 0.75f && dl.quads[1].v0 == 0.0f &&
				dl.quads[1].u1 == 1.0f && dl.quads[1].v1 == 0.25f,
			"the fill samples stencil cell (3, 0)");
	int stencil_quads = 0;
	bool stencil_quads_are_untinted = true;
	for (size_t i = 2; i < 10; ++i) {
		if (dl.quads[i].texture == border && dl.quads[i].texture2 == brush) {
			++stencil_quads;
			stencil_quads_are_untinted =
					stencil_quads_are_untinted && dl.quads[i].color == 0xFFFFFFFFu;
		}
	}
	CHECK(stencil_quads == 8, "eight stencil/brush border material pieces");
	CHECK(dl.quads[1].color == 0xFFFFFFFFu,
			"the frame fill preserves the copied stencil color and alpha");
	CHECK(stencil_quads_are_untinted,
			"the frame material carries no extra compiler tint");
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

// Options-menu tabs and buttons use vertically stacked sprite sheets. The
// authored MAP_STATE selects one HEIGHT-tall row; emitting the whole texture
// stretches all four states into the widget and is immediately visible on the
// shipped Options screen. [orig: CUIElement_ParseXMLDefinition @ 0x648120;
// CUIElement_DrawTextureNative @ 0x647e40 ->
// CTextureManager_DrawScaledRect @ 0x654e60]
void test_image_appearance_crops_authored_map_state(const fnt_font_t *font) {
	const char *xml = R"(
<SCREEN>
  <NAME>OPTIONS</NAME>
  <WINDOW type="window" name="ROOT">
    <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>
    <WINDOW type="button" name="TAB">
      <POSITION><LEFT>10</LEFT><TOP>20</TOP><RIGHT>110</RIGHT><BOTTOM>40</BOTTOM></POSITION>
      <APPEARANCE type="image" state="default" map_state="2" height="20">tab_atlas.tga</APPEARANCE>
    </WINDOW>
  </WINDOW>
</SCREEN>
)";
	opennova::mnu::Document doc = parse_or_die(xml);
	MenuFrameCompiler c;
	c.configure(doc.first_screen(), font);
	const int32_t atlas = slot_of(c, "tab_atlas.tga");
	CHECK(atlas >= 0, "the Options tab atlas is interned");
	c.set_texture_size(atlas, 100, 80);

	MenuFrameState state;
	const MenuDrawList &dl = c.compile(state, 1.0f, 1.0f);
	const MenuQuad *tab = nullptr;
	for (const MenuQuad &q : dl.quads) {
		if (q.texture == atlas) {
			tab = &q;
			break;
		}
	}
	CHECK(tab != nullptr, "the Options tab image draws");
	if (tab != nullptr) {
		CHECK(tab->u0 == 0.0f && tab->u1 == 1.0f,
				"the atlas row spans the full texture width");
		CHECK(tab->v0 == 0.5f && tab->v1 == 0.75f,
				"map_state 2 crops the third 20px row from an 80px atlas");
	}
}

// A type="scroll" Window draws its COLOR/OUTLINE over the full rect and its
// IMAGE in the middle span, then its shuttle/up/down children. Options GAMMA
// authors no HEIGHT, so the constructor's 20px part extent wins rather than
// the arrow appearance crop. Its seeded retail range (5..20, page 2) produces
// the original 25px thumb in the 150px middle span. [orig:
// CScrollWnd_Construct @ 0x64c450; CScrollWnd_Render @ 0x64c5c0;
// scroll COLOR sink @ 0x64ce70; scroll IMAGE sink @ 0x64cf70;
// CUIScrollbar_CreateChildWindows @ 0x64d330;
// CUIScrollbar_CalcThumbRect @ 0x64cba0; options_screen_init @ 0x554800]
void test_scroll_draws_authored_visual_parts(const fnt_font_t *font) {
	const char *xml = R"(
<SCREEN>
  <NAME>OPTIONS</NAME>
  <WINDOW type="window" name="ROOT">
    <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>
    <WINDOW type="scroll" name="GAMMA">
      <POSITION><LEFT>185</LEFT><TOP>72</TOP><RIGHT>375</RIGHT><BOTTOM>94</BOTTOM></POSITION>
      <ORIENTATION>HORIZONTAL</ORIENTATION>
      <APPEARANCE type="image" state="default">track.tga</APPEARANCE>
      <SHUTTLE type="color" state="default">80FF0000</SHUTTLE>
      <SCROLLUP type="image" state="default">left.tga</SCROLLUP>
      <SCROLLDOWN type="image" state="default">right.tga</SCROLLDOWN>
    </WINDOW>
  </WINDOW>
</SCREEN>
)";
	opennova::mnu::Document doc = parse_or_die(xml);
	MenuFrameCompiler c;
	c.configure(doc.first_screen(), font);
	const int32_t track = slot_of(c, "track.tga");
	const int32_t up = slot_of(c, "left.tga");
	const int32_t down = slot_of(c, "right.tga");
	CHECK(track >= 0 && up >= 0 && down >= 0,
			"configure interns every authored scroll visual");
	c.set_texture_size(track, 76, 20);
	c.set_texture_size(up, 12, 20);
	c.set_texture_size(down, 12, 20);

	MenuWidgetState gamma_state;
	gamma_state.index = 1;
	gamma_state.has_scroll_range = true;
	gamma_state.scroll_min = 5;
	gamma_state.scroll_max = 20;
	gamma_state.scroll_page = 2;
	gamma_state.scroll_value = 5;
	MenuFrameState state;
	state.widgets.push_back(gamma_state);
	const MenuDrawList &dl = c.compile(state, 1.0f, 1.0f);
	CHECK(count_quads_with_texture(dl, track) == 1,
			"the scroll track draws exactly once");
	CHECK(up >= 0 && count_quads_with_texture(dl, up) == 1,
			"the authored up/left arrow draws exactly once");
	CHECK(down >= 0 && count_quads_with_texture(dl, down) == 1,
			"the authored down/right arrow draws exactly once");

	// Widget x=185..375, default extent=20: IMAGE track x=205..355.
	// Thumb = trunc(150 * (2+1) / (2-5+20+1)) = 25px; current=min
	// places it at the leading edge, x=205..230.
	if (dl.quads.size() == 4) {
		CHECK(dl.quads[0].texture == track && dl.quads[0].x0 == 205.0f &&
						dl.quads[0].y0 == 72.0f && dl.quads[0].x1 == 355.0f &&
						dl.quads[0].y1 == 94.0f,
				"the scroll IMAGE pass occupies only the middle track span");
		CHECK(dl.quads[1].texture == kMenuTexNone &&
						dl.quads[1].color == 0x80FF0000u && dl.quads[1].x0 == 205.0f &&
						dl.quads[1].x1 == 230.0f,
				"the color-only shuttle uses the seeded retail range and page");
		CHECK(dl.quads[2].texture == up && dl.quads[2].x0 == 185.0f &&
						dl.quads[2].y0 == 72.0f && dl.quads[2].x1 == 205.0f &&
						dl.quads[2].y1 == 94.0f,
				"the up/left arrow occupies the leading 20px extent");
		CHECK(dl.quads[3].texture == down && dl.quads[3].x0 == 355.0f &&
						dl.quads[3].y0 == 72.0f && dl.quads[3].x1 == 375.0f &&
						dl.quads[3].y1 == 94.0f,
				"the down/right arrow occupies the trailing extent");
	} else {
		CHECK(false, "the scroll emits exactly four visual quads");
	}
}

// An omitted RIGHT/BOTTOM on a spin arrow is completed from the native image
// extent, which means an authored HEIGHT row — not the full atlas —
// supplies the fallback height. The same completed rect is the arrow's hit
// target. [orig: CSpinListWnd_CreateUpDownChildren @ 0x64b8b0;
//  CUIElement_DrawTextureNative @ 0x647e40]
void test_spin_arrow_uses_cropped_atlas_extent(const fnt_font_t *font) {
	const char *xml = R"(
<SCREEN>
  <NAME>SPIN_CROP</NAME>
  <WINDOW type="window" name="ROOT">
    <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>
    <WINDOW type="spinlist" name="SPIN">
      <POSITION><LEFT>100</LEFT><TOP>100</TOP><RIGHT>200</RIGHT><BOTTOM>120</BOTTOM></POSITION>
      <ITEMS><ITEM type="ID" value="0">VALUE</ITEM></ITEMS>
      <SPINUP>
        <POSITION><LEFT>10</LEFT><TOP>5</TOP></POSITION>
        <APPEARANCE type="image" state="default" map_state="2" height="20">spin_up_atlas.tga</APPEARANCE>
      </SPINUP>
    </WINDOW>
  </WINDOW>
</SCREEN>
)";
	opennova::mnu::Document doc = parse_or_die(xml);
	MenuFrameCompiler c;
	c.configure(doc.first_screen(), font);
	const int32_t up = slot_of(c, "spin_up_atlas.tga");
	CHECK(up >= 0, "the spin-arrow atlas is interned");
	c.set_texture_size(up, 16, 80);

	MenuFrameState state;
	const MenuDrawList &dl = c.compile(state, 1.0f, 1.0f);
	const MenuQuad *arrow = nullptr;
	for (const MenuQuad &q : dl.quads) {
		if (q.texture == up) {
			arrow = &q;
			break;
		}
	}
	CHECK(arrow != nullptr, "the authored spin-up arrow draws");
	if (arrow != nullptr) {
		CHECK(arrow->x0 == 110.0f && arrow->y0 == 105.0f && arrow->x1 == 126.0f &&
						arrow->y1 == 125.0f,
				"the omitted arrow edges use the 16x20 cropped native extent");
		CHECK(arrow->v0 == 0.5f && arrow->v1 == 0.75f,
				"the spin arrow draws only map_state 2 from the 80px atlas");
	}
	CHECK(c.spin_arrow_at(1, state, 115.0f, 124.0f, 1.0f, 1.0f) == 1,
			"the hit target reaches the cropped row's completed bottom edge");
	CHECK(c.spin_arrow_at(1, state, 115.0f, 125.0f, 1.0f, 1.0f) == 0,
			"the half-open hit target ends at the cropped row height");
}

// A list draws its authored child scrollbar after its rows; the child uses
// its own rect, the original 20px part extent, and the owner's
// row range/page/value to place a proportional shuttle. [orig:
// CListWnd_DrawItems @ 0x643f30;
//  CScrollWnd_Render @ 0x64c5c0; CScrollWnd_SetPageSize @ 0x64ce10;
//  CScrollWnd_SetRangeAndClamp @ 0x64d490]
void test_list_scrollbar_uses_authored_geometry_and_range(
		const fnt_font_t *font) {
	const char *xml = R"(
<SCREEN>
  <NAME>LIST_SCROLL</NAME>
  <WINDOW type="window" name="ROOT">
    <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>
    <WINDOW type="list" name="LIST">
      <POSITION><LEFT>10</LEFT><TOP>20</TOP><RIGHT>110</RIGHT><BOTTOM>120</BOTTOM></POSITION>
      <ITEMS>
        <ITEM type="ID" value="0">ZERO</ITEM>
        <ITEM type="ID" value="1">ONE</ITEM>
        <ITEM type="ID" value="2">TWO</ITEM>
        <ITEM type="ID" value="3">THREE</ITEM>
        <ITEM type="ID" value="4">FOUR</ITEM>
        <ITEM type="ID" value="5">FIVE</ITEM>
        <ITEM type="ID" value="6">SIX</ITEM>
        <ITEM type="ID" value="7">SEVEN</ITEM>
        <ITEM type="ID" value="8">EIGHT</ITEM>
        <ITEM type="ID" value="9">NINE</ITEM>
      </ITEMS>
      <MIN_ITEM_HEIGHT>20</MIN_ITEM_HEIGHT>
      <SCROLLBAR>
        <POSITION><LEFT>80</LEFT><TOP>0</TOP><RIGHT>100</RIGHT><BOTTOM>100</BOTTOM></POSITION>
        <APPEARANCE type="image" state="default">list_track.tga</APPEARANCE>
        <SHUTTLE type="image" state="default">list_shuttle.tga</SHUTTLE>
        <SCROLLUP type="image" state="default" map_state="1" height="20">list_up_atlas.tga</SCROLLUP>
        <SCROLLDOWN type="image" state="default" map_state="3" height="20">list_down_atlas.tga</SCROLLDOWN>
      </SCROLLBAR>
    </WINDOW>
  </WINDOW>
</SCREEN>
)";
	opennova::mnu::Document doc = parse_or_die(xml);
	MenuFrameCompiler c;
	c.configure(doc.first_screen(), font);
	const int32_t track = slot_of(c, "list_track.tga");
	const int32_t shuttle = slot_of(c, "list_shuttle.tga");
	const int32_t up = slot_of(c, "list_up_atlas.tga");
	const int32_t down = slot_of(c, "list_down_atlas.tga");
	CHECK(track >= 0 && shuttle >= 0 && up >= 0 && down >= 0,
			"configure interns every nested list-scroll visual");
	c.set_texture_size(track, 20, 60);
	c.set_texture_size(shuttle, 20, 30);
	c.set_texture_size(up, 20, 80);
	c.set_texture_size(down, 20, 80);

	MenuWidgetState list_state;
	list_state.index = 1;
	list_state.scroll_row = 2;
	MenuFrameState state;
	state.widgets.push_back(list_state);
	const MenuDrawList &dl = c.compile(state, 1.0f, 1.0f);
	const MenuQuad *track_quad = nullptr;
	const MenuQuad *shuttle_quad = nullptr;
	const MenuQuad *up_quad = nullptr;
	const MenuQuad *down_quad = nullptr;
	for (const MenuQuad &q : dl.quads) {
		if (q.texture == track) {
			track_quad = &q;
		} else if (q.texture == shuttle) {
			shuttle_quad = &q;
		} else if (q.texture == up) {
			up_quad = &q;
		} else if (q.texture == down) {
			down_quad = &q;
		}
	}
	CHECK(track_quad != nullptr && shuttle_quad != nullptr &&
					up_quad != nullptr && down_quad != nullptr,
			"the nested list scrollbar draws track, shuttle, and both arrows");
	if (track_quad != nullptr) {
		CHECK(track_quad->x0 == 90.0f && track_quad->y0 == 40.0f &&
						track_quad->x1 == 110.0f && track_quad->y1 == 100.0f,
				"the scrollbar IMAGE pass occupies the span between its arrows");
	}
	if (up_quad != nullptr && down_quad != nullptr) {
		CHECK(up_quad->x0 == 90.0f && up_quad->y0 == 20.0f &&
						up_quad->x1 == 110.0f && up_quad->y1 == 40.0f &&
						down_quad->x0 == 90.0f && down_quad->y0 == 100.0f &&
						down_quad->x1 == 110.0f && down_quad->y1 == 120.0f,
				"the arrows occupy the authored scrollbar's two ends");
		CHECK(up_quad->v0 == 0.25f && up_quad->v1 == 0.5f &&
						down_quad->v0 == 0.75f && down_quad->v1 == 1.0f,
				"each arrow crops its authored 20px atlas row");
	}
	if (shuttle_quad != nullptr) {
		// 10 rows, 5 visible: 30px thumb in a 60px middle span. scroll_row=2
		// advances 2/5 of the remaining 30px travel: y=40+12..70+12.
		CHECK(shuttle_quad->x0 == 90.0f && shuttle_quad->y0 == 52.0f &&
						shuttle_quad->x1 == 110.0f && shuttle_quad->y1 == 82.0f,
				"range, page, and scroll_row place the proportional shuttle");
	}
	CHECK(c.list_row_at(1, state, 20.0f, 25.0f, 1.0f, 1.0f) == 2,
			"the first visible list row keeps its scrolled absolute index");
	CHECK(c.list_row_at(1, state, 95.0f, 25.0f, 1.0f, 1.0f) == -1,
			"the visible child scrollbar strip does not select a list row");

	// Retail hides the child when the visible page contains every row.
	state.widgets[0].has_items = true;
	state.widgets[0].items = { "A", "B", "C", "D", "E" };
	state.widgets[0].scroll_row = 0;
	const MenuDrawList &fit = c.compile(state, 1.0f, 1.0f);
	CHECK(count_quads_with_texture(fit, track) == 0 &&
					count_quads_with_texture(fit, shuttle) == 0 &&
					count_quads_with_texture(fit, up) == 0 &&
					count_quads_with_texture(fit, down) == 0,
			"a list scrollbar stays hidden while all rows fit");
}

// A combo delegates its popup to the same CList range/child model. Scrolling
// changes the absolute item index at the top, and the visible scrollbar child
// owns its strip before row selection. [orig: CComboWnd_Render @ 0x65bfd0;
// CListWnd_DrawItems @ 0x643f30; CListWnd_CreateScrollChild @ 0x6444c0]
void test_combo_scrollbar_offsets_rows_and_hit(const fnt_font_t *font) {
	const char *xml = R"(
<SCREEN>
  <NAME>COMBO_SCROLL</NAME>
  <WINDOW type="window" name="ROOT">
    <FONT><NAME>f.fnt</NAME><DEFAULT_FG>FFFFFF</DEFAULT_FG></FONT>
    <WINDOW type="combo" name="COMBO">
      <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>100</RIGHT><BOTTOM>20</BOTTOM></POSITION>
      <ITEMS>
        <ITEM type="ID" value="0">ZERO</ITEM><ITEM type="ID" value="1">ONE</ITEM>
        <ITEM type="ID" value="2">TWO</ITEM><ITEM type="ID" value="3">THREE</ITEM>
        <ITEM type="ID" value="4">FOUR</ITEM><ITEM type="ID" value="5">FIVE</ITEM>
      </ITEMS>
      <LIST_BOX>
        <POSITION><LEFT>0</LEFT><TOP>20</TOP><RIGHT>100</RIGHT><BOTTOM>100</BOTTOM></POSITION>
        <MIN_ITEM_HEIGHT>20</MIN_ITEM_HEIGHT>
        <SCROLLBAR>
          <POSITION><LEFT>80</LEFT><TOP>0</TOP><RIGHT>100</RIGHT><BOTTOM>80</BOTTOM></POSITION>
          <APPEARANCE type="image" state="default">combo_track.tga</APPEARANCE>
          <SHUTTLE type="image" state="default">combo_shuttle.tga</SHUTTLE>
          <SCROLLUP type="image" state="default">combo_up.tga</SCROLLUP>
          <SCROLLDOWN type="image" state="default">combo_down.tga</SCROLLDOWN>
        </SCROLLBAR>
      </LIST_BOX>
    </WINDOW>
  </WINDOW>
</SCREEN>
)";
	opennova::mnu::Document doc = parse_or_die(xml);
	MenuFrameCompiler c;
	c.configure(doc.first_screen(), font);
	for (const char *name : { "combo_track.tga", "combo_shuttle.tga",
				 "combo_up.tga", "combo_down.tga" }) {
		c.set_texture_size(slot_of(c, name), 20, 20);
	}
	MenuWidgetState combo;
	combo.index = 1;
	combo.popup_open = true;
	combo.scroll_row = 1;
	MenuFrameState state;
	state.widgets.push_back(combo);
	const MenuDrawList &dl = c.compile(state, 1.0f, 1.0f);
	CHECK(count_quads_with_texture(dl, slot_of(c, "combo_track.tga")) == 1,
			"the overflowing combo popup draws its scrollbar");
	CHECK(c.combo_popup_row_at(1, state, 10.0f, 21.0f, 1.0f, 1.0f) == 1,
			"the popup's top hit maps to its first scrolled absolute row");
	CHECK(c.combo_popup_row_at(1, state, 90.0f, 21.0f, 1.0f, 1.0f) == -1,
			"the popup scrollbar strip does not select a row");
	// The open popup is menu-top overlay: every op from overlay_op_start on is
	// the popup (+ cursor), and the scrollbar track quad is one of them, so a
	// shell can paint that tail above any Control it mounts over the frame.
	CHECK(dl.overlay_op_start > 0 &&
					dl.overlay_op_start < static_cast<int32_t>(dl.draw_ops.size()),
			"the open popup starts the draw list's overlay tail");
	{
		bool track_in_overlay = false;
		const int32_t track = slot_of(c, "combo_track.tga");
		for (size_t i = static_cast<size_t>(dl.overlay_op_start);
				i < dl.draw_ops.size(); ++i) {
			const MenuDrawList::DrawOp &op = dl.draw_ops[i];
			if (op.kind == MenuDrawList::DrawOp::Kind::Quad &&
					dl.quads[static_cast<size_t>(op.index)].texture == track) {
				track_in_overlay = true;
			}
		}
		CHECK(track_in_overlay, "the popup's own quads sit in the overlay tail");
	}
	MenuWidgetState closed = combo;
	closed.popup_open = false;
	MenuFrameState closed_state;
	closed_state.widgets.push_back(closed);
	const MenuDrawList &closed_dl = c.compile(closed_state, 1.0f, 1.0f);
	CHECK(closed_dl.overlay_op_start ==
					static_cast<int32_t>(closed_dl.draw_ops.size()),
			"with no popup and no cursor the overlay tail is empty");
}

// TABLE has two independent heights: the FONT "W" measure owns the header,
// while top-level MIN_ITEM_HEIGHT owns body rows. An unpositioned scrollbar
// covers the full table height, while its page ratio uses the body-row height.
// [orig: CTableWnd_ParseXMLContentDefinition @ 0x6427d0; table SCROLLBAR
// delegate
// @ 0x643b22; CUITable_Render @ 0x6411d0; CScrollWnd_SetPageSize @ 0x64ce10;
// CScrollWnd_SetRangeAndClamp @ 0x64d490]
void test_table_scrollbar_separates_header_and_body_row_heights(
		const fnt_font_t *font) {
	const char *xml = R"(
<SCREEN>
  <NAME>TABLE_SCROLL</NAME>
  <WINDOW type="window" name="ROOT">
    <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>
    <FONT><NAME>f.fnt</NAME><DEFAULT_FG>FFFFFF</DEFAULT_FG></FONT>
    <WINDOW type="table" name="TABLE">
      <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>100</RIGHT><BOTTOM>116</BOTTOM></POSITION>
      <MIN_ITEM_HEIGHT>20</MIN_ITEM_HEIGHT>
      <COLUMN count="1">
        <HEADER column="0" width="80" justify="LEFT">NAME</HEADER>
      </COLUMN>
      <SCROLLBAR>
        <APPEARANCE type="image" state="default">table_track.tga</APPEARANCE>
        <SHUTTLE type="image" state="default">table_shuttle.tga</SHUTTLE>
        <SCROLLUP type="image" state="default">table_up.tga</SCROLLUP>
        <SCROLLDOWN type="image" state="default">table_down.tga</SCROLLDOWN>
      </SCROLLBAR>
    </WINDOW>
  </WINDOW>
</SCREEN>
)";
	opennova::mnu::Document doc = parse_or_die(xml);
	MenuFrameCompiler c;
	c.configure(doc.first_screen(), font);
	const int32_t shuttle = slot_of(c, "table_shuttle.tga");
	const int32_t up = slot_of(c, "table_up.tga");
	CHECK(shuttle >= 0 && up >= 0,
			"configure interns the table scrollbar visuals");
	for (const char *name : { "table_track.tga", "table_shuttle.tga",
				 "table_up.tga", "table_down.tga" }) {
		c.set_texture_size(slot_of(c, name), 16, 16);
	}

	MenuWidgetState table_state;
	table_state.index = 1;
	table_state.scroll_row = 2;
	for (int i = 0; i < 10; ++i) {
		table_state.table_rows.push_back({ std::to_string(i) });
	}
	MenuFrameState state;
	state.widgets.push_back(table_state);
	const MenuDrawList &dl = c.compile(state, 1.0f, 1.0f);
	const MenuQuad *up_quad = nullptr;
	const MenuQuad *shuttle_quad = nullptr;
	for (const MenuQuad &q : dl.quads) {
		if (q.texture == up) {
			up_quad = &q;
		} else if (q.texture == shuttle) {
			shuttle_quad = &q;
		}
	}
	CHECK(up_quad != nullptr && shuttle_quad != nullptr,
			"the table scrollbar emits its arrow and shuttle");
	if (up_quad != nullptr) {
		CHECK(up_quad->x0 == 78.0f && up_quad->y0 == 0.0f &&
						up_quad->x1 == 100.0f && up_quad->y1 == 20.0f,
				"the unpositioned table bar is a full-height rightmost 22px strip");
	}
	if (shuttle_quad != nullptr) {
		// (116-16px header) / 20px rows = a 5-row page. The 76px middle
		// span yields a 38px thumb; scroll_row 2 places it at y=35..73.
		CHECK(shuttle_quad->x0 == 78.0f && shuttle_quad->y0 == 35.0f &&
						shuttle_quad->x1 == 100.0f && shuttle_quad->y1 == 73.0f,
				"the thumb page uses body MIN_ITEM_HEIGHT, not header height");
	}
}

// Retail clamps the table's stored visible-row count to one after dividing the
// body height by the row height. A one-row table therefore does not sprout a
// scrollbar merely because less than one complete body row fits below the
// header. [orig: CTableWnd_RecalcLayout @ 0x63f1a0, clamp @ 0x63f276]
void test_table_visible_count_floors_to_one(const fnt_font_t *font) {
	const char *xml = R"(
<SCREEN>
  <NAME>TABLE_TINY</NAME>
  <WINDOW type="window" name="ROOT">
    <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>
    <FONT><NAME>f.fnt</NAME><DEFAULT_FG>FFFFFF</DEFAULT_FG></FONT>
    <WINDOW type="table" name="TABLE">
      <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>100</RIGHT><BOTTOM>25</BOTTOM></POSITION>
      <MIN_ITEM_HEIGHT>20</MIN_ITEM_HEIGHT>
      <COLUMN count="1"><HEADER column="0" width="80">NAME</HEADER></COLUMN>
      <SCROLLBAR>
        <SCROLLUP type="image" state="default">tiny_up.tga</SCROLLUP>
      </SCROLLBAR>
    </WINDOW>
  </WINDOW>
</SCREEN>
)";
	opennova::mnu::Document doc = parse_or_die(xml);
	MenuFrameCompiler c;
	c.configure(doc.first_screen(), font);
	const int32_t up = slot_of(c, "tiny_up.tga");
	CHECK(up >= 0, "the tiny-table scrollbar visual is interned");
	c.set_texture_size(up, 16, 16);
	MenuWidgetState table_state;
	table_state.index = 1;
	table_state.table_rows.push_back({ "only" });
	MenuFrameState state;
	state.widgets.push_back(table_state);
	const MenuDrawList &dl = c.compile(state, 1.0f, 1.0f);
	CHECK(count_quads_with_texture(dl, up) == 0,
			"one stored visible row keeps a one-row tiny table scrollbar hidden");
}

// Table cells draw with the ROW's own state, never the widget hover visual:
// headers push state 0, a selected row is state 3, and the per-cell background
// comes from the ITEMS appearance record for that row state (outline grid on
// default rows, color fill on the selected row — options.mnu CONTROL_MAPPING
// authors exactly that pair). [orig: CUITable_Render text state @
// 0x64189a..0x6418da, header push 0 @ 0x641446, cell backgrounds @
// 0x641642..0x6416b6; CTableWnd_SetRowSelected @ 0x63f5f0 — states 0/1/3]
void test_table_rows_draw_row_state_not_widget_hover(const fnt_font_t *font) {
	const char *xml = R"(
<SCREEN>
  <NAME>TABLE_SEL</NAME>
  <WINDOW type="window" name="ROOT">
    <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>
    <FONT><NAME>f.fnt</NAME><DEFAULT_FG>AAAAAA</DEFAULT_FG></FONT>
    <WINDOW type="table" name="TABLE">
      <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>100</RIGHT><BOTTOM>116</BOTTOM></POSITION>
      <FONT><NAME>f.fnt</NAME><DEFAULT_FG>AAAAAA</DEFAULT_FG><MOUSEOVER_FG>FF0000</MOUSEOVER_FG><SELECTED_FG>00FF00</SELECTED_FG></FONT>
      <MIN_ITEM_HEIGHT>20</MIN_ITEM_HEIGHT>
      <COLUMN count="1"><HEADER column="0" width="80" justify="LEFT">NAME</HEADER></COLUMN>
      <ITEMS>
        <APPEARANCE type="outline" state="default">445566</APPEARANCE>
        <APPEARANCE type="color" state="selected">112233</APPEARANCE>
      </ITEMS>
    </WINDOW>
  </WINDOW>
</SCREEN>
)";
	opennova::mnu::Document doc = parse_or_die(xml);
	MenuFrameCompiler c;
	c.configure(doc.first_screen(), font);
	MenuWidgetState table_state;
	table_state.index = 1;
	table_state.hovered = true; // the widget-level visual must not tint cells
	table_state.selected_item = -1;
	table_state.selected_items.push_back(1);
	for (int i = 0; i < 3; ++i) {
		table_state.table_rows.push_back({ "r" + std::to_string(i) });
	}
	MenuFrameState state;
	state.widgets.push_back(table_state);
	const MenuDrawList &dl = c.compile(state, 1.0f, 1.0f);

	// Row 1 spans y 36..56 (16px measured header + 20px rows); glyph tops
	// carry the font's -0.5 bake offset.
	bool any_mouseover_text = false;
	bool selected_row_text_selected_fg = false;
	bool other_text_default_fg = false;
	for (const auto &g : dl.glyphs) {
		const uint32_t rgb = g.color & 0xFFFFFFu;
		if (rgb == 0xFF0000u) {
			any_mouseover_text = true;
		}
		if (g.y_top >= 35.0f && g.y_top < 55.0f) {
			if (rgb == 0x00FF00u) {
				selected_row_text_selected_fg = true;
			}
		} else if (rgb == 0xAAAAAAu) {
			other_text_default_fg = true;
		}
	}
	CHECK(!any_mouseover_text,
			"widget hover never tints table cells or headers");
	CHECK(selected_row_text_selected_fg,
			"the selected row's text uses the selected fg");
	CHECK(other_text_default_fg,
			"header and unselected rows keep the default fg");

	bool selected_fill = false;
	for (const MenuQuad &q : dl.quads) {
		if ((q.color & 0xFFFFFFu) == 0x112233u && q.texture == kMenuTexNone &&
				q.y0 == 36.0f && q.y1 == 56.0f) {
			selected_fill = true;
		}
	}
	CHECK(selected_fill,
			"the selected row's cell draws the ITEMS selected color fill");

	int trim_lines = 0;
	for (const auto &l : dl.lines) {
		if ((l.color & 0xFFFFFFu) == 0x445566u) {
			++trim_lines;
		}
	}
	// Two default-state rows x one cell x 4 outline edges.
	CHECK(trim_lines == 8,
			"default rows draw the ITEMS outline grid per cell");
}

void test_text_placement_and_truncation(const fnt_font_t *font) {
	opennova::mnu::Document doc = parse_or_die(kScreenXml);
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
	opennova::mnu::Document doc = parse_or_die(kScreenXml);
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
	opennova::mnu::Document doc = parse_or_die(xml);
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
	opennova::mnu::Document doc = parse_or_die(xml);
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
      <MIN_ITEM_HEIGHT>20</MIN_ITEM_HEIGHT>
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
	opennova::mnu::Document doc = parse_or_die(xml);
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
	opennova::mnu::Document doc = parse_or_die(kScreenXml);
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
	opennova::mnu::Document doc = parse_or_die(xml);
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
	opennova::mnu::Document doc = parse_or_die(xml);
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

// DRAW_FRAME gating [orig: CStaticWnd_Render @ 0x657b10 — field +0x134 guards
// CUIElement_DrawFrame]: a window may author a <FRAME> purely to hand its
// textures down to framed descendants (jo_game.mnu's root MAIN defines the
// camo BOXTILE brush + BORDER2 stencil with NO DRAW_FRAME and must draw no
// frame — the full-window camo bug on the in-game ESC menu); a child WINDOW
// carrying DRAW_FRAME draws the INHERITED frame
// [orig: CWnd_FindInheritedFrameBlock @ 0x647190].
void test_draw_frame_gate(const fnt_font_t *font) {
	const char *xml = R"(
<SCREEN>
  <NAME>DF</NAME>
  <WINDOW type="window" name="MAIN">
    <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>
    <FRAME>
      <STENCIL size="8">border.tga</STENCIL>
      <BRUSH>tile.tga</BRUSH>
    </FRAME>
    <WINDOW type="window" name="BOX" DRAW_FRAME>
      <POSITION><LEFT>100</LEFT><TOP>100</TOP><RIGHT>300</RIGHT><BOTTOM>250</BOTTOM></POSITION>
    </WINDOW>
  </WINDOW>
</SCREEN>
)";
	opennova::mnu::Document doc = parse_or_die(xml);
	MenuFrameCompiler c;
	c.configure(doc.first_screen(), font);
	const int32_t border = slot_of(c, "border.tga");
	const int32_t brush = slot_of(c, "tile.tga");
	CHECK(border >= 0 && brush >= 0, "the parent's FRAME textures intern");
	c.set_texture_size(border, 32, 32);
	c.set_texture_size(brush, 64, 64);

	MenuFrameState state;
	const MenuDrawList &dl = c.compile(state, 1.0f, 1.0f);
	CHECK(dl.widgets_drawn == 2, "both windows draw");
	// The parent authored the FRAME but no DRAW_FRAME: exactly one stencil
	// fill cell and one eight-piece stencil+brush set appear — the child’s.
	CHECK(count_quads_with_texture(dl, border) == 9,
			"no DRAW_FRAME on the parent: one child fill plus eight borders draw");
	CHECK(count_quads_with_texture2(dl, brush) == 8,
			"only the child's eight border material pieces use the brush");
	// The child's stencil fill covers the CHILD rect (100,100)-(300,250), not
	// the parent window: inherited frame assets stay scoped to DRAW_FRAME.
	const MenuQuad *fill = nullptr;
	for (const MenuQuad &q : dl.quads) {
		if (q.texture == border && q.texture2 == kMenuTexNone && q.tiled) {
			fill = &q;
		}
	}
	CHECK(fill != nullptr && fill->x0 == 100.0f && fill->y0 == 100.0f &&
					fill->x1 == 300.0f && fill->y1 == 250.0f,
			"the inherited frame fills the DRAW_FRAME child's rect only");
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

// Runtime-seeded item rows + the multi-select set: the embedder's set_items
// replaces the authored <ITEM> rows for the closed cell, the list rows, and
// the combo popup alike; MULTI lists style every selected-set row.
void test_runtime_items_and_multiselect(const fnt_font_t *font) {
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
      </ITEMS>
      <MIN_ITEM_HEIGHT>20</MIN_ITEM_HEIGHT>
    </WINDOW>
  </WINDOW>
</SCREEN>
)";
	opennova::mnu::Document doc = parse_or_die(xml);
	MenuFrameCompiler c;
	c.configure(doc.first_screen(), font);

	MenuWidgetState list;
	list.index = 1;
	list.has_items = true;
	list.items = {"XX", "YY", "ZZ"};
	list.selected_item = 0;
	list.selected_items = {0, 2};
	MenuFrameState st;
	st.widgets.push_back(list);
	const MenuDrawList &dl = c.compile(st, 1.0f, 1.0f);
	// Three runtime rows of two glyphs each replace the single authored row.
	CHECK(dl.glyphs.size() == 6, "runtime rows replace the authored items");
	CHECK(c.item_count(1, st) == 3, "item_count reports the runtime rows");
	// Rows 0 and 2 both fill the selection style (the MULTI selected set).
	int sel_fills = 0;
	for (const MenuQuad &q : dl.quads) {
		if (q.texture == kMenuTexNone && (q.color & 0xFFFFFFu) == 0x204060u) {
			++sel_fills;
		}
	}
	CHECK(sel_fills == 2, "the selected set styles every selected row");
}

// The widget queries: pre-order identity, names/kinds, authored text, the
// absolute rect accumulation, and the edit-limits mapping.
void test_widget_queries(const fnt_font_t *font) {
	opennova::mnu::Document doc = parse_or_die(kScreenXml);
	MenuFrameCompiler c;
	c.configure(doc.first_screen(), font);

	CHECK(c.widget_count() == 3, "widget_count covers the pre-order tree");
	CHECK(c.widget_name(1) == "OK", "widget_name reads the authored NAME");
	CHECK(c.widget_kind(1) == static_cast<int>(opennova::mnu::WindowType::Button),
			"widget_kind reports the parsed type");
	CHECK(c.widget_authored_text(1) == "OK",
			"widget_authored_text resolves the STRING");
	MenuFrameState st;
	opennova::mnu::RectEdges rect{};
	CHECK(c.widget_rect(1, st, &rect) && rect.left == 10 && rect.top == 20 &&
					rect.right == 110 && rect.bottom == 40,
			"widget_rect solves the nested absolute rect");

	const char *edit_xml = R"(
<SCREEN>
  <NAME>E</NAME>
  <WINDOW type="window" name="ROOT">
    <WINDOW type="edit" name="NUM" NUMBER MINVAL="1" MAXVAL="99" MAXCHAR="2">
      <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>60</RIGHT><BOTTOM>20</BOTTOM></POSITION>
    </WINDOW>
  </WINDOW>
</SCREEN>
)";
	opennova::mnu::Document edoc = parse_or_die(edit_xml);
	MenuFrameCompiler ec;
	ec.configure(edoc.first_screen(), font);
	opennova::menu::EditLimits lim;
	CHECK(ec.widget_edit_limits(1, &lim) && lim.numeric &&
					lim.min_value == 1 && lim.max_value == 99 &&
					lim.max_len == 2 && !lim.read_only,
			"widget_edit_limits maps the authored constraints");
}

// The interaction geometry queries reuse the emitters' witnessed layout math
// [orig: CListWnd_DrawItems @ 0x643f30 rows; CComboWnd @ 0x65be40 popup rect;
//  CSpinListWnd_CreateUpDownChildren @ 0x64b8b0 arrow rects].
void test_interaction_geometry(const fnt_font_t *font) {
	const char *xml = R"(
<SCREEN>
  <NAME>G</NAME>
  <WINDOW type="window" name="ROOT">
    <FONT><NAME>f.fnt</NAME><DEFAULT_FG>111111</DEFAULT_FG></FONT>
    <WINDOW type="list" name="L1">
      <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>100</RIGHT><BOTTOM>64</BOTTOM></POSITION>
      <ITEMS>
        <ITEM type="ID" value="0">AAA</ITEM>
        <ITEM type="ID" value="1">BBB</ITEM>
        <ITEM type="ID" value="2">CCC</ITEM>
        <ITEM type="ID" value="3">DDD</ITEM>
      </ITEMS>
      <MIN_ITEM_HEIGHT>20</MIN_ITEM_HEIGHT>
    </WINDOW>
    <WINDOW type="combo" name="C1">
      <POSITION><LEFT>200</LEFT><TOP>0</TOP><RIGHT>300</RIGHT><BOTTOM>20</BOTTOM></POSITION>
      <ITEMS>
        <ITEM type="ID" value="0">ONE</ITEM>
        <ITEM type="ID" value="1">TWO</ITEM>
      </ITEMS>
      <LIST_BOX>
        <POSITION><LEFT>0</LEFT><TOP>20</TOP><RIGHT>100</RIGHT><BOTTOM>80</BOTTOM></POSITION>
        <MIN_ITEM_HEIGHT>20</MIN_ITEM_HEIGHT>
      </LIST_BOX>
    </WINDOW>
    <WINDOW type="spinlist" name="S1">
      <POSITION><LEFT>400</LEFT><TOP>0</TOP><RIGHT>460</RIGHT><BOTTOM>20</BOTTOM></POSITION>
      <ITEMS><ITEM type="ID" value="0">V</ITEM></ITEMS>
      <SPINUP>
        <POSITION><LEFT>40</LEFT><TOP>0</TOP><RIGHT>50</RIGHT><BOTTOM>10</BOTTOM></POSITION>
        <APPEARANCE type="image" state="default">up.tga</APPEARANCE>
      </SPINUP>
      <SPINDOWN>
        <POSITION><LEFT>40</LEFT><TOP>10</TOP><RIGHT>50</RIGHT><BOTTOM>20</BOTTOM></POSITION>
        <APPEARANCE type="image" state="default">down.tga</APPEARANCE>
      </SPINDOWN>
    </WINDOW>
  </WINDOW>
</SCREEN>
)";
	opennova::mnu::Document doc = parse_or_die(xml);
	MenuFrameCompiler c;
	c.configure(doc.first_screen(), font);
	MenuFrameState st;

	// List rows: 20 high from the top; the scroll window offsets.
	CHECK(c.list_row_at(1, st, 50.0f, 25.0f, 1.0f, 1.0f) == 1,
			"list_row_at maps a point to its row");
	CHECK(c.list_row_at(1, st, 150.0f, 25.0f, 1.0f, 1.0f) == -1,
			"list_row_at rejects points outside the rect");
	CHECK(c.list_visible_rows(1, st) == 3, "list_visible_rows floors rect/row_h");
	MenuWidgetState scrolled;
	scrolled.index = 1;
	scrolled.scroll_row = 1;
	st.widgets.push_back(scrolled);
	CHECK(c.list_row_at(1, st, 50.0f, 5.0f, 1.0f, 1.0f) == 1,
			"the scroll window shifts the row mapping");
	st.widgets.clear();

	// The combo popup rect is combo-relative [orig: D-MNU-7], rows inside it.
	opennova::mnu::RectEdges popup{};
	CHECK(c.combo_popup_rect(2, st, &popup) && popup.left == 200 &&
					popup.top == 20 && popup.right == 300 && popup.bottom == 80,
			"combo_popup_rect offsets the authored LIST_BOX rect");
	CHECK(c.combo_popup_contains(2, st, 250.0f, 50.0f, 1.0f, 1.0f),
			"combo_popup_contains covers the popup");
	CHECK(c.combo_popup_row_at(2, st, 250.0f, 45.0f, 1.0f, 1.0f) == 1,
			"combo_popup_row_at maps popup rows");

	// Spin arrows: authored child rects offset into the widget.
	CHECK(c.spin_arrow_at(3, st, 445.0f, 5.0f, 1.0f, 1.0f) == 1,
			"the up arrow zone reports 1");
	CHECK(c.spin_arrow_at(3, st, 445.0f, 15.0f, 1.0f, 1.0f) == 2,
			"the down arrow zone reports 2");
	CHECK(c.spin_arrow_at(3, st, 405.0f, 5.0f, 1.0f, 1.0f) == 0,
			"outside both arrows reports 0");

	// The non-mutating hit query mirrors the pump's claim walk.
	CHECK(c.hit_widget(st, 50.0f, 25.0f, 1.0f, 1.0f) == 1,
			"hit_widget claims the front-most widget");
	CHECK(st.widgets.empty(), "hit_widget mutates no state");
}

// D-MNU-15/16: the combo closed face draws the selection over RUNTIME-seeded
// rows (the armory's companion fill — the authored-only gate left every such
// face blank) [orig: the closed face is the +764 CButtonWnd showing
// items[selected], CComboWnd ctor @ 0x65be40], and the pump's claim walk
// hands presses in the OUTSIDE-authored spin arrow rects to the spin widget
// (mp.mnu GAME_TYPE authors −18..−2 / 217..233 against a 0..215 widget)
// [orig: CSpinListWnd_CreateUpDownChildren @ 0x64b8b0 child-window rects].
void test_combo_face_and_outside_arrow_claim(const fnt_font_t *font) {
	const char *xml = R"(
<SCREEN>
  <NAME>K</NAME>
  <WINDOW type="window" name="ROOT">
    <FONT><NAME>f.fnt</NAME><DEFAULT_FG>111111</DEFAULT_FG></FONT>
    <WINDOW type="combo" name="C1">
      <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>100</RIGHT><BOTTOM>20</BOTTOM></POSITION>
      <LIST_BOX>
        <POSITION><LEFT>0</LEFT><TOP>20</TOP><RIGHT>100</RIGHT><BOTTOM>80</BOTTOM></POSITION>
        <MIN_ITEM_HEIGHT>20</MIN_ITEM_HEIGHT>
      </LIST_BOX>
    </WINDOW>
    <WINDOW type="spinlist" name="S1">
      <POSITION><LEFT>200</LEFT><TOP>0</TOP><RIGHT>260</RIGHT><BOTTOM>20</BOTTOM></POSITION>
      <ITEMS><ITEM type="ID" value="0">V</ITEM></ITEMS>
      <SPINUP>
        <POSITION><LEFT>-18</LEFT><TOP>0</TOP><RIGHT>-2</RIGHT><BOTTOM>16</BOTTOM></POSITION>
        <APPEARANCE type="image" state="default">up.tga</APPEARANCE>
      </SPINUP>
      <SPINDOWN>
        <POSITION><LEFT>62</LEFT><TOP>0</TOP><RIGHT>78</RIGHT><BOTTOM>16</BOTTOM></POSITION>
        <APPEARANCE type="image" state="default">down.tga</APPEARANCE>
      </SPINDOWN>
    </WINDOW>
  </WINDOW>
</SCREEN>
)";
	opennova::mnu::Document doc = parse_or_die(xml);
	MenuFrameCompiler c;
	c.configure(doc.first_screen(), font);

	// No authored items, no runtime rows: the swapped row text is empty and
	// the face draws NOTHING — never the widget's own authored TEXT
	// [orig: CComboWnd_Render @ 0x65c05b..0x65c083 swaps unconditionally].
	MenuFrameState st;
	const MenuDrawList &empty_face = c.compile(st, 1.0f, 1.0f);
	int combo_glyphs = 0;
	for (const auto &g : empty_face.glyphs) {
		if (g.x_top_left < 150.0f) {
			++combo_glyphs;
		}
	}
	CHECK(combo_glyphs == 0, "an unseeded authored-itemless combo face is empty");

	// Runtime rows: the closed face draws items[selected] (D-MNU-15).
	MenuWidgetState combo;
	combo.index = 1;
	combo.has_items = true;
	combo.items = {"ALPHA", "BRAVO"};
	combo.selected_item = 1;
	st.widgets.push_back(combo);
	const MenuDrawList &face = c.compile(st, 1.0f, 1.0f);
	combo_glyphs = 0;
	for (const auto &g : face.glyphs) {
		if (g.x_top_left < 150.0f) {
			++combo_glyphs;
		}
	}
	CHECK(combo_glyphs == 5,
			"the closed combo face draws the runtime selection 'BRAVO'");

	// The outside-authored arrows claim the spin widget in the pump walk
	// (D-MNU-16): SPINUP local -18..-2 -> absolute 182..198.
	st.widgets.clear();
	const auto claim_up = c.pump_mouse(st, 190.0f, 8.0f, false, 1.0f, 1.0f);
	CHECK(claim_up.hovered == 2, "the outside SPINUP rect claims the spinlist");
	CHECK(c.spin_arrow_at(2, st, 190.0f, 8.0f, 1.0f, 1.0f) == 1,
			"spin_arrow_at reports up in the same rect");
	// SPINDOWN local 62..78 -> absolute 262..278 (right of the widget).
	const auto claim_down = c.pump_mouse(st, 270.0f, 8.0f, false, 1.0f, 1.0f);
	CHECK(claim_down.hovered == 2,
			"the outside SPINDOWN rect claims the spinlist");
	CHECK(c.spin_arrow_at(2, st, 270.0f, 8.0f, 1.0f, 1.0f) == 2,
			"spin_arrow_at reports down in the same rect");
	// Between the arrows and the widget nothing claims.
	const auto claim_gap = c.pump_mouse(st, 199.0f, 8.0f, false, 1.0f, 1.0f);
	CHECK(claim_gap.hovered == 0 || claim_gap.hovered == -1,
			"the gap between arrow and widget claims spin never");
	CHECK(c.hit_widget(st, 190.0f, 8.0f, 1.0f, 1.0f) == 2,
			"hit_widget mirrors the arrow claim");
}

// The accelerator scan [orig: the screen hotkey registration family
// @ 0x5674a8; hidden subtrees prune — a hidden BACK must not eat ESC].
void test_hotkey_widget(const fnt_font_t *font) {
	const char *xml = R"(
<SCREEN>
  <NAME>H</NAME>
  <WINDOW type="window" name="ROOT">
    <WINDOW type="window" name="HIDDEN_GROUP" HIDDEN>
      <WINDOW type="button" name="HIDDEN_ESC">
        <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>10</RIGHT><BOTTOM>10</BOTTOM></POSITION>
        <HOTKEY VIRTUAL>VK_ESCAPE</HOTKEY>
      </WINDOW>
    </WINDOW>
    <WINDOW type="button" name="BACK">
      <POSITION><LEFT>0</LEFT><TOP>20</TOP><RIGHT>10</RIGHT><BOTTOM>30</BOTTOM></POSITION>
      <HOTKEY VIRTUAL>VK_ESCAPE</HOTKEY>
      <HOTKEY>V</HOTKEY>
    </WINDOW>
    <WINDOW type="button" name="GO">
      <POSITION><LEFT>0</LEFT><TOP>40</TOP><RIGHT>10</RIGHT><BOTTOM>50</BOTTOM></POSITION>
      <HOTKEY VIRTUAL>VK_ENTER</HOTKEY>
    </WINDOW>
  </WINDOW>
</SCREEN>
)";
	opennova::mnu::Document doc = parse_or_die(xml);
	MenuFrameCompiler c;
	c.configure(doc.first_screen(), font);
	MenuFrameState st;
	// The hidden subtree's ESC never matches; the shown BACK does.
	CHECK(c.hotkey_widget("VK_ESCAPE", true, st) == 3,
			"a hidden subtree prunes; the shown widget matches");
	// Character rows are a separate namespace, case-insensitive.
	CHECK(c.hotkey_widget("v", false, st) == 3,
			"character hotkeys match case-insensitively");
	CHECK(c.hotkey_widget("VK_ESCAPE", false, st) == -1,
			"a virtual name never matches as a character");
	// VK_RETURN and VK_ENTER are interchangeable.
	CHECK(c.hotkey_widget("VK_RETURN", true, st) == 4,
			"VK_RETURN matches an authored VK_ENTER");
	// A runtime show override un-prunes the subtree; pre-order then prefers it.
	MenuWidgetState shown;
	shown.index = 1;
	shown.show = true;
	st.widgets.push_back(shown);
	CHECK(c.hotkey_widget("VK_ESCAPE", true, st) == 2,
			"a shown-override subtree joins the scan in pre-order");
}

// The wrapped multiline-edit drawer [orig: CMEditWnd_Render @ 0x6608e0 ->
// draw_text_wrapped_clipped @ 0x653D60 -> draw_text_wrapped @ 0x653710]:
// word wrap at the last space, explicit LF, the line-based scroll window,
// the bottom clip, and the count twin [orig: @ 0x653b90].
void test_multiline_wrap(const fnt_font_t *font) {
	const char *xml = R"(
<SCREEN>
  <NAME>M</NAME>
  <WINDOW type="window" name="ROOT">
    <FONT><NAME>f.fnt</NAME><DEFAULT_FG>111111</DEFAULT_FG></FONT>
    <WINDOW type="multiline_edit" name="BODY" READONLY>
      <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>100</RIGHT><BOTTOM>64</BOTTOM></POSITION>
    </WINDOW>
  </WINDOW>
</SCREEN>
)";
	opennova::mnu::Document doc = parse_or_die(xml);
	MenuFrameCompiler c;
	c.configure(doc.first_screen(), font);

	// 9 px advance per glyph: 12 chars measure 106 > 100, so the line breaks
	// at the LAST SPACE (index 9) — "AAAA BBBB" then "CCCC".
	MenuWidgetState body;
	body.index = 1;
	body.has_text = true;
	body.text = "AAAA BBBB CCCC";
	MenuFrameState st;
	st.widgets.push_back(body);
	const MenuDrawList &dl = c.compile(st, 1.0f, 1.0f);
	// Glyphs ride the GameFont half-texel offset; bucket rows by the 16px
	// line height.
	int rows_y0 = 0;
	int rows_y16 = 0;
	for (const auto &g : dl.glyphs) {
		if (g.y_top < 8.0f) {
			++rows_y0;
		} else if (g.y_top < 24.0f) {
			++rows_y16;
		}
	}
	CHECK(rows_y0 == 9 && rows_y16 == 4,
			"word wrap breaks at the last space; the space is consumed");

	// An explicit LF breaks; CR is not special.
	st.widgets[0].text = "AB\nCD";
	const MenuDrawList &dl2 = c.compile(st, 1.0f, 1.0f);
	rows_y0 = 0;
	rows_y16 = 0;
	for (const auto &g : dl2.glyphs) {
		if (g.y_top < 8.0f) {
			++rows_y0;
		} else if (g.y_top < 24.0f) {
			++rows_y16;
		}
	}
	CHECK(rows_y0 == 2 && rows_y16 == 2, "an explicit LF breaks the line");

	// The scroll window skips lines without advancing y.
	st.widgets[0].text = "AAAA BBBB CCCC";
	st.widgets[0].scroll_row = 1;
	const MenuDrawList &dl3 = c.compile(st, 1.0f, 1.0f);
	CHECK(dl3.glyphs.size() == 4 && dl3.glyphs[0].y_top < 8.0f,
			"the first-visible-line window skips rows at the top");
	st.widgets[0].scroll_row = 0;

	// The bottom clip stops when the NEXT line's bottom would overflow.
	const char *short_xml = R"(
<SCREEN>
  <NAME>M2</NAME>
  <WINDOW type="window" name="ROOT">
    <FONT><NAME>f.fnt</NAME><DEFAULT_FG>111111</DEFAULT_FG></FONT>
    <WINDOW type="multiline_edit" name="BODY" READONLY>
      <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>100</RIGHT><BOTTOM>20</BOTTOM></POSITION>
    </WINDOW>
  </WINDOW>
</SCREEN>
)";
	opennova::mnu::Document sdoc = parse_or_die(short_xml);
	MenuFrameCompiler sc;
	sc.configure(sdoc.first_screen(), font);
	MenuFrameState sst;
	MenuWidgetState sbody;
	sbody.index = 1;
	sbody.has_text = true;
	sbody.text = "AB\nCD";
	sst.widgets.push_back(sbody);
	const MenuDrawList &dl4 = sc.compile(sst, 1.0f, 1.0f);
	CHECK(dl4.glyphs.size() == 2, "the bottom clip truncates trailing lines");

	// The count twin: 2 wrapped lines, both fitting a 64-high rect.
	int fit = 0;
	int total = 0;
	CHECK(c.multiline_line_counts(1, st, &fit, &total) && total == 2 &&
					fit == 2,
			"multiline_line_counts reports the scroll range inputs");
}

// The compiler walk is painter ordered across primitive kinds. A later
// sibling's background must cover an earlier sibling's label; separate quad
// and glyph vectors cannot communicate that ordering to the applier. [orig:
// CUIScene_DrawScreensAndCursor @ 0x63bf60; CUIElement_Draw @ 0x64a8a0;
// CStaticWnd_Render @ 0x657b10]
void test_draw_list_preserves_interleaved_primitive_order(
		const fnt_font_t *font) {
	const char *xml = R"(
<SCREEN>
  <NAME>INTERLEAVED</NAME>
  <WINDOW type="window" name="ROOT">
    <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>
    <FONT><NAME>f.fnt</NAME><DEFAULT_FG>FFFFFF</DEFAULT_FG></FONT>
    <WINDOW type="static" name="EARLIER_LABEL">
      <POSITION><LEFT>10</LEFT><TOP>10</TOP><RIGHT>110</RIGHT><BOTTOM>30</BOTTOM></POSITION>
      <STRING>EARLIER</STRING>
    </WINDOW>
    <WINDOW type="window" name="LATER_COVER">
      <POSITION><LEFT>10</LEFT><TOP>10</TOP><RIGHT>110</RIGHT><BOTTOM>30</BOTTOM></POSITION>
      <APPEARANCE type="color" state="default">112233</APPEARANCE>
    </WINDOW>
  </WINDOW>
</SCREEN>
)";
	opennova::mnu::Document doc = parse_or_die(xml);
	MenuFrameCompiler c;
	c.configure(doc.first_screen(), font);

	MenuFrameState state;
	const MenuDrawList &dl = c.compile(state, 1.0f, 1.0f);
	CHECK(dl.font_runs.size() == 1 && dl.quads.size() == 1,
			"the fixture emits earlier text and one later covering quad");
	CHECK(!dl.glyphs.empty() && !dl.quads.empty() &&
					dl.glyphs[0].x_bottom_right > dl.quads[0].x0 &&
					dl.glyphs[0].x_top_left < dl.quads[0].x1 &&
					dl.glyphs[0].y_bottom > dl.quads[0].y0 &&
					dl.glyphs[0].y_top < dl.quads[0].y1,
			"the later quad geometrically overlaps the earlier label");

	using DrawKind = MenuDrawList::DrawOp::Kind;
	CHECK(dl.draw_ops.size() == 2,
			"the draw list records both primitive groups in compiler order");
	if (dl.draw_ops.size() == 2) {
		CHECK(dl.draw_ops[0].kind == DrawKind::FontRun && dl.draw_ops[0].index == 0,
				"the earlier label's font run is the first draw operation");
		CHECK(dl.draw_ops[1].kind == DrawKind::Quad && dl.draw_ops[1].index == 0,
				"the later covering quad follows the label in painter order");
	}
}

// The witnessed CScrollWnd interaction map: arrows step, the track pages
// toward the click, the shuttle press anchors a drag whose moves invert the
// travel ratio into a clamped value.
// [orig: CScrollWnd_HandleEvent @ 0x64d050 (arrows @ 0x64d2d9/0x64d31a, track
//  @ 0x64d0f0/0x64d10e, anchor @ 0x64d1cb..0x64d217, drag @ 0x64d231..
//  0x64d2aa); ctor defaults step 1 @ 0x64c4cf, page 10 @ 0x64c4d9]
void test_scroll_interaction_hits_and_drag(const fnt_font_t *font) {
	const char *xml = R"(
<SCREEN>
  <NAME>OPTIONS</NAME>
  <WINDOW type="window" name="ROOT">
    <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>
    <WINDOW type="scroll" name="GAMMA">
      <POSITION><LEFT>100</LEFT><TOP>72</TOP><RIGHT>300</RIGHT><BOTTOM>92</BOTTOM></POSITION>
      <ORIENTATION>HORIZONTAL</ORIENTATION>
      <APPEARANCE type="image" state="default">track.tga</APPEARANCE>
      <SHUTTLE type="color" state="default">80FF0000</SHUTTLE>
      <SCROLLUP type="image" state="default">left.tga</SCROLLUP>
      <SCROLLDOWN type="image" state="default">right.tga</SCROLLDOWN>
    </WINDOW>
  </WINDOW>
</SCREEN>
)";
	opennova::mnu::Document doc = parse_or_die(xml);
	MenuFrameCompiler c;
	c.configure(doc.first_screen(), font);
	c.set_texture_size(slot_of(c, "track.tga"), 76, 20);
	c.set_texture_size(slot_of(c, "left.tga"), 12, 20);
	c.set_texture_size(slot_of(c, "right.tga"), 12, 20);

	MenuWidgetState gamma;
	gamma.index = 1;
	gamma.has_scroll_range = true;
	gamma.scroll_min = 0;
	gamma.scroll_max = 100;
	gamma.scroll_page = 10;
	gamma.scroll_value = 0;
	MenuFrameState state;
	state.widgets.push_back(gamma);
	c.compile(state, 1.0f, 1.0f);

	// Widget 100..300 wide, ctor part extent 20: arrows 100..120 / 280..300,
	// track 120..280 (160px), 20px-floored shuttle at 120..140 for value 0.
	CHECK(c.scroll_hit_at(1, state, 105.0f, 80.0f, 1.0f, 1.0f) ==
					MenuFrameCompiler::kScrollHitUp,
			"the left arrow strip hits Up");
	CHECK(c.scroll_hit_at(1, state, 295.0f, 80.0f, 1.0f, 1.0f) ==
					MenuFrameCompiler::kScrollHitDown,
			"the right arrow strip hits Down");
	CHECK(c.scroll_hit_at(1, state, 125.0f, 80.0f, 1.0f, 1.0f) ==
					MenuFrameCompiler::kScrollHitShuttle,
			"the value-0 shuttle sits at the track start");
	CHECK(c.scroll_hit_at(1, state, 270.0f, 80.0f, 1.0f, 1.0f) ==
					MenuFrameCompiler::kScrollHitTrackAfter,
			"the strip past the shuttle hits track-after");
	CHECK(c.scroll_hit_at(1, state, 50.0f, 80.0f, 1.0f, 1.0f) ==
					MenuFrameCompiler::kScrollHitNone,
			"outside the widget nothing hits");

	// Drag: anchor at a press on the shuttle, then a move to the track end
	// lands the max value; back to the start lands the min.
	const int anchor = c.scroll_drag_anchor(1, state, 125.0f, 80.0f, 1.0f, 1.0f);
	CHECK(c.scroll_drag_value(1, state, 500.0f, 80.0f, 1.0f, 1.0f, anchor) == 100,
			"dragging past the track end clamps to max");
	CHECK(c.scroll_drag_value(1, state, 125.0f, 80.0f, 1.0f, 1.0f, anchor) == 0,
			"dragging back to the press point restores the pressed value");
	CHECK(c.scroll_drag_value(1, state, 0.0f, 80.0f, 1.0f, 1.0f, anchor) == 0,
			"dragging before the track start clamps to min");
}

// The embedded-scrollbar owners scroll ROWS: range 0..rows-visible, page =
// visible-1, value = the first visible row — the same CScrollWnd parts and
// drag math as the standalone slider. [orig: the table SCROLLBAR delegate
// @ 0x643b22; CMEditWnd page = visibleLines - 1; CScrollWnd_HandleEvent
// @ 0x64d050]
void test_table_embedded_scrollbar_scrolls_rows(const fnt_font_t *font) {
	const char *xml = R"(
<SCREEN>
  <NAME>CONTROLS</NAME>
  <WINDOW type="window" name="ROOT">
    <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>
    <FONT><NAME>f.fnt</NAME><DEFAULT_FG>FFFFFF</DEFAULT_FG></FONT>
    <WINDOW type="table" name="MAPPING">
      <POSITION><LEFT>100</LEFT><TOP>100</TOP><RIGHT>400</RIGHT><BOTTOM>216</BOTTOM></POSITION>
      <MIN_ITEM_HEIGHT>20</MIN_ITEM_HEIGHT>
      <COLUMN count="1">
        <HEADER column="0" width="200" justify="LEFT">ACTION</HEADER>
      </COLUMN>
      <SCROLLBAR>
        <POSITION><LEFT>280</LEFT><TOP>0</TOP><RIGHT>300</RIGHT><BOTTOM>116</BOTTOM></POSITION>
        <APPEARANCE type="color" state="default">303030</APPEARANCE>
        <SHUTTLE type="color" state="default">80FF0000</SHUTTLE>
        <SCROLLUP type="color" state="default">505050</SCROLLUP>
        <SCROLLDOWN type="color" state="default">505050</SCROLLDOWN>
      </SCROLLBAR>
    </WINDOW>
  </WINDOW>
</SCREEN>
)";
	opennova::mnu::Document doc = parse_or_die(xml);
	MenuFrameCompiler c;
	c.configure(doc.first_screen(), font);
	MenuWidgetState table;
	table.index = 1;
	for (int i = 0; i < 10; ++i) {
		table.table_rows.push_back({ "ROW" });
	}
	table.scroll_row = 0;
	MenuFrameState state;
	state.widgets.push_back(table);
	c.compile(state, 1.0f, 1.0f);

	// Header (font "W" = 16+2 spacing measure) + 20px body rows over the
	// 116px widget: the row span reports rows=10 and a visible count; the
	// scrollable remainder is the row limit.
	const int limit = c.scroll_row_limit(1, state);
	const int page = c.scroll_page_rows(1, state);
	CHECK(limit > 0 && limit < 10, "part of the 10 rows scrolls out of view");
	CHECK(page >= 1, "the page step is at least one row");
	// The authored scrollbar strip (combo-relative 380..400 x 100..216):
	// arrows at the extent ends, the value-0 shuttle at the track start.
	CHECK(c.scroll_hit_at(1, state, 390.0f, 105.0f, 1.0f, 1.0f) ==
					MenuFrameCompiler::kScrollHitUp,
			"the top strip hits Up");
	CHECK(c.scroll_hit_at(1, state, 390.0f, 210.0f, 1.0f, 1.0f) ==
					MenuFrameCompiler::kScrollHitDown,
			"the bottom strip hits Down");
	CHECK(c.scroll_hit_at(1, state, 390.0f, 125.0f, 1.0f, 1.0f) ==
					MenuFrameCompiler::kScrollHitShuttle,
			"the value-0 shuttle sits at the track start");
	// A drag from the shuttle to the track end reaches the row limit.
	const int anchor = c.scroll_drag_anchor(1, state, 390.0f, 125.0f, 1.0f, 1.0f);
	CHECK(c.scroll_drag_value(1, state, 390.0f, 400.0f, 1.0f, 1.0f, anchor) ==
					limit,
			"dragging to the track end lands the last first-visible row");
}

// The retail combo closed face (the +764 CButtonWnd) shows the SELECTED row of
// the embedded LIST — shipped options.mnu combos (WATERQUALITY et al.) author
// their rows ONLY inside <LIST_BOX><ITEMS> with an empty widget STRING, so a
// face that reads only widget-level ITEMS renders blank.
// [orig: CComboWnd ctor @ 0x65be40 — the closed cell; the LIST_BOX parse
//  delegate CComboWnd_ParseXMLDefinition @ 0x65c0d0]
void test_combo_face_shows_list_box_selection(const fnt_font_t *font) {
	const char *xml = R"(
<SCREEN>
  <NAME>COMBO_FACE</NAME>
  <WINDOW type="window" name="ROOT">
    <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>
    <FONT><NAME>f.fnt</NAME><DEFAULT_FG>FFFFFF</DEFAULT_FG></FONT>
    <WINDOW type="combo" name="WATER">
      <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>152</RIGHT><BOTTOM>20</BOTTOM></POSITION>
      <STRING edge="5" justify="CENTER" vjustify="CENTER"></STRING>
      <LIST_BOX>
        <POSITION><LEFT>0</LEFT><TOP>20</TOP><RIGHT>152</RIGHT><BOTTOM>80</BOTTOM></POSITION>
        <ITEMS justify="LEFT" vjustify="CENTER">
          <ITEM type="ID" value="1">LOW</ITEM>
          <ITEM type="ID" value="2">NORMAL</ITEM>
          <ITEM type="ID" value="3">HIGH</ITEM>
        </ITEMS>
        <MIN_ITEM_HEIGHT>20</MIN_ITEM_HEIGHT>
      </LIST_BOX>
    </WINDOW>
  </WINDOW>
</SCREEN>
)";
	opennova::mnu::Document doc = parse_or_die(xml);
	MenuFrameCompiler c;
	c.configure(doc.first_screen(), font);
	MenuWidgetState combo;
	combo.index = 1;
	combo.selected_item = 1; // NORMAL
	MenuFrameState state;
	state.widgets.push_back(combo);
	const MenuDrawList &dl = c.compile(state, 1.0f, 1.0f);
	// The closed cell draws the selection's glyphs inside the widget rect
	// (y within [0, 20)) — 6 glyphs for "NORMAL".
	int face_glyphs = 0;
	for (const auto &g : dl.glyphs) {
		if (g.y_top >= 0.0f && g.y_bottom <= 20.0f) {
			++face_glyphs;
		}
	}
	CHECK(face_glyphs == 6,
			"the closed combo face draws the LIST_BOX selection (NORMAL)");
}

// The open dropdown draws OVER later widgets: shipped options.mnu authors
// WATERQUALITY before SHADOWQUALITY/PARTICLES, yet its open popup covers
// them — the scene draw defers the registered open popup to the end of the
// walk, it is NOT painted inline at tree position.
// [orig: CUIElement_Draw @ 0x64a8a0 (the popup-flagged re-register);
//  g_ui_open_popup_wnd @ 0x31C16D8]
void test_open_combo_popup_draws_over_later_widgets(const fnt_font_t *font) {
	const char *xml = R"(
<SCREEN>
  <NAME>COMBO_OVER</NAME>
  <WINDOW type="window" name="ROOT">
    <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>
    <FONT><NAME>f.fnt</NAME><DEFAULT_FG>FFFFFF</DEFAULT_FG></FONT>
    <WINDOW type="combo" name="WATER">
      <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>152</RIGHT><BOTTOM>20</BOTTOM></POSITION>
      <LIST_BOX>
        <APPEARANCE type="color" state="default">445566</APPEARANCE>
        <POSITION><LEFT>0</LEFT><TOP>20</TOP><RIGHT>152</RIGHT><BOTTOM>80</BOTTOM></POSITION>
        <ITEMS justify="LEFT" vjustify="CENTER">
          <ITEM type="ID" value="1">LOW</ITEM>
          <ITEM type="ID" value="2">NORMAL</ITEM>
        </ITEMS>
        <MIN_ITEM_HEIGHT>20</MIN_ITEM_HEIGHT>
      </LIST_BOX>
    </WINDOW>
    <WINDOW type="window" name="SHADOWS_BELOW">
      <POSITION><LEFT>0</LEFT><TOP>30</TOP><RIGHT>152</RIGHT><BOTTOM>50</BOTTOM></POSITION>
      <APPEARANCE type="color" state="default">112233</APPEARANCE>
    </WINDOW>
  </WINDOW>
</SCREEN>
)";
	opennova::mnu::Document doc = parse_or_die(xml);
	MenuFrameCompiler c;
	c.configure(doc.first_screen(), font);
	MenuWidgetState combo;
	combo.index = 1;
	combo.popup_open = true;
	MenuFrameState state;
	state.widgets.push_back(combo);
	const MenuDrawList &dl = c.compile(state, 1.0f, 1.0f);
	int popup_bg = -1;
	int later_cover = -1;
	for (size_t i = 0; i < dl.quads.size(); ++i) {
		if ((dl.quads[i].color & 0x00FFFFFFu) == 0x445566u) {
			popup_bg = static_cast<int>(i);
		}
		if ((dl.quads[i].color & 0x00FFFFFFu) == 0x112233u) {
			later_cover = static_cast<int>(i);
		}
	}
	CHECK(popup_bg >= 0 && later_cover >= 0,
			"both the popup background and the later sibling emit quads");
	int popup_op = -1;
	int later_op = -1;
	for (size_t i = 0; i < dl.draw_ops.size(); ++i) {
		const auto &op = dl.draw_ops[i];
		if (op.kind == MenuDrawList::DrawOp::Kind::Quad) {
			if (op.index == popup_bg) {
				popup_op = static_cast<int>(i);
			}
			if (op.index == later_cover) {
				later_op = static_cast<int>(i);
			}
		}
	}
	CHECK(popup_op >= 0 && later_op >= 0 && popup_op > later_op,
			"the open popup paints AFTER the later sibling that overlaps it");
}

// The pump-integrated CScrollWnd interaction: a press on a part claims and
// applies at the pump seam, the pressed part captures every held sample
// until release (retail's child-window capture — no other widget can turn
// the press into a click), and a disabled owner claims without acting.
// [orig: CScrollWnd_HandleEvent @ 0x64d050 — arrows @ 0x64d2d9/0x64d31a,
//  track @ 0x64d0f0/0x64d10e, anchor @ 0x64d1cb..0x64d217, drag
//  @ 0x64d231..0x64d2aa]
void test_scroll_pump_owns_press_capture_and_value(const fnt_font_t *font) {
	const char *xml = R"(
<SCREEN>
  <NAME>OPTIONS</NAME>
  <WINDOW type="window" name="ROOT">
    <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>
    <WINDOW type="scroll" name="GAMMA">
      <POSITION><LEFT>100</LEFT><TOP>72</TOP><RIGHT>300</RIGHT><BOTTOM>92</BOTTOM></POSITION>
      <ORIENTATION>HORIZONTAL</ORIENTATION>
      <APPEARANCE type="color" state="default">303030</APPEARANCE>
      <SHUTTLE type="color" state="default">80FF0000</SHUTTLE>
      <SCROLLUP type="color" state="default">505050</SCROLLUP>
      <SCROLLDOWN type="color" state="default">505050</SCROLLDOWN>
    </WINDOW>
  </WINDOW>
</SCREEN>
)";
	opennova::mnu::Document doc = parse_or_die(xml);
	MenuFrameCompiler c;
	c.configure(doc.first_screen(), font);
	MenuWidgetState gamma;
	gamma.index = 1;
	gamma.has_scroll_range = true;
	gamma.scroll_min = 0;
	gamma.scroll_max = 100;
	gamma.scroll_page = 10;
	gamma.scroll_value = 50;
	MenuFrameState state;
	state.widgets.push_back(gamma);

	// Press edge on the right arrow: the slider claims, steps +1, applies.
	auto claim = c.pump_mouse(state, 295.0f, 80.0f, true, 1.0f, 1.0f);
	CHECK(claim.hovered == 1 && claim.scroll_index == 1,
			"the arrow press claims the slider through the pump");
	CHECK(claim.scroll_value_changed && claim.scroll_value == 51,
			"the arrow steps +1");
	CHECK(state.widgets[0].scroll_value == 51,
			"the pump applies the value into the widget state");

	// Held samples drifting elsewhere: the latch keeps the claim on the
	// slider and never auto-repeats — the ghost-press class is impossible.
	claim = c.pump_mouse(state, 400.0f, 300.0f, true, 1.0f, 1.0f);
	CHECK(claim.hovered == 1 && claim.scroll_index == 1 &&
					!claim.scroll_value_changed,
			"the latched part keeps the mouse without repeating");
	claim = c.pump_mouse(state, 400.0f, 300.0f, false, 1.0f, 1.0f);
	CHECK(claim.scroll_index == -1, "release frees the latch to the walk");

	// Shuttle press captures with no step; the drag lands the ratio value.
	// value 51 -> shuttle offset 20 + 51*140/100 = 91 -> x 191..211.
	claim = c.pump_mouse(state, 200.0f, 80.0f, true, 1.0f, 1.0f);
	CHECK(claim.scroll_index == 1 && !claim.scroll_value_changed,
			"the shuttle press captures without a value step");
	claim = c.pump_mouse(state, 500.0f, 80.0f, true, 1.0f, 1.0f);
	CHECK(claim.scroll_value_changed && claim.scroll_value == 100,
			"the captured drag past the track end lands the max");
	c.pump_mouse(state, 500.0f, 80.0f, false, 1.0f, 1.0f);

	// A disabled owner claims (blocking beneath) but takes no action.
	state.widgets[0].disabled = true;
	claim = c.pump_mouse(state, 295.0f, 80.0f, true, 1.0f, 1.0f);
	CHECK(claim.scroll_index == 1 && !claim.scroll_value_changed,
			"a disabled slider claims without scrolling");
	CHECK(state.widgets[0].scroll_value == 100, "the disabled value holds");
	c.pump_mouse(state, 295.0f, 80.0f, false, 1.0f, 1.0f);
}

// An OPEN combo popup's scrollbar child is interactive through the same
// pump: arrows step scroll_row, the shuttle captures and drags, and the
// pressed part owns the sample so it can never become a popup row pick.
// [orig: dispatch_mouse_event @ 0x63ab00 g_ui_open_popup_wnd gate routes to
//  the popup; CListWnd child walk @ 0x643f30 gives its scrollbar the event
//  first; CScrollWnd_HandleEvent @ 0x64d050 is the part interaction]
void test_combo_popup_scrollbar_scrolls_through_pump(const fnt_font_t *font) {
	const char *xml = R"(
<SCREEN>
  <NAME>COMBO_POPUP_SCROLL</NAME>
  <WINDOW type="window" name="ROOT">
    <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>
    <FONT><NAME>f.fnt</NAME><DEFAULT_FG>FFFFFF</DEFAULT_FG></FONT>
    <WINDOW type="combo" name="COMBO">
      <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>100</RIGHT><BOTTOM>20</BOTTOM></POSITION>
      <ITEMS>
        <ITEM type="ID" value="0">ZERO</ITEM><ITEM type="ID" value="1">ONE</ITEM>
        <ITEM type="ID" value="2">TWO</ITEM><ITEM type="ID" value="3">THREE</ITEM>
        <ITEM type="ID" value="4">FOUR</ITEM><ITEM type="ID" value="5">FIVE</ITEM>
      </ITEMS>
      <LIST_BOX>
        <POSITION><LEFT>0</LEFT><TOP>20</TOP><RIGHT>100</RIGHT><BOTTOM>100</BOTTOM></POSITION>
        <MIN_ITEM_HEIGHT>20</MIN_ITEM_HEIGHT>
        <SCROLLBAR>
          <POSITION><LEFT>80</LEFT><TOP>0</TOP><RIGHT>100</RIGHT><BOTTOM>80</BOTTOM></POSITION>
          <APPEARANCE type="color" state="default">303030</APPEARANCE>
          <SHUTTLE type="color" state="default">80FF0000</SHUTTLE>
          <SCROLLUP type="color" state="default">505050</SCROLLUP>
          <SCROLLDOWN type="color" state="default">505050</SCROLLDOWN>
        </SCROLLBAR>
      </LIST_BOX>
    </WINDOW>
  </WINDOW>
</SCREEN>
)";
	opennova::mnu::Document doc = parse_or_die(xml);
	MenuFrameCompiler c;
	c.configure(doc.first_screen(), font);
	MenuWidgetState combo;
	combo.index = 1;
	combo.popup_open = true;
	combo.scroll_row = 0;
	MenuFrameState state;
	state.widgets.push_back(combo);

	// Popup (0,20)-(100,100): 6 rows, 4 visible, range 0..2, page 3. The
	// authored scrollbar sits absolute (80,20)-(100,100): up arrow to y 40,
	// track to y 80, down arrow below.
	auto claim = c.pump_mouse(state, 90.0f, 90.0f, true, 1.0f, 1.0f);
	CHECK(claim.hovered == 1 && claim.scroll_index == 1,
			"the popup down-arrow press claims the combo through the pump");
	CHECK(claim.scroll_value_changed && claim.scroll_value == 1,
			"the popup down arrow steps scroll_row +1");
	CHECK(state.widgets[0].scroll_row == 1,
			"the pump applies the popup scroll into the widget state");
	// Held drift onto the row strip: the latch keeps the claim — a scrollbar
	// press can never become a popup row pick.
	claim = c.pump_mouse(state, 50.0f, 55.0f, true, 1.0f, 1.0f);
	CHECK(claim.scroll_index == 1 && !claim.scroll_value_changed,
			"the latched popup arrow keeps the mouse without repeating");
	c.pump_mouse(state, 50.0f, 55.0f, false, 1.0f, 1.0f);

	// Up arrow steps back.
	claim = c.pump_mouse(state, 90.0f, 30.0f, true, 1.0f, 1.0f);
	CHECK(claim.scroll_value_changed && claim.scroll_value == 0 &&
					state.widgets[0].scroll_row == 0,
			"the popup up arrow steps scroll_row -1");
	c.pump_mouse(state, 90.0f, 30.0f, false, 1.0f, 1.0f);

	// Shuttle drag: at scroll_row 0 the 26px shuttle tops the track (y 40).
	// Capture there, drag past the track end: the ratio lands the max row.
	claim = c.pump_mouse(state, 90.0f, 50.0f, true, 1.0f, 1.0f);
	CHECK(claim.scroll_index == 1 && !claim.scroll_value_changed,
			"the popup shuttle press captures without a value step");
	claim = c.pump_mouse(state, 90.0f, 100.0f, true, 1.0f, 1.0f);
	CHECK(claim.scroll_value_changed && claim.scroll_value == 2 &&
					state.widgets[0].scroll_row == 2,
			"the captured popup drag lands the clamped last first-row");
	c.pump_mouse(state, 90.0f, 100.0f, false, 1.0f, 1.0f);

	// The popup-exclusive entry: restricted to the open combo, it claims the
	// scrollbar parts and nothing else — a press on the row strip flows back
	// to the caller's row picking, and other widgets can never claim.
	state.widgets[0].scroll_row = 0;
	claim = c.pump_popup_mouse(state, 1, 90.0f, 90.0f, true, 1.0f, 1.0f);
	CHECK(claim.scroll_index == 1 && claim.scroll_value_changed &&
					claim.scroll_value == 1 && state.widgets[0].scroll_row == 1,
			"pump_popup_mouse steps the popup down arrow");
	c.pump_popup_mouse(state, 1, 90.0f, 90.0f, false, 1.0f, 1.0f);
	claim = c.pump_popup_mouse(state, 1, 50.0f, 55.0f, true, 1.0f, 1.0f);
	CHECK(claim.scroll_index == -1,
			"a row-strip press flows past the popup pump to row picking");
	c.pump_popup_mouse(state, 1, 50.0f, 55.0f, false, 1.0f, 1.0f);

	// A CLOSED combo exposes no scroll parts to the pump.
	state.widgets[0].popup_open = false;
	CHECK(c.scroll_owner_at(state, 90.0f, 90.0f, 1.0f, 1.0f) == -1,
			"a closed combo's popup scrollbar is not claimable");
	claim = c.pump_popup_mouse(state, 1, 90.0f, 90.0f, true, 1.0f, 1.0f);
	CHECK(claim.scroll_index == -1,
			"the popup pump refuses a closed combo's scrollbar strip");
	c.pump_popup_mouse(state, 1, 90.0f, 90.0f, false, 1.0f, 1.0f);
}

// Wheel ticks (D-MNU-18, deliberate divergence — retail ships no functioning
// menu wheel scroll; witness map at pump_mouse_wheel): one tick = one row,
// the open popup consumes ticks exclusively wherever the cursor sits, a
// closed overflowing list scrolls only under the point, and rows clamp.
void test_wheel_ticks_scroll_popup_and_row_owners(const fnt_font_t *font) {
	const char *xml = R"(
<SCREEN>
  <NAME>WHEELY</NAME>
  <WINDOW type="window" name="ROOT">
    <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>
    <FONT><NAME>f.fnt</NAME><DEFAULT_FG>FFFFFF</DEFAULT_FG></FONT>
    <WINDOW type="list" name="ROSTER">
      <POSITION><LEFT>400</LEFT><TOP>100</TOP><RIGHT>600</RIGHT><BOTTOM>140</BOTTOM></POSITION>
      <ITEMS>
        <ITEM type="ID" value="0">A</ITEM><ITEM type="ID" value="1">B</ITEM>
        <ITEM type="ID" value="2">C</ITEM><ITEM type="ID" value="3">D</ITEM>
      </ITEMS>
      <MIN_ITEM_HEIGHT>20</MIN_ITEM_HEIGHT>
    </WINDOW>
    <WINDOW type="combo" name="COMBO">
      <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>100</RIGHT><BOTTOM>20</BOTTOM></POSITION>
      <ITEMS>
        <ITEM type="ID" value="0">ZERO</ITEM><ITEM type="ID" value="1">ONE</ITEM>
        <ITEM type="ID" value="2">TWO</ITEM><ITEM type="ID" value="3">THREE</ITEM>
        <ITEM type="ID" value="4">FOUR</ITEM><ITEM type="ID" value="5">FIVE</ITEM>
      </ITEMS>
      <LIST_BOX>
        <POSITION><LEFT>0</LEFT><TOP>20</TOP><RIGHT>100</RIGHT><BOTTOM>100</BOTTOM></POSITION>
        <MIN_ITEM_HEIGHT>20</MIN_ITEM_HEIGHT>
      </LIST_BOX>
    </WINDOW>
  </WINDOW>
</SCREEN>
)";
	opennova::mnu::Document doc = parse_or_die(xml);
	MenuFrameCompiler c;
	c.configure(doc.first_screen(), font);
	MenuFrameState state;

	// The 4-row list shows 2 rows: wheel over it scrolls, clamped 0..2.
	MenuFrameCompiler::MouseClaim claim;
	CHECK(c.pump_mouse_wheel(state, 500.0f, 120.0f, 1, 1.0f, 1.0f, &claim),
			"a wheel tick over the overflowing list claims");
	CHECK(claim.scroll_index == 1 && claim.scroll_value_changed &&
					claim.scroll_value == 1,
			"one down tick scrolls the list one row");
	claim = MenuFrameCompiler::MouseClaim();
	c.pump_mouse_wheel(state, 500.0f, 120.0f, 5, 1.0f, 1.0f, &claim);
	CHECK(claim.scroll_value == 2, "the down tick clamps at rows - visible");
	claim = MenuFrameCompiler::MouseClaim();
	CHECK(!c.pump_mouse_wheel(state, 200.0f, 300.0f, 1, 1.0f, 1.0f, &claim),
			"a tick over nothing scrollable claims no one");
	claim = MenuFrameCompiler::MouseClaim();
	c.pump_mouse_wheel(state, 500.0f, 120.0f, -9, 1.0f, 1.0f, &claim);
	CHECK(claim.scroll_value_changed && claim.scroll_value == 0,
			"the up tick clamps at row zero");

	// An OPEN popup consumes the tick exclusively — even over the list.
	MenuWidgetState combo;
	combo.index = 2;
	combo.popup_open = true;
	state.widgets.push_back(combo);
	claim = MenuFrameCompiler::MouseClaim();
	CHECK(c.pump_mouse_wheel(state, 500.0f, 120.0f, 1, 1.0f, 1.0f, &claim),
			"the open popup claims the tick");
	CHECK(claim.scroll_index == 2 && claim.scroll_value == 1,
			"the popup rows scroll instead of the list under the cursor");
	// 6 rows, 4 visible: popup clamps at 2.
	claim = MenuFrameCompiler::MouseClaim();
	c.pump_mouse_wheel(state, 500.0f, 120.0f, 9, 1.0f, 1.0f, &claim);
	CHECK(claim.scroll_value == 2, "the popup clamps at its own row limit");
}

// A list shorter than one full row holding a single item must not draw a
// scrollbar its own interaction refuses to hit: the draw walk and the
// interaction solve share scroll_row_span_'s min-1 visible clamp.
void test_degenerate_list_draws_no_dead_scrollbar(const fnt_font_t *font) {
	const char *xml = R"(
<SCREEN>
  <NAME>SHORT_LIST</NAME>
  <WINDOW type="window" name="ROOT">
    <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>
    <FONT><NAME>f.fnt</NAME><DEFAULT_FG>FFFFFF</DEFAULT_FG></FONT>
    <WINDOW type="list" name="SHORTY">
      <POSITION><LEFT>100</LEFT><TOP>100</TOP><RIGHT>300</RIGHT><BOTTOM>110</BOTTOM></POSITION>
      <ITEMS>
        <ITEM type="ID" value="0">ONLY</ITEM>
      </ITEMS>
      <MIN_ITEM_HEIGHT>20</MIN_ITEM_HEIGHT>
      <SCROLLBAR>
        <APPEARANCE type="color" state="default">303030</APPEARANCE>
        <SHUTTLE type="color" state="default">80FF0000</SHUTTLE>
        <SCROLLUP type="color" state="default">505050</SCROLLUP>
        <SCROLLDOWN type="color" state="default">505050</SCROLLDOWN>
      </SCROLLBAR>
    </WINDOW>
  </WINDOW>
</SCREEN>
)";
	opennova::mnu::Document doc = parse_or_die(xml);
	MenuFrameCompiler c;
	c.configure(doc.first_screen(), font);
	MenuFrameState state;
	const MenuDrawList &dl = c.compile(state, 1.0f, 1.0f);
	bool shuttle_drawn = false;
	for (const MenuQuad &q : dl.quads) {
		if (q.color == 0x80FF0000u) {
			shuttle_drawn = true;
		}
	}
	CHECK(!shuttle_drawn,
			"the one-visible-row clamp hides the single-item scrollbar");
	CHECK(c.scroll_owner_at(state, 290.0f, 105.0f, 1.0f, 1.0f) == -1,
			"the interaction solve agrees with the draw gate");
}

} // namespace

int main() {
	fnt_font_t font = make_font();
	test_draw_order_and_state_selection(&font);
	test_image_appearance_crops_authored_map_state(&font);
	test_scroll_draws_authored_visual_parts(&font);
	test_spin_arrow_uses_cropped_atlas_extent(&font);
	test_list_scrollbar_uses_authored_geometry_and_range(&font);
	test_combo_scrollbar_offsets_rows_and_hit(&font);
	test_table_scrollbar_separates_header_and_body_row_heights(&font);
	test_table_visible_count_floors_to_one(&font);
	test_table_rows_draw_row_state_not_widget_hover(&font);
	test_text_placement_and_truncation(&font);
	test_scale_truncation(&font);
	test_radio_checkbox_forcing(&font);
	test_edit_caret(&font);
	test_list_rows_and_item_cell(&font);
	test_mouse_pump(&font);
	test_table_interior(&font);
	test_marquee_roll(&font);
	test_draw_frame_gate(&font);
	test_edit_input_ops();
	test_runtime_items_and_multiselect(&font);
	test_widget_queries(&font);
	test_interaction_geometry(&font);
	test_combo_face_and_outside_arrow_claim(&font);
	test_hotkey_widget(&font);
	test_multiline_wrap(&font);
	test_draw_list_preserves_interleaved_primitive_order(&font);
	test_scroll_interaction_hits_and_drag(&font);
	test_scroll_pump_owns_press_capture_and_value(&font);
	test_combo_popup_scrollbar_scrolls_through_pump(&font);
	test_wheel_ticks_scroll_popup_and_row_owners(&font);
	test_degenerate_list_draws_no_dead_scrollbar(&font);
	test_table_embedded_scrollbar_scrolls_rows(&font);
	test_combo_face_shows_list_box_selection(&font);
	test_open_combo_popup_draws_over_later_widgets(&font);
	fnt_free(&font);
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("menu_frame_compiler_test: all checks passed\n");
	return 0;
}
