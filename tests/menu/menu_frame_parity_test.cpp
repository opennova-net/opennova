// The frame compiler's retail rules from the 2026-09-23 IDA grill (sets A, C, D):
// the per-widget string tables, the cursor, no default font, the APPEARANCE rows,
// the IMAGE band and the texture's first load, the rect's image extents, the label
// colour, the spin arrows as buttons, the table columns and the sort indicator, the
// marquee credits, and the menu texture / font file choice; and from sets B and C
// the input geometry: the label mnemonics by class, the table hit test, the open
// popup's pump and the runtime enabled flag.
// [orig: CWnd_GetInheritedTextRsrc @ 0x646AB0; CUIStringTable_LookupString
//  @ 0x6527c0; CWnd_GetFontAndColors @ 0x646a70; CUIElement_ParseXMLDefinition
//  @ 0x648120; CWnd_ProcessMouseEvent @ 0x647a00; Render_DrawTiledTextureStrip
//  @ 0x67aed0; CUITable_Render @ 0x6411d0; CMarqueeWnd_LoadCreditsFromIni
//  @ 0x65c5a0; CTextureManager_LoadOrFindTexture @ 0x654980]
// Witness record: docs/mnu/menu-re.md.

#include <formats/mnu/mnu.h>
#include <formats/pcx/pcx_io.h>
#include <runtime/menu/menu_assets.h>
#include <runtime/menu/menu_credits.h>
#include <runtime/menu/menu_frame.h>
#include <runtime/menu/menu_screen_inputs.h>
#include <runtime/menu/menu_text_tables.h>

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "common/test_font.h"

using namespace opennova::fnt;
using opennova::menu::MarqueeCredits;
using opennova::menu::MenuDrawList;
using opennova::menu::MenuFrameCompiler;
using opennova::menu::MenuTableRow;
using opennova::menu::MenuFrameState;
using opennova::menu::MenuQuad;
using opennova::menu::MenuTextTables;
using opennova::menu::MenuWidgetState;
using opennova::menu::kMenuTexNone;

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

opennova::mnu::Document parse_or_die(const std::string &xml) {
	opennova::mnu::Document doc;
	std::string err;
	if (!opennova::mnu::parse(xml, doc, err)) {
		std::fprintf(stderr, "FAIL: fixture parse: %s\n", err.c_str());
		++failures;
	}
	return doc;
}

std::shared_ptr<opennova::rtxt::File> table_of(
		std::initializer_list<std::pair<const char *, std::vector<std::pair<const char *, const char *>>>> sections) {
	auto file = std::make_shared<opennova::rtxt::File>();
	uint32_t index = 0;
	for (const auto &section : sections) {
		file->sections.push_back({ section.first, static_cast<uint32_t>(section.second.size()) });
		for (const auto &row : section.second) {
			opennova::rtxt::Entry entry;
			entry.key = row.first;
			entry.text = row.second;
			entry.section_index = index;
			file->entries.push_back(entry);
		}
		++index;
	}
	return file;
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

const MenuQuad *quad_with(const MenuDrawList &dl, int32_t slot) {
	for (const MenuQuad &q : dl.quads) {
		if (q.texture == slot) {
			return &q;
		}
	}
	return nullptr;
}

std::set<uint32_t> glyph_colors(const MenuDrawList &dl) {
	std::set<uint32_t> out;
	for (const auto &g : dl.glyphs) {
		out.insert(g.color & 0xFFFFFFu);
	}
	return out;
}

const char *kPos = "<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>";

std::string screen(const std::string &root_body) {
	return "<SCREEN><NAME>S</NAME><WINDOW type=\"window\" name=\"ROOT\">" + std::string(kPos) +
			root_body + "</WINDOW></SCREEN>";
}

std::string button(const char *name, const std::string &body,
		const char *pos = "<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>200</RIGHT><BOTTOM>20</BOTTOM></POSITION>") {
	return std::string("<WINDOW type=\"button\" name=\"") + name + "\">" + pos + body + "</WINDOW>";
}

// ---- string tables ---------------------------------------------------------

// Own-or-root: a nested window's TEXT_RSRC reads for its own ids only; its
// children read the ROOT's; section "menu" only (the first), the override first,
// and no table means the raw key [orig: CWnd_GetInheritedTextRsrc @ 0x646AB0;
// CUIStringTable_LookupString @ 0x6527c0; TextResource_FindEntryBySectionAndKey
// @ 0x75d250].
void test_text_tables(const fnt_font_t *font) {
	const std::string xml = screen(
			"<FONT><NAME>f.fnt</NAME></FONT><TEXT_RSRC>root.bin</TEXT_RSRC>" +
			button("A", "<STRING type=\"id\">K</STRING>") +
			"<WINDOW type=\"static\" name=\"PANEL\">" + kPos + "<TEXT_RSRC>panel.bin</TEXT_RSRC>" +
			button("B", "<STRING type=\"id\">K</STRING>") + "<STRING type=\"id\">K</STRING></WINDOW>" +
			button("C", "<STRING type=\"id\">OTHER</STRING>") +
			button("D", "<STRING type=\"id\">SECOND</STRING>") +
			button("E", "<STRING type=\"id\">OVER</STRING>"));
	opennova::mnu::Document doc = parse_or_die(xml);
	MenuTextTables tables;
	tables.set_table("root.bin", table_of({ { "Menu", { { "K", "root" }, { "OVER", "root-over" } } },
			{ "Other", { { "OTHER", "other" } } }, { "menu", { { "SECOND", "second" } } } }));
	tables.set_table("PANEL.BIN", table_of({ { "MENU", { { "K", "panel" } } } }));
	const auto over = table_of({ { "Menu", { { "OVER", "expansion" } } } });
	tables.set_override(over.get());
	MenuFrameCompiler c;
	c.register_font("f.fnt", font);
	c.set_text_tables(&tables);
	c.configure(doc.first_screen());
	// 0 ROOT, 1 A, 2 PANEL, 3 B, 4 C, 5 D, 6 E
	CHECK(c.widget_authored_text(1) == "root", "a widget with no table reads its root's");
	CHECK(c.widget_authored_text(2) == "panel", "a nested TEXT_RSRC reads for the window itself");
	CHECK(c.widget_authored_text(3) == "root", "its child reads the ROOT's, not its parent's");
	CHECK(c.widget_authored_text(4) == "OTHER", "a key outside section menu shows raw");
	CHECK(c.widget_authored_text(5) == "SECOND", "only the first menu section is searched");
	CHECK(c.widget_authored_text(6) == "expansion", "the override table is searched first");
	// No table at all: the raw key.
	opennova::mnu::Document bare = parse_or_die(screen(button("A", "<STRING type=\"id\">K</STRING>")));
	MenuFrameCompiler b;
	b.set_text_tables(&tables);
	b.configure(bare.first_screen());
	CHECK(b.widget_authored_text(1) == "K", "no TEXT_RSRC up the chain: the key");
	// A table that did not load: the key, and the override is not consulted.
	MenuTextTables missing;
	missing.set_table("root.bin", nullptr);
	missing.set_override(over.get());
	opennova::mnu::Document one = parse_or_die(screen("<TEXT_RSRC>root.bin</TEXT_RSRC>" +
			button("A", "<STRING type=\"id\">OVER</STRING>")));
	MenuFrameCompiler m;
	m.set_text_tables(&missing);
	m.configure(one.first_screen());
	CHECK(m.widget_authored_text(1) == "OVER", "a table that did not load gives the key");
	CHECK(opennova::menu::screen_text_rsrc_names(*doc.first_screen()) ==
					std::vector<std::string>({ "root.bin", "panel.bin" }),
			"the screen's tables, each once");
}

// ---- cursor ------------------------------------------------------------------

// Own-or-root [orig: CWnd_GetInheritedCursorTexture @ 0x646AD0 and its inlined
// copy @ 0x647ad0]: a nested window's CURSOR is its own only; its children show
// the root's; a widget whose root has none shows the first root with one
// [orig: CUIScene_EndFrame @ 0x63e600]. The handles are what loaded
// [orig: CTextureManager_LoadOrFindTexture @ 0x654980 zeroes a failed load's]:
// a CURSOR whose texture did not load falls through the same way. A spin arrow
// is a button of its own: over it, the arrow's cursor, not the list's.
void test_cursor() {
	const std::string xml =
			"<SCREEN><NAME>S</NAME>"
			"<WINDOW type=\"window\" name=\"R0\"><POSITION><LEFT>0</LEFT><TOP>500</TOP><RIGHT>10</RIGHT><BOTTOM>510</BOTTOM></POSITION>"
			"<CURSOR><FILE>gone.tga</FILE></CURSOR></WINDOW>"
			"<WINDOW type=\"window\" name=\"R1\"><POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>400</RIGHT><BOTTOM>400</BOTTOM></POSITION>"
			"<CURSOR><FILE>r1.tga</FILE></CURSOR>"
			"<WINDOW type=\"window\" name=\"N\"><POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>200</RIGHT><BOTTOM>200</BOTTOM></POSITION>"
			"<CURSOR><FILE>n.tga</FILE></CURSOR>"
			"<WINDOW type=\"window\" name=\"C\"><POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>50</RIGHT><BOTTOM>50</BOTTOM></POSITION></WINDOW>"
			"</WINDOW>"
			"<WINDOW type=\"window\" name=\"M\"><POSITION><LEFT>250</LEFT><TOP>0</TOP><RIGHT>300</RIGHT><BOTTOM>50</BOTTOM></POSITION>"
			"<CURSOR><FILE>gone.tga</FILE></CURSOR></WINDOW>"
			"<WINDOW type=\"spinlist\" name=\"SPIN\"><POSITION><LEFT>100</LEFT><TOP>300</TOP><RIGHT>200</RIGHT><BOTTOM>320</BOTTOM></POSITION>"
			"<CURSOR><FILE>list.tga</FILE></CURSOR>"
			"<SPINUP><POSITION><LEFT>-20</LEFT><TOP>0</TOP><RIGHT>0</RIGHT><BOTTOM>20</BOTTOM></POSITION></SPINUP>"
			"<SPINDOWN><POSITION><LEFT>100</LEFT><TOP>0</TOP><RIGHT>120</RIGHT><BOTTOM>20</BOTTOM></POSITION>"
			"<CURSOR><FILE>down.tga</FILE></CURSOR></SPINDOWN>"
			"</WINDOW></WINDOW>"
			"<WINDOW type=\"window\" name=\"R2\"><POSITION><LEFT>400</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>400</BOTTOM></POSITION>"
			"<WINDOW type=\"window\" name=\"W\"><POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>50</RIGHT><BOTTOM>50</BOTTOM></POSITION></WINDOW>"
			"</WINDOW></SCREEN>";
	opennova::mnu::Document doc = parse_or_die(xml);
	MenuFrameCompiler c;
	c.configure(doc.first_screen());
	const int32_t r1 = slot_of(c, "r1.tga");
	const int32_t n = slot_of(c, "n.tga");
	const int32_t list = slot_of(c, "list.tga");
	const int32_t down = slot_of(c, "down.tga");
	for (const int32_t slot : { r1, n, list, down }) {
		c.set_texture_size(slot, 16, 16);
	}
	MenuFrameState st;
	CHECK(c.pump_mouse(st, 100.0f, 100.0f, false, 1.0f, 1.0f).cursor == n, "a nested CURSOR is the window's own");
	CHECK(c.pump_mouse(st, 10.0f, 10.0f, false, 1.0f, 1.0f).cursor == r1,
			"its child shows the root's, not its parent's");
	CHECK(c.pump_mouse(st, 260.0f, 10.0f, false, 1.0f, 1.0f).cursor == r1,
			"a CURSOR that did not load falls to the root's");
	CHECK(c.pump_mouse(st, 410.0f, 10.0f, false, 1.0f, 1.0f).cursor == r1,
			"a root with none: the first root whose cursor loaded");
	CHECK(c.pump_mouse(st, 150.0f, 305.0f, false, 1.0f, 1.0f).cursor == list, "the spin list's own");
	const MenuFrameCompiler::MouseClaim up = c.pump_mouse(st, 85.0f, 305.0f, false, 1.0f, 1.0f);
	CHECK(up.spin_part == 1 && up.cursor == r1,
			"over the SPINUP (none of its own): its root's, not the list's");
	const MenuFrameCompiler::MouseClaim dn = c.pump_mouse(st, 205.0f, 305.0f, false, 1.0f, 1.0f);
	CHECK(dn.spin_part == 2 && dn.cursor == down, "over the SPINDOWN: its own");
	st.cursor_visible = true;
	st.cursor_x = 205.0f;
	st.cursor_y = 305.0f;
	const MenuDrawList &dl = c.compile(st, 1.0f, 1.0f);
	CHECK(quad_with(dl, down) != nullptr, "the cursor pass draws the arrow's cursor");
}

// ---- fonts -----------------------------------------------------------------

// No default font: a widget draws with the nearest self-or-ancestor FONT that
// loaded, in THAT widget's colors; none up the chain draws and measures nothing
// [orig: CWnd_GetFontAndColors @ 0x646a70; font_cache_ensure_font_loaded
// @ 0x653de0].
void test_fonts(const fnt_font_t *font) {
	const std::string xml = screen(
			"<FONT><NAME>f.fnt</NAME><DEFAULT_FG>FF0000</DEFAULT_FG></FONT>" +
			button("MISSING", "<FONT><NAME>gone.fnt</NAME><DEFAULT_FG>00FF00</DEFAULT_FG></FONT><STRING>A</STRING>") +
			button("NAMELESS", "<FONT><DEFAULT_FG>0000FF</DEFAULT_FG></FONT><STRING>B</STRING>",
					"<POSITION><LEFT>0</LEFT><TOP>30</TOP><RIGHT>200</RIGHT><BOTTOM>50</BOTTOM></POSITION>"));
	opennova::mnu::Document doc = parse_or_die(xml);
	MenuFrameCompiler c;
	c.register_font("f.fnt", font);
	c.configure(doc.first_screen());
	MenuFrameState st;
	const MenuDrawList &dl = c.compile(st, 1.0f, 1.0f);
	const std::set<uint32_t> colors = glyph_colors(dl);
	CHECK(dl.glyphs.size() == 2, "both labels draw with the root's font");
	CHECK(colors == std::set<uint32_t>({ text_rgb(0xFF0000u) }),
			"a FONT that did not load (or has no NAME) takes the ancestor's colors too");
	// No font anywhere: nothing drawn, and a text-sized rect measures nothing.
	opennova::mnu::Document none = parse_or_die(screen(button("L", "<STRING>LABEL</STRING>",
			"<POSITION><LEFT>10</LEFT><TOP>10</TOP></POSITION>")));
	MenuFrameCompiler n;
	n.configure(none.first_screen());
	const MenuDrawList &dl2 = n.compile(st, 1.0f, 1.0f);
	opennova::mnu::RectEdges rect;
	n.widget_rect(1, st, &rect);
	CHECK(dl2.glyphs.empty(), "no font up the chain: no text");
	CHECK(rect.right == 10 && rect.bottom == 10, "and nothing measured");
	// The .fnt a name loads [orig: String_ReplaceOrAppendExtension @ 0x64fe60].
	CHECK(opennova::menu::menu_font_file("arial18") == "arial18.fnt", ".fnt appended");
	CHECK(opennova::menu::menu_font_file("Gunpl22b.fnt") == "Gunpl22b.fnt", "kept");
	CHECK(opennova::menu::menu_font_file("a.b.fnt") == "a.fnt", "replaced from the first dot");
}

// ---- appearance ------------------------------------------------------------

// The rows [orig: CUIElement_ParseXMLDefinition @ 0x6483d4..0x648634]: an
// unknown STATE sets nothing; a missing TYPE is an availability marker; flags OR
// and each type's last value wins; COLOR / OUTLINE are wcstoul words; CUSTOM
// draws nothing here.
void test_appearance_rows(const fnt_font_t *font) {
	const std::string xml = screen(
			"<FONT><NAME>f.fnt</NAME></FONT>" +
			button("A", "<APPEARANCE state=\"default\" type=\"image\">a.tga</APPEARANCE>"
						"<APPEARANCE state=\"default\">b.tga</APPEARANCE>"
						"<APPEARANCE state=\"default\" type=\"color\">102030</APPEARANCE>"
						"<APPEARANCE state=\"mouseover\">marker.tga</APPEARANCE>"
						"<APPEARANCE state=\"selected\" type=\"custom\">7f3f0000</APPEARANCE>") +
			button("B", "<APPEARANCE state=\"default\" type=\"color\">#FF0000</APPEARANCE>"
						"<APPEARANCE state=\"default\" type=\"outline\">FF00ff00zz</APPEARANCE>",
					"<POSITION><LEFT>0</LEFT><TOP>30</TOP><RIGHT>200</RIGHT><BOTTOM>50</BOTTOM></POSITION>"));
	opennova::mnu::Document doc = parse_or_die(xml);
	MenuFrameCompiler c;
	c.register_font("f.fnt", font);
	c.configure(doc.first_screen());
	const int32_t a = slot_of(c, "a.tga");
	CHECK(a >= 0 && slot_of(c, "b.tga") < 0 && slot_of(c, "marker.tga") < 0,
			"a typeless row loads no texture");
	c.set_texture_size(a, 32, 32);
	MenuFrameState st;
	const MenuDrawList &dl = c.compile(st, 1.0f, 1.0f);
	const MenuQuad *img = quad_with(dl, a);
	CHECK(img != nullptr, "the IMAGE survives a later typeless row of its state");
	bool fill = false;
	bool red = false;
	for (const MenuQuad &q : dl.quads) {
		if (q.texture == kMenuTexNone && q.color == 0x00102030u) fill = true;
		if (q.texture == kMenuTexNone && (q.color & 0xFFFFFFu) == 0xFF0000u) red = true;
	}
	CHECK(fill, "COLOR and IMAGE both draw; six digits leave alpha 0");
	CHECK(!red, "'#' reads 0");
	bool outline = false;
	for (const auto &l : dl.lines) {
		if (l.color == 0xFF00FF00u) outline = true;
	}
	CHECK(outline, "a partly valid OUTLINE keeps its prefix");
	// Hover: MOUSEOVER is authored (a marker): kept, drawing nothing.
	MenuWidgetState hover;
	hover.index = 1;
	hover.hovered = true;
	st.widgets.push_back(hover);
	const MenuDrawList &dl2 = c.compile(st, 1.0f, 1.0f);
	CHECK(quad_with(dl2, a) == nullptr, "an authored marker state is kept (no DEFAULT fallback)");
	// Pressed: SELECTED is authored as CUSTOM: nothing drawn here.
	st.widgets[0].hovered = false;
	st.widgets[0].pressed = true;
	const MenuDrawList &dl3 = c.compile(st, 1.0f, 1.0f);
	CHECK(quad_with(dl3, a) == nullptr, "CUSTOM draws nothing in the compiler");
}

// The label color follows the resolved state [orig: the pump's fallback
// @ 0x647c89..0x647cb5; CStaticWnd_DrawLabel's color switch @ 0x653445]: a
// static with only DEFAULT authored stays in the default pair under the mouse
// (the shipped red-hover case); a checked radio forces SELECTED with no
// fallback; a checkbox with no state at all draws no checked art.
void test_label_colors(const fnt_font_t *font) {
	const std::string xml = screen(
			"<FONT><NAME>f.fnt</NAME><DEFAULT_FG>FFFFFF</DEFAULT_FG><MOUSEOVER_FG>FF0000</MOUSEOVER_FG>"
			"<SELECTED_FG>00FF00</SELECTED_FG></FONT>" +
			std::string("<WINDOW type=\"static\" name=\"S\"><POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>200</RIGHT><BOTTOM>20</BOTTOM></POSITION>"
						"<APPEARANCE state=\"default\"></APPEARANCE><STRING>PLAY</STRING></WINDOW>") +
			"<WINDOW type=\"radio\" name=\"R\" CHECKED><POSITION><LEFT>0</LEFT><TOP>30</TOP><RIGHT>200</RIGHT><BOTTOM>50</BOTTOM></POSITION>"
			"<APPEARANCE state=\"default\" type=\"image\">r.tga</APPEARANCE><STRING>ON</STRING></WINDOW>"
			"<WINDOW type=\"checkbox\" name=\"C\" CHECKED><POSITION><LEFT>0</LEFT><TOP>60</TOP><RIGHT>20</RIGHT><BOTTOM>80</BOTTOM></POSITION>"
			"<APPEARANCE state=\"selected\" type=\"image\">c.tga</APPEARANCE></WINDOW>");
	opennova::mnu::Document doc = parse_or_die(xml);
	MenuFrameCompiler c;
	c.register_font("f.fnt", font);
	c.configure(doc.first_screen());
	const int32_t r = slot_of(c, "r.tga");
	const int32_t ck = slot_of(c, "c.tga");
	c.set_texture_size(r, 8, 8);
	c.set_texture_size(ck, 8, 8);
	MenuFrameState st;
	MenuWidgetState hover;
	hover.index = 1;
	hover.hovered = true;
	st.widgets.push_back(hover);
	const MenuDrawList &dl = c.compile(st, 1.0f, 1.0f);
	std::set<uint32_t> static_colors;
	std::set<uint32_t> radio_colors;
	for (const auto &g : dl.glyphs) {
		(g.y_top < 25.0f ? static_colors : radio_colors).insert(g.color & 0xFFFFFFu);
	}
	CHECK(static_colors == std::set<uint32_t>({ text_rgb(0xFFFFFFu) }),
			"a hovered static with no MOUSEOVER row keeps the default color");
	CHECK(radio_colors == std::set<uint32_t>({ text_rgb(0x00FF00u) }), "a checked radio draws the selected pair");
	CHECK(quad_with(dl, r) == nullptr, "and its unauthored SELECTED slot draws nothing");
	CHECK(quad_with(dl, ck) == nullptr,
			"a checkbox whose state resolves to none draws no checked art");
}

// ---- the IMAGE band and the texture's first load -----------------------------

// [orig: Render_DrawTiledTextureStrip @ 0x67aed0 — the band [MS*H, (MS+1)*H), nothing
// when it leaves the texture; CTextureManager_LoadOrFindTexture @ 0x654980 bakes
// the first load's HEIGHT]
void test_image_band(const fnt_font_t *font) {
	const std::string xml = screen(
			"<FONT><NAME>f.fnt</NAME></FONT>" +
			button("MS_ONLY", "<APPEARANCE state=\"default\" type=\"image\" map_state=\"1\">m.tga</APPEARANCE>") +
			button("H_ONLY", "<APPEARANCE state=\"default\" type=\"image\" height=\"20\">h.tga</APPEARANCE>",
					"<POSITION><LEFT>0</LEFT><TOP>30</TOP><RIGHT>200</RIGHT><BOTTOM>50</BOTTOM></POSITION>") +
			button("FIRST", "<APPEARANCE state=\"default\" type=\"image\" map_state=\"0\" height=\"20\">s.tga</APPEARANCE>",
					"<POSITION><LEFT>0</LEFT><TOP>60</TOP><RIGHT>200</RIGHT><BOTTOM>80</BOTTOM></POSITION>") +
			button("LATER", "<APPEARANCE state=\"default\" type=\"image\" map_state=\"2\" height=\"24\">s.tga</APPEARANCE>",
					"<POSITION><LEFT>0</LEFT><TOP>90</TOP><RIGHT>200</RIGHT><BOTTOM>110</BOTTOM></POSITION>") +
			button("ZERO", "<APPEARANCE state=\"default\" type=\"image\" height=\"0\">z.tga</APPEARANCE>",
					"<POSITION><LEFT>0</LEFT><TOP>120</TOP><RIGHT>200</RIGHT><BOTTOM>140</BOTTOM></POSITION>"));
	opennova::mnu::Document doc = parse_or_die(xml);
	MenuFrameCompiler c;
	c.register_font("f.fnt", font);
	c.configure(doc.first_screen());
	for (const char *name : { "m.tga", "h.tga", "s.tga", "z.tga" }) {
		c.set_texture_size(slot_of(c, name), 32, 80);
	}
	MenuFrameState st;
	const MenuDrawList &dl = c.compile(st, 1.0f, 1.0f);
	CHECK(quad_with(dl, slot_of(c, "m.tga")) == nullptr, "MAP_STATE 1 with no HEIGHT draws nothing");
	const MenuQuad *h = quad_with(dl, slot_of(c, "h.tga"));
	CHECK(h != nullptr && h->v0 == 0.0f && h->v1 == 0.25f, "a HEIGHT alone crops row 0");
	int s_quads = 0;
	for (const MenuQuad &q : dl.quads) {
		if (q.texture == slot_of(c, "s.tga")) {
			++s_quads;
			if (q.y0 >= 90.0f) {
				CHECK(q.v0 == 0.5f && q.v1 == 0.75f,
						"a later row draws the band the texture's first load baked (20, not 24)");
			}
		}
	}
	CHECK(s_quads == 2, "both users of the shared texture draw");
	const MenuQuad *z = quad_with(dl, slot_of(c, "z.tga"));
	CHECK(z != nullptr && z->v0 == 0.0f && z->v1 == 1.0f / 80.0f, "a HEIGHT of 0 stretches texel row 0");
	// The texture loads persist: a second document reads the first's height.
	opennova::mnu::Document second = parse_or_die(screen(button("X",
			"<APPEARANCE state=\"default\" type=\"image\" map_state=\"1\" height=\"30\">s.tga</APPEARANCE>")));
	c.configure(second.first_screen());
	c.set_texture_size(slot_of(c, "s.tga"), 32, 80);
	const MenuDrawList &dl2 = c.compile(st, 1.0f, 1.0f);
	const MenuQuad *x = quad_with(dl2, slot_of(c, "s.tga"));
	CHECK(x != nullptr && x->v0 == 0.25f && x->v1 == 0.5f, "the first load's HEIGHT holds across screens");
	c.reset_texture_loads();
	c.configure(second.first_screen());
	c.set_texture_size(slot_of(c, "s.tga"), 32, 80);
	const MenuDrawList &dl3 = c.compile(st, 1.0f, 1.0f);
	const MenuQuad *y = quad_with(dl3, slot_of(c, "s.tga"));
	CHECK(y != nullptr && y->v0 == 30.0f / 80.0f, "until the loads are reset");
}

// The rect's image extents as parsed [orig: @ 0x6485cd..0x648634]: every IMAGE
// row counts, a later one of its state included; HEIGHT 0 adds nothing.
void test_rect_extents(const fnt_font_t *font) {
	const std::string xml = screen(
			"<FONT><NAME>f.fnt</NAME></FONT>"
			"<WINDOW type=\"window\" name=\"W\"><POSITION><LEFT>10</LEFT><TOP>10</TOP></POSITION>"
			"<APPEARANCE state=\"default\" type=\"image\">big.tga</APPEARANCE>"
			"<APPEARANCE state=\"default\" type=\"image\">small.tga</APPEARANCE></WINDOW>"
			"<WINDOW type=\"window\" name=\"Z\"><POSITION><LEFT>10</LEFT><TOP>10</TOP></POSITION>"
			"<APPEARANCE state=\"default\" type=\"image\" height=\"0\">small.tga</APPEARANCE></WINDOW>");
	opennova::mnu::Document doc = parse_or_die(xml);
	MenuFrameCompiler c;
	c.register_font("f.fnt", font);
	c.configure(doc.first_screen());
	c.set_texture_size(slot_of(c, "big.tga"), 64, 48);
	c.set_texture_size(slot_of(c, "small.tga"), 16, 8);
	MenuFrameState st;
	opennova::mnu::RectEdges w;
	opennova::mnu::RectEdges z;
	c.widget_rect(1, st, &w);
	c.widget_rect(2, st, &z);
	CHECK(w.right == 74 && w.bottom == 58, "the replaced row's larger extent still sizes the rect");
	CHECK(z.right == 26 && z.bottom == 10, "a HEIGHT of 0 adds no height");
}

// ---- spin arrows -------------------------------------------------------------

// The arrows are whole buttons [orig: CUISpinList_ParseXMLDefinition @ 0x64bd10;
// CSpinListWnd_CreateUpDownChildren @ 0x64b8b0]: their own states, their own
// STRING through their own table only, 0x0 with no art, text or far edges, and
// hit by rect alone.
void test_spin_arrows(const fnt_font_t *font) {
	const std::string xml = screen(
			"<FONT><NAME>f.fnt</NAME></FONT><TEXT_RSRC>root.bin</TEXT_RSRC>"
			"<WINDOW type=\"spinlist\" name=\"SPIN\"><POSITION><LEFT>100</LEFT><TOP>100</TOP><RIGHT>200</RIGHT><BOTTOM>120</BOTTOM></POSITION>"
			"<ITEMS><ITEM type=\"ID\" value=\"0\">K</ITEM></ITEMS>"
			"<SPINUP><POSITION><LEFT>-20</LEFT><TOP>0</TOP></POSITION>"
			"<APPEARANCE state=\"default\" type=\"image\" map_state=\"0\" height=\"20\">arrow.tga</APPEARANCE>"
			"<APPEARANCE state=\"mouseover\" type=\"image\" map_state=\"1\" height=\"20\">arrow.tga</APPEARANCE>"
			"</SPINUP>"
			"<SPINDOWN><POSITION><LEFT>110</LEFT><TOP>0</TOP></POSITION><FONT><NAME>f.fnt</NAME></FONT>"
			"<STRING type=\"id\">K</STRING></SPINDOWN>"
			"</WINDOW>"
			"<WINDOW type=\"spinlist\" name=\"BARE\"><POSITION><LEFT>100</LEFT><TOP>200</TOP><RIGHT>200</RIGHT><BOTTOM>220</BOTTOM></POSITION>"
			"<SPINUP><POSITION><LEFT>0</LEFT><TOP>0</TOP></POSITION><APPEARANCE state=\"default\"></APPEARANCE></SPINUP>"
			"<SPINDOWN><POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>20</RIGHT><BOTTOM>20</BOTTOM></POSITION>"
			"<APPEARANCE state=\"default\"></APPEARANCE></SPINDOWN>"
			"</WINDOW>");
	opennova::mnu::Document doc = parse_or_die(xml);
	MenuTextTables tables;
	tables.set_table("root.bin", table_of({ { "Menu", { { "K", "Item" } } } }));
	MenuFrameCompiler c;
	c.register_font("f.fnt", font);
	c.set_text_tables(&tables);
	c.configure(doc.first_screen());
	CHECK(c.widget_count() == 3, "the arrows are outside the document's index space");
	const int32_t arrow = slot_of(c, "arrow.tga");
	c.set_texture_size(arrow, 16, 80);
	MenuFrameState st;
	const MenuDrawList &dl = c.compile(st, 1.0f, 1.0f);
	const MenuQuad *up = quad_with(dl, arrow);
	CHECK(up != nullptr && up->x0 == 80.0f && up->v0 == 0.0f, "the SPINUP draws its DEFAULT row");
	// The SPINDOWN's STRING reads its own table (none): the raw key; the list's
	// item reads the root's.
	bool raw_key = false;
	bool item = false;
	for (const auto &g : dl.glyphs) {
		if (g.x_top_left >= 209.0f) raw_key = true;
		if (g.x_top_left > 100.0f && g.x_top_left < 200.0f) item = true;
	}
	CHECK(raw_key && item, "an arrow's STRING resolves through its own table only");
	// Hovering the arrow: its MOUSEOVER row; the list itself is not hovered.
	const MenuFrameCompiler::MouseClaim claim = c.pump_mouse(st, 85.0f, 105.0f, false, 1.0f, 1.0f);
	CHECK(claim.hovered == 1 && claim.spin_part == 1, "the arrow claims through its spin list");
	const MenuDrawList &dl2 = c.compile(st, 1.0f, 1.0f);
	const MenuQuad *hover = quad_with(dl2, arrow);
	CHECK(hover != nullptr && hover->v0 == 0.25f, "the hovered arrow draws its MOUSEOVER row");
	// An arrow with no art, text or far edges is 0x0 and never hit; one with its
	// far edges but no art still is.
	CHECK(c.spin_arrow_at(2, st, 101.0f, 201.0f, 1.0f, 1.0f) == 2,
			"an invisible arrow with a whole POSITION is hit");
	CHECK(c.spin_arrow_at(2, st, 130.0f, 210.0f, 1.0f, 1.0f) == 0, "outside both arrows");
	MenuFrameState none;
	const MenuFrameCompiler::MouseClaim bare = c.pump_mouse(none, 125.0f, 215.0f, false, 1.0f, 1.0f);
	CHECK(bare.spin_part == 0, "the 0x0 SPINUP is never hit");
}

// ---- tables ------------------------------------------------------------------

// [orig: CUITable_Render @ 0x6411d0; the HEADER walk @ 0x6427d0 -> init_table_row]
void test_table_columns(const fnt_font_t *font) {
	const std::string xml = screen(
			"<FONT><NAME>f.fnt</NAME><DEFAULT_FG>FFFFFF</DEFAULT_FG></FONT>"
			"<WINDOW type=\"table\" name=\"T\"><POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>300</RIGHT><BOTTOM>200</BOTTOM></POSITION>"
			"<FIXED_HEADER_HEIGHT>30</FIXED_HEADER_HEIGHT>"
			"<COLUMN count=\"4\">"
			"<HEADER column=\"1\" width=\"100\" justify=\"LEFT\">B</HEADER>"
			"<HEADER column=\"0\" width=\"50\" justify=\"LEFT\">A</HEADER>"
			"<HEADER column=\"2\" width=\"0\">HIDDEN</HEADER>"
			"<HEADER column=\"3\" width=\"400\">WIDE</HEADER>"
			"<HEADER column=\"9\" width=\"50\">NONE</HEADER>"
			"</COLUMN></WINDOW>");
	opennova::mnu::Document doc = parse_or_die(xml);
	MenuFrameCompiler c;
	c.register_font("f.fnt", font);
	c.configure(doc.first_screen());
	MenuFrameState st;
	const MenuDrawList &dl = c.compile(st, 1.0f, 1.0f);
	// Column 0 ("A") draws first at x 0, column 1 ("B") after it at 50; the width
	// 0 column and the one past the right edge draw nothing; index 9 is not set up.
	CHECK(dl.glyphs.size() == 2, "only the placed, fitting columns draw");
	if (dl.glyphs.size() == 2) {
		CHECK(dl.glyphs[0].x_top_left == -0.5f && dl.glyphs[1].x_top_left == 49.5f,
				"columns by index, not document order");
		CHECK(dl.glyphs[0].y_top == 6.5f, "a HEADER centres its label vertically by default");
	}
	CHECK(dl.lines.empty(), "no sort indicator on an unsorted table");
	// Sorted on column 0: the taper right of the label, one line a row.
	MenuWidgetState sorted;
	sorted.index = 1;
	sorted.table_sort_column = 0;
	sorted.table_rows = { MenuTableRow{ { "r0a", "r0b" } } };
	st.widgets.push_back(sorted);
	const MenuDrawList &dl2 = c.compile(st, 1.0f, 1.0f);
	int taper = 0;
	for (const auto &l : dl2.lines) {
		if (l.color == 0xFF7F7F7Fu && l.x0 >= 9.0f && l.x1 <= 25.0f) ++taper;
	}
	CHECK(taper == 8, "the sorted column draws the 8-row taper past its label");
	// The body starts under the FIXED_HEADER_HEIGHT.
	bool body = false;
	for (const auto &g : dl2.glyphs) {
		if (g.y_top == 29.5f) body = true;
	}
	CHECK(body, "the body starts under FIXED_HEADER_HEIGHT");
	// Installed columns replace the XML ones (a RESULTLIST with no HEADER).
	opennova::mnu::Document bare = parse_or_die(screen("<FONT><NAME>f.fnt</NAME></FONT>"
			"<WINDOW type=\"table\" name=\"R\"><POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>300</RIGHT><BOTTOM>200</BOTTOM></POSITION></WINDOW>"));
	MenuFrameCompiler r;
	r.register_font("f.fnt", font);
	r.configure(bare.first_screen());
	MenuFrameState rs;
	MenuWidgetState rows;
	rows.index = 1;
	rows.table_rows = { MenuTableRow{ { "ann", "7" } } };
	rs.widgets.push_back(rows);
	CHECK(r.compile(rs, 1.0f, 1.0f).glyphs.empty(), "a table with no HEADER draws no cells");
	opennova::menu::MenuTableColumn name;
	name.label = "Name";
	name.width = 150;
	opennova::menu::MenuTableColumn kills;
	kills.label = "K";
	kills.width = 100;
	rs.widgets[0].has_table_columns = true;
	rs.widgets[0].table_columns = { name, kills };
	rs.widgets[0].table_rows[0].flags |= opennova::menu::kTableRowFlagColor;
	rs.widgets[0].table_rows[0].color = 0xFF00BFFFu;
	const MenuDrawList &rl = r.compile(rs, 1.0f, 1.0f);
	CHECK(rl.glyphs.size() == 4 + 1 + 3 + 1, "the installed columns draw their labels and cells");
	bool team = false;
	for (const auto &g : rl.glyphs) {
		if ((g.color & 0xFFFFFFu) == text_rgb(0x00BFFFu)) team = true;
	}
	CHECK(team, "a row's colour override colours its cells");
}

// ---- marquee credits -----------------------------------------------------------

// [orig: CMarqueeWnd_LoadCreditsFromIni @ 0x65c5a0 over the ConfigFile text reader]
void test_marquee_credits() {
	const auto load = [](const char *text, MarqueeCredits &out) {
		return opennova::menu::marquee_load_credits(reinterpret_cast<const uint8_t *>(text),
				std::strlen(text), out, [](const std::string &name) { return name == "pic.tga"; });
	};
	MarqueeCredits plain;
	CHECK(load("[TEXT]\r\nTEXT=A_B@C, font.fnt\r\nTEXT=next\r\n", plain),
			"a text config loads");
	CHECK(plain.scroll_rate == 1.0f && plain.center_x == 400 && plain.vertical_space == 0,
			"no [ENV]: the defaults");
	CHECK(plain.nodes.size() == 2 && plain.nodes[1].font == "font.fnt", "the font carries to the next line");
	CHECK(opennova::menu::marquee_node_text(plain, plain.nodes[0]) == "A B,C", "the marks remap");
	MarqueeCredits env;
	CHECK(load("[ENV]\r\nCENTER_X=320\r\n[TEXT]\r\nTEXT=~CFF8000\r\nTEXT=~JR\r\nTEXT=x\r\n"
					"TEXT=~Ipic.tga\r\nTEXT=~Imissing.tga\r\nTEXT=~F10|20|pic.tga\r\n",
				  env),
			"loads");
	CHECK(env.scroll_rate == 0.0f && env.center_x == 320 && env.vertical_space == 0,
			"an [ENV] missing a key reads 0 for it");
	CHECK(env.nodes.size() == 3 && env.nodes[0].color == 0x00FF8000u && env.nodes[0].justify == 2,
			"~C and ~J set the next text node");
	CHECK(env.nodes.size() == 3 && env.nodes[1].image == "pic.tga" && !env.nodes[1].fades &&
					env.nodes[2].fades && env.nodes[2].fixed_x == 10 && env.nodes[2].fixed_y == 20,
			"an image node needs its texture to load; ~F fades at its point");
	MarqueeCredits cbin;
	CHECK(!load("CBIN....", cbin), "a CBIN config is the embedder's");
	MarqueeCredits lf;
	CHECK(load("[TEXT]\nTEXT=a\n", lf) && lf.nodes.empty(), "LF alone ends no line");
	MarqueeCredits lower;
	CHECK(load("[text]\r\nTEXT=a\r\n", lower) && lower.nodes.empty(), "a lowercase label is no section");
}

// ---- menu textures -------------------------------------------------------------

// [orig: CTextureManager_LoadOrFindTexture @ 0x654980; load_pcx_to_argb @ 0x664cc0]
void test_texture_files() {
	using opennova::menu::MenuTextureFormat;
	const auto has = [](const std::string &name) { return name == "here.tga"; };
	const auto tga = opennova::menu::menu_texture_source("here.tga", has);
	CHECK(tga.format == MenuTextureFormat::Tga && tga.file == "here.tga", "a .tga that exists");
	const auto dds = opennova::menu::menu_texture_source("a.b.tga", has);
	CHECK(dds.format == MenuTextureFormat::Dds && dds.file == "a.dds", "a missing .tga loads its .dds");
	CHECK(opennova::menu::menu_texture_source("x.PCX", has).format == MenuTextureFormat::Pcx, "PCX");
	CHECK(opennova::menu::menu_texture_source("x.png", has).format == MenuTextureFormat::Png, "PNG");
	CHECK(opennova::menu::menu_texture_source("x.bmp", has).format == MenuTextureFormat::None, "BMP: nothing");
	CHECK(opennova::menu::menu_texture_source("x", has).format == MenuTextureFormat::None, "no extension: nothing");
	CHECK(opennova::menu::menu_texture_source("x.dds", has, true).format == MenuTextureFormat::None,
			"a URL image has no DDS");
	// A 3x2 8-bit PCX: BytesPerLine 4, the palette fully opaque; a row's pad pixel
	// lands on the next row's start, which that row overwrites.
	std::vector<uint8_t> pcx(128, 0);
	pcx[0] = 0x0A;
	pcx[3] = 8;
	pcx[8] = 2;  // xmax
	pcx[10] = 1; // ymax
	pcx[0x41] = 1;
	pcx[0x42] = 4;
	for (uint8_t v : { 1, 2, 3, 9, 4, 5, 6, 9 }) pcx.push_back(v);
	std::vector<uint8_t> palette(768, 0);
	for (int i = 0; i < 256; ++i) {
		palette[3 * i] = static_cast<uint8_t>(i);
		palette[3 * i + 1] = static_cast<uint8_t>(i * 2);
		palette[3 * i + 2] = 7;
	}
	pcx.insert(pcx.end(), palette.begin(), palette.end());
	// The loader reads file size - 896 bytes of RLE data: the 8 above.
	opennova::RgbaImage image;
	std::string error;
	CHECK(opennova::decode_pcx_menu_rgba(pcx.data(), pcx.size(), image, error), "decodes");
	CHECK(image.width == 3 && image.height == 2, "3x2");
	CHECK(image.pixels.size() == 24 && image.pixels[0] == 1 && image.pixels[1] == 2 && image.pixels[2] == 7 &&
					image.pixels[3] == 0xFF,
			"palette colour, fully opaque");
	CHECK(image.pixels.size() == 24 && image.pixels[12] == 4, "the next row overwrites the pad pixel");
	pcx[3] = 4;
	CHECK(!opennova::decode_pcx_menu_rgba(pcx.data(), pcx.size(), image, error), "not 8 bits per pixel");
}


// ---- input geometry --------------------------------------------------------------

// The mnemonic registers for every class whose parse reads a STRING [orig:
// CUIButtonWidget_ParseXMLAttributes @ 0x657c30, the S-chain of grill set A3]:
// STATIC, BUTTON, RADIO (the shipped tab labels), EDIT, CHECKBOX, COMBOBOX...; a
// generic window, a SCROLL and a MARQUEE read no STRING.
void test_mnemonics(const fnt_font_t *font) {
	const auto window = [](const char *type, const char *name) {
		return std::string("<WINDOW type=\"") + type + "\" name=\"" + name + "\">" +
				"<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>50</RIGHT><BOTTOM>20</BOTTOM></POSITION>"
				"<STRING type=\"id\">TAB</STRING></WINDOW>";
	};
	const std::string xml = screen("<FONT><NAME>f.fnt</NAME></FONT><TEXT_RSRC>t.bin</TEXT_RSRC>" +
			window("static", "S") + window("button", "B") + window("radio", "R") + window("edit", "E") +
			window("checkbox", "C") + window("combobox", "O") + window("spinlist", "P") +
			window("list", "L") + window("table", "T") + window("window", "W") +
			window("scroll", "X") + window("marquee_wnd", "M"));
	opennova::mnu::Document doc = parse_or_die(xml);
	MenuTextTables tables;
	tables.set_table("t.bin", table_of({ { "menu", { { "TAB", "{hot}Video" } } } }));
	MenuFrameCompiler c;
	c.register_font("f.fnt", font);
	c.set_text_tables(&tables);
	c.configure(doc.first_screen());
	for (int i = 1; i <= 9; ++i) {
		CHECK(c.widget_mnemonic(i) == "V", "a class whose parse reads the STRING registers its mnemonic");
	}
	CHECK(c.widget_mnemonic(10).empty() && c.widget_mnemonic(11).empty() && c.widget_mnemonic(12).empty(),
			"a generic window, a SCROLL and a MARQUEE register none");
}

// [orig: table_hit_test @ 0x63fe90]
void test_table_hit(const fnt_font_t *font) {
	// No HEADER: one zero-width column. Rows 20 tall under the "W" header (16).
	const std::string bare = screen("<FONT><NAME>f.fnt</NAME></FONT>"
			"<WINDOW type=\"table\" name=\"R\"><POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>300</RIGHT><BOTTOM>100</BOTTOM></POSITION>"
			"<MIN_ITEM_HEIGHT>20</MIN_ITEM_HEIGHT></WINDOW>");
	opennova::mnu::Document doc = parse_or_die(bare);
	MenuFrameCompiler c;
	c.register_font("f.fnt", font);
	c.configure(doc.first_screen());
	MenuFrameState st;
	MenuWidgetState rows;
	rows.index = 1;
	rows.table_rows = { MenuTableRow{ { "a" } }, MenuTableRow{ { "b" } }, MenuTableRow{ { "c" } },
		MenuTableRow{ { "d" } }, MenuTableRow{ { "e" } }, MenuTableRow{ { "f" } } };
	st.widgets.push_back(rows);
	int row = -9;
	int column = -9;
	CHECK(c.table_hit(1, st, 10.0f, 16.0f + 25.0f, 1.0f, 1.0f, &row, &column) && row == 1 && column == -1,
			"a table with no HEADER hits by row with column -1");
	// (100 - 16) / 20 = 4 visible rows: a point on the partial fifth reads the fourth.
	CHECK(c.table_hit(1, st, 10.0f, 99.0f, 1.0f, 1.0f, &row, &column) && row == 3,
			"the row clamps to the last visible row");
	st.widgets[0].scroll_row = 2;
	CHECK(c.table_hit(1, st, 10.0f, 16.0f + 5.0f, 1.0f, 1.0f, &row, &column) && row == 2,
			"the row counts past the scroll offset");
	// The header strip with no column: visible row -1, the row above the view.
	CHECK(c.table_hit(1, st, 10.0f, 5.0f, 1.0f, 1.0f, &row, &column) && row == 1 && column == -1,
			"a header point no column holds is the row above a scrolled view");
	st.widgets[0].scroll_row = 0;
	CHECK(!c.table_hit(1, st, 10.0f, 5.0f, 1.0f, 1.0f, &row, &column), "and nothing unscrolled");
	st.widgets[0].table_rows.resize(2);
	CHECK(!c.table_hit(1, st, 10.0f, 16.0f + 45.0f, 1.0f, 1.0f, &row, &column), "a row past the last fails");
	CHECK(c.table_hit(1, st, 400.0f, 30.0f, 1.0f, 1.0f, &row, &column) && row == -1 && column == -1,
			"outside the table: no row, no column");
	// Headers: the column under the x on the header strip (SPACING between), and
	// in a row; a header past the right edge fails the header test.
	const std::string cols = screen("<FONT><NAME>f.fnt</NAME></FONT>"
			"<WINDOW type=\"table\" name=\"T\"><POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>300</RIGHT><BOTTOM>100</BOTTOM></POSITION>"
			"<MIN_ITEM_HEIGHT>20</MIN_ITEM_HEIGHT><FIXED_HEADER_HEIGHT>10</FIXED_HEADER_HEIGHT>"
			"<COLUMN count=\"3\" spacing=\"10\"><HEADER column=\"0\" width=\"100\">A</HEADER>"
			"<HEADER column=\"1\" width=\"0\">Z</HEADER><HEADER column=\"2\" width=\"100\">B</HEADER></COLUMN></WINDOW>");
	opennova::mnu::Document with = parse_or_die(cols);
	MenuFrameCompiler t;
	t.register_font("f.fnt", font);
	t.configure(with.first_screen());
	MenuFrameState ts;
	MenuWidgetState trows;
	trows.index = 1;
	trows.table_rows = { MenuTableRow{ { "a", "", "b" } } };
	ts.widgets.push_back(trows);
	CHECK(t.table_hit(1, ts, 50.0f, 5.0f, 1.0f, 1.0f, &row, &column) && row == -1 && column == 0,
			"the header strip's column");
	// Column 1 is 0 wide but still advances by SPACING: column 2 spans 120..220.
	CHECK(t.table_hit(1, ts, 125.0f, 15.0f, 1.0f, 1.0f, &row, &column) && row == 0 && column == 2,
			"a row's column: a zero-width column still steps SPACING");
	CHECK(t.table_hit(1, ts, 105.0f, 15.0f, 1.0f, 1.0f, &row, &column) && row == 0 && column == -1,
			"in the gap: the row with column -1");
}

// With a popup open the pump serves its subtree alone [orig: CUIScene_EndFrame
// @ 0x63e600]; the enabled flag the runtime writes replaces the authored DISABLE.
void test_popup_and_enabled(const fnt_font_t *font) {
	const std::string xml = screen("<FONT><NAME>f.fnt</NAME></FONT>" +
			button("OUT", "", "<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>100</RIGHT><BOTTOM>100</BOTTOM></POSITION>") +
			"<WINDOW type=\"window\" name=\"DLG\" MODAL><POSITION><LEFT>200</LEFT><TOP>200</TOP><RIGHT>400</RIGHT><BOTTOM>400</BOTTOM></POSITION>" +
			button("IN", "", "<POSITION><LEFT>10</LEFT><TOP>10</TOP><RIGHT>50</RIGHT><BOTTOM>50</BOTTOM></POSITION>") +
			"</WINDOW>" + "<WINDOW type=\"button\" name=\"OFF\" DISABLE>" +
			"<POSITION><LEFT>500</LEFT><TOP>0</TOP><RIGHT>600</RIGHT><BOTTOM>50</BOTTOM></POSITION></WINDOW>");
	opennova::mnu::Document doc = parse_or_die(xml);
	MenuFrameCompiler c;
	c.register_font("f.fnt", font);
	c.configure(doc.first_screen());
	// 0 ROOT, 1 OUT, 2 DLG, 3 IN, 4 OFF
	MenuFrameState st;
	CHECK(c.pump_mouse(st, 50.0f, 50.0f, false, 1.0f, 1.0f).hovered == 1, "no popup: OUT claims");
	st.popup_root = 2;
	CHECK(c.pump_mouse(st, 50.0f, 50.0f, false, 1.0f, 1.0f).hovered == -1, "a popup open: OUT never claims");
	CHECK(c.pump_mouse(st, 220.0f, 220.0f, false, 1.0f, 1.0f).hovered == 3,
			"the popup's child claims at its absolute rect");
	CHECK(c.pump_mouse(st, 300.0f, 300.0f, false, 1.0f, 1.0f).hovered == 2, "the popup itself claims");
	st.popup_root = -1;
	CHECK(c.widget_disabled(4, st), "the authored DISABLE");
	MenuWidgetState on;
	on.index = 4;
	on.has_disabled = true;
	on.disabled = false;
	st.widgets.push_back(on);
	CHECK(!c.widget_disabled(4, st), "an ENABLE the runtime wrote re-enables it");
}

} // namespace

int main() {
	fnt_font_t font = test_font::uniform_test_font();
	test_text_tables(&font);
	test_cursor();
	test_fonts(&font);
	test_appearance_rows(&font);
	test_label_colors(&font);
	test_image_band(&font);
	test_rect_extents(&font);
	test_spin_arrows(&font);
	test_table_columns(&font);
	test_marquee_credits();
	test_texture_files();
	test_mnemonics(&font);
	test_table_hit(&font);
	test_popup_and_enabled(&font);
	fnt_free(&font);
	if (failures != 0) {
		std::fprintf(stderr, "menu_frame_parity_test: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("menu_frame_parity_test: all checks passed\n");
	return 0;
}
