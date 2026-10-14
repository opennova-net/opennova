// The menu writer's own form (formats/mnu/mnu_write.cpp, D-MNU-22): a menu is written from the model
// alone, in the writer's layout (no original tool writes a menu, so the layout is OpenNova's own; a
// file's comments and spacing are not kept). A document made in code writes the canonical bytes
// pinned below, pretty and on one line; a hand-written menu, its comments and spacing dropped, reads
// back as the model it was read as; a set of edits, and seeded rounds of list edits, each written and
// read again, read back as the edited model. The retail leg (--retail) sweeps every .mnu the packed
// install serves and the reference set's shipped menus: each read, written and read again is the
// model it was, and the same edits over each read back as edited.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <functional>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <base/vfs/vfs.h>
#include <formats/mnu/mnu.h>
#include <formats/mnu/mnu_schema.h>

#include "common/file_io.h"
#include "common/retail_paths.h"

namespace {

namespace mnu = opennova::mnu;

int g_failed = 0;

#define CHECK(cond, msg)                                                         \
	do {                                                                         \
		if (!(cond)) {                                                           \
			std::fprintf(stderr, "FAIL: %s at line %d\n", msg, __LINE__);        \
			++g_failed;                                                          \
			return false;                                                        \
		}                                                                        \
	} while (0)

// What a document reads as, made comparable: the writer's bytes for it.
std::string canonical(const mnu::Document &doc) { return mnu::serialize(doc); }

bool parse(const std::string &text, mnu::Document &doc) {
	std::string error;
	if (!mnu::parse(text, doc, error)) {
		std::fprintf(stderr, "  parse: %s\n", error.c_str());
		return false;
	}
	return true;
}

// The edited model, written, reads back as the edited model: field by field (mnu::equal), no text
// between them, so a field the writer drops is a difference (the #1098 review, F5).
bool reads_back(const mnu::Document &edited, std::string *written = nullptr) {
	const std::string text = mnu::serialize(edited);
	if (written) *written = text;
	mnu::Document again;
	std::string error;
	if (!mnu::parse(text, again, error)) {
		std::fprintf(stderr, "  the written menu does not read: %s\n", error.c_str());
		return false;
	}
	std::string where;
	if (!mnu::equal(again, edited, &where)) {
		std::fprintf(stderr, "  the written menu reads back as another model at %s:\n%s\n", where.c_str(), text.c_str());
		return false;
	}
	return true;
}

// The first difference between two texts, for a failure message.
std::string first_difference(const std::string &a, const std::string &b) {
	size_t i = 0;
	while (i < a.size() && i < b.size() && a[i] == b[i]) ++i;
	const size_t from = i > 60 ? i - 60 : 0;
	return "at byte " + std::to_string(i) + ":\n  written  [" + a.substr(from, 140) + "]\n  expected [" +
	       b.substr(from, 140) + "]";
}

bool same(const std::string &written, const std::string &expected, const char *what) {
	if (written == expected) return true;
	std::fprintf(stderr, "  %s %s\n", what, first_difference(written, expected).c_str());
	return false;
}

// --- a document made in code: the writer's form ------------------------------------------------

// The bytes the writer puts down for the document below (captured from the writer for the #1098
// review: each record on a window whose type reads it, each keyword as its table spells it): pretty
// with two spaces a step, and on one line.
const char *const kCanonicalPretty =
    "<SCREEN>\n"
    "  <NAME>EVERY</NAME>\n"
    "  <MUSICVAR>3</MUSICVAR>\n"
    "  <WINDOW type=\"table\" name=\"ALL\" DRAW_FRAME HIDDEN MODAL DISABLE CHECKED GLOBAL_VAR FORM=\"3\">\n"
    "    <GROUP>2</GROUP>\n"
    "    <HOTKEY VIRTUAL>VK_ESCAPE</HOTKEY>\n"
    "    <HOTKEY>=</HOTKEY>\n"
    "    <ACTION type=\"WINDOW\" state=\"SHOW\" file=\"f.mnu\" FIELD=\"slot\" target_form=\"0\" TOGGLE test=\"EQ\" EXTERNAL_BROWSER> PANEL </ACTION>\n"
    "    <ACTION type=\"SCREEN\" file=\"next.mnu\">NEXT</ACTION>\n"
    "    <FRAME>\n"
    "      <STENCIL size=\"8\" insetx=\"5\">s.tga</STENCIL>\n"
    "      <BRUSH>b.tga</BRUSH>\n"
    "      <MONOGRAM>m.tga</MONOGRAM>\n"
    "    </FRAME>\n"
    "    <POSITION>\n"
    "      <LEFT>1</LEFT>\n"
    "      <TOP>2</TOP>\n"
    "      <RIGHT>300</RIGHT>\n"
    "      <BOTTOM>40</BOTTOM>\n"
    "    </POSITION>\n"
    "    <APPEARANCE type=\"IMAGE\" state=\"DEFAULT\" map_state=\"1\" height=\"24\" flags=\"STANDARD_TRANSPARENT|ALPHATEST\">a.tga</APPEARANCE>\n"
    "    <APPEARANCE type=\"IMAGE\" state=\"MOUSEOVER\" map_state=\"1\" height=\"24\" flags=\"STANDARD_TRANSPARENT|ALPHATEST\">b&amp;c&lt;d>.tga</APPEARANCE>\n"
    "    <TEXT_RSRC>menutxt.BIN</TEXT_RSRC>\n"
    "    <PRIVATE_DATA>p</PRIVATE_DATA>\n"
    "    <CURSOR>\n"
    "      <FILE>c.tga</FILE>\n"
    "      <FLAGS>STANDARD</FLAGS>\n"
    "    </CURSOR>\n"
    "    <FONT>\n"
    "      <NAME>f.fnt</NAME>\n"
    "      <DEFAULT_FG>FF000001</DEFAULT_FG>\n"
    "      <MOUSEOVER_BG>%DEF_BG%</MOUSEOVER_BG>\n"
    "      <DISABLED_FG>3</DISABLED_FG>\n"
    "    </FONT>\n"
    "    <STRING type=\"ID\" justify=\"CENTER\" vjustify=\"BOTTOM\" edge=\"4\" WRAP>KEY</STRING>\n"
    "    <TOGGLE_STRING type=\"ID\">ALT</TOGGLE_STRING>\n"
    "    <SOUND state=\"MOUSEIN\" trigger=\"MOUSE_OVER\">menu.lwf</SOUND>\n"
    "    <ITEMS MULTISELECT>\n"
    "      <APPEARANCE type=\"IMAGE\" state=\"DEFAULT\" map_state=\"1\" height=\"24\" flags=\"STANDARD_TRANSPARENT|ALPHATEST\">row.tga</APPEARANCE>\n"
    "      <ROW>\n"
    "        <ITEM type=\"BITMAP\" column=\"1\">c.tga</ITEM>\n"
    "        <ITEM type=\"BITMAP\" column=\"1\">c.tga</ITEM>\n"
    "      </ROW>\n"
    "      <ROW>\n"
    "      </ROW>\n"
    "    </ITEMS>\n"
    "    <COLUMN count=\"3\" spacing=\"2\">\n"
    "      <HEADER justify=\"CENTER\" SECONDARY_SORT TERTIARY_SORT column=\"0\" sort=\"A\" width=\"100\" type=\"ID\">H0</HEADER>\n"
    "      <HEADER justify=\"CENTER\" PRIMARY_SORT column=\"2\" sort=\"A\" width=\"100\" type=\"ID\">H2</HEADER>\n"
    "      <BODY column=\"1\" BITMAP_TEXT CUSTOM_DRAW BITMAP_DRAW BITMAP_FLAGS=\"STANDARD\" SCALE_BITMAP></BODY>\n"
    "      <SUBST column=\"0\" value=\"1\" URL FILE>x.tga</SUBST>\n"
    "    </COLUMN>\n"
    "    <MIN_ITEM_HEIGHT>20</MIN_ITEM_HEIGHT>\n"
    "    <FIXED_HEADER_HEIGHT>0</FIXED_HEADER_HEIGHT>\n"
    "    <SCROLLBAR>\n"
    "      <SCROLLDOWN type=\"IMAGE\" state=\"DEFAULT\" map_state=\"1\" height=\"24\" flags=\"STANDARD_TRANSPARENT|ALPHATEST\">dn.tga</SCROLLDOWN>\n"
    "    </SCROLLBAR>\n"
    "    <WINDOW type=\"edit\" name=\"ED\" READONLY NUMBER MINVAL=\"-1\" MAXVAL=\"99\" MAXCHAR=\"2\" PASSWORD>\n"
    "      <POSITION>\n"
    "        <LEFT>10</LEFT>\n"
    "      </POSITION>\n"
    "    </WINDOW>\n"
    "    <WINDOW type=\"checkbox\" name=\"CB\" CHECKED AS_BUTTON>\n"
    "      <POSITION>\n"
    "        <LEFT>11</LEFT>\n"
    "      </POSITION>\n"
    "    </WINDOW>\n"
    "    <WINDOW type=\"scroll\" name=\"SC\">\n"
    "      <POSITION>\n"
    "        <LEFT>12</LEFT>\n"
    "      </POSITION>\n"
    "      <WIDTH>20</WIDTH>\n"
    "      <ORIENTATION>HORIZONTAL</ORIENTATION>\n"
    "      <SHUTTLE type=\"IMAGE\" state=\"DEFAULT\" map_state=\"1\" height=\"24\" flags=\"STANDARD_TRANSPARENT|ALPHATEST\">s1.tga</SHUTTLE>\n"
    "      <SCROLLUP type=\"IMAGE\" state=\"DEFAULT\" map_state=\"1\" height=\"24\" flags=\"STANDARD_TRANSPARENT|ALPHATEST\">u1.tga</SCROLLUP>\n"
    "      <SCROLLDOWN type=\"IMAGE\" state=\"DEFAULT\" map_state=\"1\" height=\"24\" flags=\"STANDARD_TRANSPARENT|ALPHATEST\">d1.tga</SCROLLDOWN>\n"
    "    </WINDOW>\n"
    "    <WINDOW type=\"marquee_wnd\" name=\"MQ\">\n"
    "      <POSITION>\n"
    "        <LEFT>13</LEFT>\n"
    "      </POSITION>\n"
    "      <DATASOURCE>nlist.kda</DATASOURCE>\n"
    "      <DATASOURCE>credits.ini</DATASOURCE>\n"
    "    </WINDOW>\n"
    "    <WINDOW type=\"list\" name=\"LS\">\n"
    "      <POSITION>\n"
    "        <LEFT>14</LEFT>\n"
    "      </POSITION>\n"
    "      <ITEMS justify=\"LEFT\" vjustify=\"TOP\" MULTISELECT>\n"
    "        <APPEARANCE type=\"IMAGE\" state=\"DEFAULT\" map_state=\"1\" height=\"24\" flags=\"STANDARD_TRANSPARENT|ALPHATEST\">row.tga</APPEARANCE>\n"
    "        <ITEM type=\"ID\" value=\"1\" justify=\"RIGHT\" vjustify=\"CENTER\" PAIRS_LIST>T</ITEM>\n"
    "      </ITEMS>\n"
    "    </WINDOW>\n"
    "    <WINDOW type=\"combobox\" name=\"CO\">\n"
    "      <POSITION>\n"
    "        <LEFT>15</LEFT>\n"
    "      </POSITION>\n"
    "      <LIST_BOX sb_edge_pad=\"6\" name=\"DROP\">\n"
    "        <POSITION>\n"
    "          <TOP>9</TOP>\n"
    "        </POSITION>\n"
    "        <SCROLLBAR>\n"
    "          <SHUTTLE type=\"IMAGE\" state=\"DEFAULT\" map_state=\"1\" height=\"24\" flags=\"STANDARD_TRANSPARENT|ALPHATEST\">sh.tga</SHUTTLE>\n"
    "        </SCROLLBAR>\n"
    "      </LIST_BOX>\n"
    "    </WINDOW>\n"
    "    <WINDOW type=\"spinlist\" name=\"SP\">\n"
    "      <POSITION>\n"
    "        <LEFT>16</LEFT>\n"
    "      </POSITION>\n"
    "      <SPINUP>\n"
    "        <APPEARANCE type=\"IMAGE\" state=\"DEFAULT\" map_state=\"1\" height=\"24\" flags=\"STANDARD_TRANSPARENT|ALPHATEST\">up.tga</APPEARANCE>\n"
    "      </SPINUP>\n"
    "      <SPINDOWN>\n"
    "        <STRING>-</STRING>\n"
    "      </SPINDOWN>\n"
    "    </WINDOW>\n"
    "    <WINDOW type=\"glb_table\" name=\"GB\" PLAYERLIST>\n"
    "      <POSITION>\n"
    "        <LEFT>17</LEFT>\n"
    "      </POSITION>\n"
    "      <GLB_TABLE>\n"
    "        <POSITION>\n"
    "          <LEFT>2</LEFT>\n"
    "        </POSITION>\n"
    "        <COLUMN>\n"
    "          <HEADER column=\"0\" FIELD=\"ping\">Ping</HEADER>\n"
    "        </COLUMN>\n"
    "      </GLB_TABLE>\n"
    "      <GLB_INFO_PLAYER_DETAILS>\n"
    "        <POSITION>\n"
    "          <TOP>3</TOP>\n"
    "        </POSITION>\n"
    "      </GLB_INFO_PLAYER_DETAILS>\n"
    "      <GLB_INFO_BUTTON>\n"
    "        <STRING>Info</STRING>\n"
    "      </GLB_INFO_BUTTON>\n"
    "      <GLB_INFO_SERVER_DETAILS>\n"
    "        <POSITION>\n"
    "          <TOP>4</TOP>\n"
    "        </POSITION>\n"
    "        <FIELDS count=\"2\">\n"
    "          <FORMAT>%s\\n%s</FORMAT>\n"
    "          <FIELD which=\"0\">map</FIELD>\n"
    "          <FIELD which=\"1\">mode</FIELD>\n"
    "        </FIELDS>\n"
    "      </GLB_INFO_SERVER_DETAILS>\n"
    "      <GLB_NW_INFO>\n"
    "        <POSITION>\n"
    "          <TOP>5</TOP>\n"
    "        </POSITION>\n"
    "      </GLB_NW_INFO>\n"
    "      <TARGET>http://x</TARGET>\n"
    "    </WINDOW>\n"
    "    <WINDOW type=\"gopher\" name=\"GO\">\n"
    "      <POSITION>\n"
    "        <LEFT>18</LEFT>\n"
    "      </POSITION>\n"
    "      <TARGET>http://x</TARGET>\n"
    "      <TARGET>http://y</TARGET>\n"
    "      <FILTERS>\n"
    "        <CATEGORY LEVEL=\"LEVEL1\">\n"
    "          <LABEL>\n"
    "            <STRING>Maps</STRING>\n"
    "          </LABEL>\n"
    "          <ITEM NAME=\"B\" TARGET=\"t\" FILTER=\"f\" TABLE=\"tb\">Alpha</ITEM>\n"
    "          <INFO NAME=\"I\" FILTER=\"g\">About</INFO>\n"
    "        </CATEGORY>\n"
    "      </FILTERS>\n"
    "      <GOPHER_BACK>\n"
    "        <POSITION>\n"
    "          <LEFT>1</LEFT>\n"
    "        </POSITION>\n"
    "      </GOPHER_BACK>\n"
    "    </WINDOW>\n"
    "    <WINDOW type=\"lan_list\" name=\"LL\">\n"
    "      <POSITION>\n"
    "        <LEFT>19</LEFT>\n"
    "      </POSITION>\n"
    "      <JOIN_BUTTON>\n"
    "        <STRING>Join</STRING>\n"
    "      </JOIN_BUTTON>\n"
    "      <SEARCH_BUTTON>\n"
    "        <STRING>Find</STRING>\n"
    "      </SEARCH_BUTTON>\n"
    "    </WINDOW>\n"
    "    <WINDOW type=\"static\" name=\"CHILD\">\n"
    "      <POSITION>\n"
    "        <LEFT>7</LEFT>\n"
    "      </POSITION>\n"
    "    </WINDOW>\n"
    "    <WINDOW type=\"window\" name=\"BARE\">\n"
    "    </WINDOW>\n"
    "  </WINDOW>\n"
    "  <WINDOW type=\"static\" name=\"CHILD\">\n"
    "    <POSITION>\n"
    "      <LEFT>7</LEFT>\n"
    "    </POSITION>\n"
    "  </WINDOW>\n"
    "</SCREEN>\n"
    "<SCREEN>\n"
    "</SCREEN>\n";

const char *const kCanonicalCompact =
    "<SCREEN><NAME>EVERY</NAME><MUSICVAR>3</MUSICVAR>"
    "<WINDOW type=\"table\" name=\"ALL\" DRAW_FRAME HIDDEN MODAL DISABLE CHECKED GLOBAL_VAR FORM=\"3\"><GROUP>2"
    "</GROUP><HOTKEY VIRTUAL>VK_ESCAPE</HOTKEY><HOTKEY>=</HOTKEY>"
    "<ACTION type=\"WINDOW\" state=\"SHOW\" file=\"f.mnu\" FIELD=\"slot\" target_form=\"0\" TOGGLE test=\"EQ\" EXTERNAL_BROWSER> PANEL "
    "</ACTION><ACTION type=\"SCREEN\" file=\"next.mnu\">NEXT</ACTION><FRAME>"
    "<STENCIL size=\"8\" insetx=\"5\">s.tga</STENCIL><BRUSH>b.tga</BRUSH><MONOGRAM>m.tga</MONOGRAM></FRAME>"
    "<POSITION><LEFT>1</LEFT><TOP>2</TOP><RIGHT>300</RIGHT><BOTTOM>40</BOTTOM></POSITION>"
    "<APPEARANCE type=\"IMAGE\" state=\"DEFAULT\" map_state=\"1\" height=\"24\" flags=\"STANDARD_TRANSPARENT|ALPHATEST\">a.tga"
    "</APPEARANCE>"
    "<APPEARANCE type=\"IMAGE\" state=\"MOUSEOVER\" map_state=\"1\" height=\"24\" flags=\"STANDARD_TRANSPARENT|ALPHATEST\">b&amp;c&lt;d>.tga"
    "</APPEARANCE><TEXT_RSRC>menutxt.BIN</TEXT_RSRC><PRIVATE_DATA>p</PRIVATE_DATA><CURSOR><FILE>c.tga"
    "</FILE><FLAGS>STANDARD</FLAGS></CURSOR><FONT><NAME>f.fnt</NAME><DEFAULT_FG>FF000001</DEFAULT_FG>"
    "<MOUSEOVER_BG>%DEF_BG%</MOUSEOVER_BG><DISABLED_FG>3</DISABLED_FG></FONT>"
    "<STRING type=\"ID\" justify=\"CENTER\" vjustify=\"BOTTOM\" edge=\"4\" WRAP>KEY</STRING>"
    "<TOGGLE_STRING type=\"ID\">ALT</TOGGLE_STRING><SOUND state=\"MOUSEIN\" trigger=\"MOUSE_OVER\">menu.lwf"
    "</SOUND><ITEMS MULTISELECT>"
    "<APPEARANCE type=\"IMAGE\" state=\"DEFAULT\" map_state=\"1\" height=\"24\" flags=\"STANDARD_TRANSPARENT|ALPHATEST\">row.tga"
    "</APPEARANCE><ROW><ITEM type=\"BITMAP\" column=\"1\">c.tga</ITEM><ITEM type=\"BITMAP\" column=\"1\">c.tga"
    "</ITEM></ROW><ROW></ROW></ITEMS><COLUMN count=\"3\" spacing=\"2\">"
    "<HEADER justify=\"CENTER\" SECONDARY_SORT TERTIARY_SORT column=\"0\" sort=\"A\" width=\"100\" type=\"ID\">H0"
    "</HEADER><HEADER justify=\"CENTER\" PRIMARY_SORT column=\"2\" sort=\"A\" width=\"100\" type=\"ID\">H2</HEADER>"
    "<BODY column=\"1\" BITMAP_TEXT CUSTOM_DRAW BITMAP_DRAW BITMAP_FLAGS=\"STANDARD\" SCALE_BITMAP></BODY>"
    "<SUBST column=\"0\" value=\"1\" URL FILE>x.tga</SUBST></COLUMN><MIN_ITEM_HEIGHT>20</MIN_ITEM_HEIGHT>"
    "<FIXED_HEADER_HEIGHT>0</FIXED_HEADER_HEIGHT><SCROLLBAR>"
    "<SCROLLDOWN type=\"IMAGE\" state=\"DEFAULT\" map_state=\"1\" height=\"24\" flags=\"STANDARD_TRANSPARENT|ALPHATEST\">dn.tga"
    "</SCROLLDOWN></SCROLLBAR>"
    "<WINDOW type=\"edit\" name=\"ED\" READONLY NUMBER MINVAL=\"-1\" MAXVAL=\"99\" MAXCHAR=\"2\" PASSWORD>"
    "<POSITION><LEFT>10</LEFT></POSITION></WINDOW><WINDOW type=\"checkbox\" name=\"CB\" CHECKED AS_BUTTON>"
    "<POSITION><LEFT>11</LEFT></POSITION></WINDOW><WINDOW type=\"scroll\" name=\"SC\"><POSITION><LEFT>12"
    "</LEFT></POSITION><WIDTH>20</WIDTH><ORIENTATION>HORIZONTAL</ORIENTATION>"
    "<SHUTTLE type=\"IMAGE\" state=\"DEFAULT\" map_state=\"1\" height=\"24\" flags=\"STANDARD_TRANSPARENT|ALPHATEST\">s1.tga"
    "</SHUTTLE>"
    "<SCROLLUP type=\"IMAGE\" state=\"DEFAULT\" map_state=\"1\" height=\"24\" flags=\"STANDARD_TRANSPARENT|ALPHATEST\">u1.tga"
    "</SCROLLUP>"
    "<SCROLLDOWN type=\"IMAGE\" state=\"DEFAULT\" map_state=\"1\" height=\"24\" flags=\"STANDARD_TRANSPARENT|ALPHATEST\">d1.tga"
    "</SCROLLDOWN></WINDOW><WINDOW type=\"marquee_wnd\" name=\"MQ\"><POSITION><LEFT>13</LEFT></POSITION>"
    "<DATASOURCE>nlist.kda</DATASOURCE><DATASOURCE>credits.ini</DATASOURCE></WINDOW>"
    "<WINDOW type=\"list\" name=\"LS\"><POSITION><LEFT>14</LEFT></POSITION>"
    "<ITEMS justify=\"LEFT\" vjustify=\"TOP\" MULTISELECT>"
    "<APPEARANCE type=\"IMAGE\" state=\"DEFAULT\" map_state=\"1\" height=\"24\" flags=\"STANDARD_TRANSPARENT|ALPHATEST\">row.tga"
    "</APPEARANCE><ITEM type=\"ID\" value=\"1\" justify=\"RIGHT\" vjustify=\"CENTER\" PAIRS_LIST>T</ITEM></ITEMS>"
    "</WINDOW><WINDOW type=\"combobox\" name=\"CO\"><POSITION><LEFT>15</LEFT></POSITION>"
    "<LIST_BOX sb_edge_pad=\"6\" name=\"DROP\"><POSITION><TOP>9</TOP></POSITION><SCROLLBAR>"
    "<SHUTTLE type=\"IMAGE\" state=\"DEFAULT\" map_state=\"1\" height=\"24\" flags=\"STANDARD_TRANSPARENT|ALPHATEST\">sh.tga"
    "</SHUTTLE></SCROLLBAR></LIST_BOX></WINDOW><WINDOW type=\"spinlist\" name=\"SP\"><POSITION><LEFT>16"
    "</LEFT></POSITION><SPINUP>"
    "<APPEARANCE type=\"IMAGE\" state=\"DEFAULT\" map_state=\"1\" height=\"24\" flags=\"STANDARD_TRANSPARENT|ALPHATEST\">up.tga"
    "</APPEARANCE></SPINUP><SPINDOWN><STRING>-</STRING></SPINDOWN></WINDOW>"
    "<WINDOW type=\"glb_table\" name=\"GB\" PLAYERLIST><POSITION><LEFT>17</LEFT></POSITION><GLB_TABLE>"
    "<POSITION><LEFT>2</LEFT></POSITION><COLUMN><HEADER column=\"0\" FIELD=\"ping\">Ping</HEADER></COLUMN>"
    "</GLB_TABLE><GLB_INFO_PLAYER_DETAILS><POSITION><TOP>3</TOP></POSITION></GLB_INFO_PLAYER_DETAILS>"
    "<GLB_INFO_BUTTON><STRING>Info</STRING></GLB_INFO_BUTTON><GLB_INFO_SERVER_DETAILS><POSITION><TOP>4"
    "</TOP></POSITION><FIELDS count=\"2\"><FORMAT>%s\\n%s</FORMAT><FIELD which=\"0\">map</FIELD>"
    "<FIELD which=\"1\">mode</FIELD></FIELDS></GLB_INFO_SERVER_DETAILS><GLB_NW_INFO><POSITION><TOP>5</TOP>"
    "</POSITION></GLB_NW_INFO><TARGET>http://x</TARGET></WINDOW><WINDOW type=\"gopher\" name=\"GO\">"
    "<POSITION><LEFT>18</LEFT></POSITION><TARGET>http://x</TARGET><TARGET>http://y</TARGET><FILTERS>"
    "<CATEGORY LEVEL=\"LEVEL1\"><LABEL><STRING>Maps</STRING></LABEL>"
    "<ITEM NAME=\"B\" TARGET=\"t\" FILTER=\"f\" TABLE=\"tb\">Alpha</ITEM><INFO NAME=\"I\" FILTER=\"g\">About</INFO>"
    "</CATEGORY></FILTERS><GOPHER_BACK><POSITION><LEFT>1</LEFT></POSITION></GOPHER_BACK></WINDOW>"
    "<WINDOW type=\"lan_list\" name=\"LL\"><POSITION><LEFT>19</LEFT></POSITION><JOIN_BUTTON><STRING>Join"
    "</STRING></JOIN_BUTTON><SEARCH_BUTTON><STRING>Find</STRING></SEARCH_BUTTON></WINDOW>"
    "<WINDOW type=\"static\" name=\"CHILD\"><POSITION><LEFT>7</LEFT></POSITION></WINDOW>"
    "<WINDOW type=\"window\" name=\"BARE\"></WINDOW></WINDOW><WINDOW type=\"static\" name=\"CHILD\"><POSITION>"
    "<LEFT>7</LEFT></POSITION></WINDOW></SCREEN><SCREEN></SCREEN>";

mnu::Appearance image_row(const char *state, const char *value) {
	mnu::Appearance a;
	a.state = state;
	a.type = "IMAGE";
	a.value = value;
	a.has_map_state = true;
	a.map_state = 1;
	a.has_height = true;
	a.height = 24;
	a.flags = 0x340651u; // STANDARD_TRANSPARENT|ALPHATEST
	return a;
}

// A window of `type` named `name` at a left edge (a window with no element is not created).
mnu::Window typed_window(mnu::WindowType type, const char *name, int left) {
	mnu::Window w;
	w.name = name;
	w.type = type;
	w.position.has_left = true;
	w.position.left = left;
	return w;
}

// A document built in code with no source text: every record the writer puts down, each on a
// window whose type reads it (the writer puts down what the window's type reads alone, as the
// reader keeps it), so its canonical bytes pin the writer's own layout. Every keyword is the
// parse's table spelling, as the reader keeps it.
mnu::Document minted() {
	mnu::Window w;
	w.name = "ALL";
	w.type = mnu::WindowType::Table;
	w.hidden = w.disabled = w.checked = w.draw_frame = w.modal = true;
	w.global_var = true;
	w.has_group = true;
	w.group = 2;
	w.has_form = true;
	w.form = 3;
	w.position.has_left = w.position.has_top = w.position.has_right = w.position.has_bottom = true;
	w.position.left = 1;
	w.position.top = 2;
	w.position.right = 300;
	w.position.bottom = 40;
	w.appearances = {image_row("DEFAULT", "a.tga"), image_row("MOUSEOVER", "b&c<d>.tga")};
	mnu::Sound sound;
	sound.state = "MOUSEIN";
	sound.trigger = "MOUSE_OVER";
	sound.file = "menu.lwf";
	w.sounds = {sound};
	mnu::Action action;
	action.type = "WINDOW";
	action.state = "SHOW";
	action.file = "f.mnu";
	action.field = "slot";
	action.has_target_form = true;
	action.target_form = 0;
	action.toggle = action.external_browser = true;
	action.test = "EQ";
	action.target = " PANEL ";
	mnu::Action plain;
	plain.type = "SCREEN";
	plain.file = "next.mnu";
	plain.target = "NEXT";
	w.actions = {action, plain};
	w.string_data.present = true;
	w.string_data.type = "ID";
	w.string_data.justify = "CENTER";
	w.string_data.vjustify = "BOTTOM";
	w.string_data.has_edge = true;
	w.string_data.edge = 4;
	w.string_data.wrap = true;
	w.string_data.value = "KEY";
	w.toggle_string.present = true;
	w.toggle_string.type = "ID";
	w.toggle_string.value = "ALT";
	w.font.name = "f.fnt";
	w.font.default_fg = "FF000001";
	w.font.mouseover_bg = "%DEF_BG%";
	w.font.disabled_fg = "3";
	w.frame.stencil = "s.tga";
	w.frame.has_stencil_size = w.frame.has_insetx = true;
	w.frame.stencil_size = 8;
	w.frame.insetx = 5;
	w.frame.brush = "b.tga";
	w.frame.monogram = "m.tga";
	// A TABLE's ITEMS: MULTISELECT, its APPEARANCE rows and its ROWs of cells.
	w.items.present = w.items.multiselect = true;
	w.items.appearances = {image_row("DEFAULT", "row.tga")};
	mnu::Item cell;
	cell.type = "BITMAP";
	cell.text = "c.tga";
	cell.has_column = true;
	cell.column = 1;
	mnu::TableRow row;
	row.cells = {cell, cell};
	w.items.rows = {row, mnu::TableRow{}};
	w.scrollbar.author(mnu::WindowType::Scroll).scrolldown = {image_row("DEFAULT", "dn.tga")};
	w.cursor.file = "c.tga";
	w.cursor.flags = 0x300600u; // STANDARD
	w.has_text_rsrc = true;
	w.text_rsrc = "menutxt.BIN";
	w.private_data = "p";
	mnu::Hotkey escape;
	escape.value = "VK_ESCAPE";
	escape.virtual_key = true;
	mnu::Hotkey equals;
	equals.value = "=";
	w.hotkeys = {escape, equals};
	mnu::TableColumn &column = w.table_data.column;
	column.has_count = column.has_spacing = true;
	column.count = 3;
	column.spacing = 2;
	mnu::TableHeader header;
	header.justify = "CENTER";
	header.has_column = header.has_width = true;
	header.column = 0;
	header.width = 100;
	header.sort = "A";
	header.type = "ID";
	header.text = "H0";
	mnu::TableHeader second = header;
	second.column = 2;
	second.text = "H2";
	column.headers = {header, second};
	mnu::TableBody body;
	body.has_column = true;
	body.column = 1;
	body.bitmap_draw = body.custom_draw = body.bitmap_text = body.scale_bitmap = true;
	body.display = "BITMAP_TEXT";
	body.bitmap_flags = 0x300600u;
	column.bodies = {body};
	mnu::TableSubst subst;
	subst.has_column = true;
	subst.value = "1";
	subst.is_file = subst.is_url = true;
	subst.file = "x.tga";
	column.substitutions = {subst};
	column.primary_sort = 2;
	column.secondary_sort = 0;
	column.tertiary_sort = 0;
	w.table_data.has_min_item_height = true;
	w.table_data.min_item_height = 20;
	w.table_data.has_fixed_header_height = true;
	w.table_data.fixed_header_height = 0;

	// An EDIT's attributes [orig: CEditWnd_ParseXMLProperties @ 0x661d10].
	mnu::Window edit = typed_window(mnu::WindowType::Edit, "ED", 10);
	edit.readonly = edit.number = edit.password = true;
	edit.has_minval = edit.has_maxval = edit.has_maxchar = true;
	edit.minval = -1;
	edit.maxval = 99;
	edit.maxchar = 2;
	// A CHECKBOX's AS_BUTTON and CHECKED [orig: CCheckWnd_ParseXMLDefinition @ 0x64ad90].
	mnu::Window check = typed_window(mnu::WindowType::CheckBox, "CB", 11);
	check.as_button = check.checked = true;
	// A SCROLL's own elements [orig: CUIScrollWidget_ParseExtendedXMLDef @ 0x64c6d0].
	mnu::Window scroll = typed_window(mnu::WindowType::Scroll, "SC", 12);
	scroll.orientation = "HORIZONTAL";
	scroll.has_scroll_extent = true;
	scroll.scroll_extent = 20;
	scroll.scroll_extent_is_width = true;
	scroll.shuttle = {image_row("DEFAULT", "s1.tga")};
	scroll.scrollup = {image_row("DEFAULT", "u1.tga")};
	scroll.scrolldown = {image_row("DEFAULT", "d1.tga")};
	// A MARQUEE_WND's DATASOURCEs [orig: CMarqueeWnd_ParseXMLDefinition @ 0x65ceb0].
	mnu::Window marquee = typed_window(mnu::WindowType::Marquee, "MQ", 13);
	marquee.datasources = {"nlist.kda", "credits.ini"};
	// A LIST's ITEMS: JUSTIFY, VJUSTIFY, MULTISELECT, its APPEARANCE and ITEM rows (PAIRS_LIST).
	mnu::Window list = typed_window(mnu::WindowType::List, "LS", 14);
	list.items.present = list.items.multiselect = true;
	list.items.justify = "LEFT";
	list.items.vjustify = "TOP";
	list.items.appearances = {image_row("DEFAULT", "row.tga")};
	mnu::Item item;
	item.type = "ID";
	item.value = "1";
	item.text = "T";
	item.justify = "RIGHT";
	item.vjustify = "CENTER";
	item.pairs_list = true;
	list.items.items = {item};
	// A COMBOBOX's LIST_BOX and its sb_edge_pad [orig: CComboWnd_ParseXMLDefinition @ 0x65c0d0].
	mnu::Window combo = typed_window(mnu::WindowType::Combo, "CO", 15);
	mnu::Window &drop = combo.list_box.author(mnu::WindowType::List);
	drop.name = "DROP";
	drop.position.has_top = true;
	drop.position.top = 9;
	drop.scrollbar.author(mnu::WindowType::Scroll).shuttle = {image_row("DEFAULT", "sh.tga")};
	combo.has_sb_edge_pad = true;
	combo.sb_edge_pad = 6;
	// A SPINLIST's arrows.
	mnu::Window spin = typed_window(mnu::WindowType::SpinList, "SP", 16);
	spin.spinup.author(mnu::WindowType::Button).appearances = {image_row("DEFAULT", "up.tga")};
	spin.spindown.author(mnu::WindowType::Button).string_data.present = true;
	spin.spindown->string_data.value = "-";
	// The GLB_TABLE browser's parts [orig: CLanGameBrowser_ParseExtendedXMLDefinition @ 0x65dac0]: PLAYERLIST,
	// its table with a HEADER's FIELD, the info button, the player details table, the server details edit and
	// its FIELDS, the NovaWorld info static and the TARGET.
	mnu::Window glb = typed_window(mnu::WindowType::GlbTable, "GB", 17);
	glb.glb_playerlist = true;
	mnu::Window &browser = glb.glb_table.author(mnu::WindowType::Table);
	browser.position.has_left = true;
	browser.position.left = 2;
	mnu::TableHeader ping;
	ping.has_column = true;
	ping.column = 0;
	ping.field = "ping";
	ping.text = "Ping";
	browser.table_data.column.headers = {ping};
	glb.glb_info_button.author(mnu::WindowType::Button).string_data.present = true;
	glb.glb_info_button->string_data.value = "Info";
	mnu::Window &players = glb.glb_info_player_details.author(mnu::WindowType::Table);
	players.position.has_top = true;
	players.position.top = 3;
	mnu::Window &details = glb.glb_info_server_details.author(mnu::WindowType::MultilineEdit);
	details.position.has_top = true;
	details.position.top = 4;
	glb.glb_server_fields.present = glb.glb_server_fields.has_count = glb.glb_server_fields.has_format = true;
	glb.glb_server_fields.count = 2;
	glb.glb_server_fields.format = "%s\n%s";
	glb.glb_server_fields.fields = {{0, "map"}, {1, "mode"}};
	glb.glb_nw_info.author(mnu::WindowType::Static).position.has_top = true;
	glb.glb_nw_info->position.top = 5;
	glb.has_glb_target = true;
	glb.glb_target = "http://x";
	// The GOPHER's targets, its FILTERS' CATEGORY (LEVEL, LABEL, an ITEM and an INFO) and GOPHER_BACK [orig:
	// CGopherWnd_ParseXMLDefinition @ 0x65b130; CGopherWnd_ParseCategoryXML @ 0x65a540].
	mnu::Window gopher = typed_window(mnu::WindowType::Gopher, "GO", 18);
	gopher.gopher_targets = {"http://x", "http://y"};
	mnu::GopherCategory category;
	category.has_level = true;
	category.level = "LEVEL1";
	category.label.author(mnu::WindowType::Static).string_data.present = true;
	category.label->string_data.value = "Maps";
	mnu::GopherEntry button;
	button.name = "B";
	button.target = "t";
	button.filter = "f";
	button.table = "tb";
	button.text = "Alpha";
	mnu::GopherEntry info;
	info.info = true;
	info.name = "I";
	info.filter = "g";
	info.text = "About";
	category.entries = {button, info};
	gopher.gopher_categories = {category};
	gopher.gopher_back.author(mnu::WindowType::Button).position.has_left = true;
	gopher.gopher_back->position.left = 1;
	// The LAN_LIST's two buttons [orig: CLanListWnd_ParseXMLDefinition @ 0x65b9d0].
	mnu::Window lan = typed_window(mnu::WindowType::LanList, "LL", 19);
	lan.join_button.author(mnu::WindowType::Button).string_data.present = true;
	lan.join_button->string_data.value = "Join";
	lan.search_button.author(mnu::WindowType::Button).string_data.present = true;
	lan.search_button->string_data.value = "Find";
	mnu::Window child = typed_window(mnu::WindowType::Static, "CHILD", 7);
	mnu::Window bare;
	bare.name = "BARE";
	w.children = {edit, check, scroll, marquee, list, combo, spin, glb, gopher, lan, child, bare};

	mnu::Document doc;
	mnu::Screen screen;
	screen.name = "EVERY";
	screen.has_music_var = true;
	screen.music_var = 3;
	screen.roots = {w, child};
	mnu::Screen nameless;
	doc.screens = {screen, nameless};
	return doc;
}

mnu::Window &main_window(mnu::Document &doc) { return doc.screens.at(0).roots.at(0); }

bool test_canonical_bytes() {
	const mnu::Document doc = minted();
	CHECK(same(mnu::serialize(doc, true, 2), kCanonicalPretty, "pretty, 2:"), "the canonical bytes, pretty");
	CHECK(same(mnu::serialize(doc, false, 0), kCanonicalCompact, "compact:"), "the canonical bytes, compact");
	// Four spaces a step: each line's indentation doubled.
	std::string four;
	const std::string pretty = kCanonicalPretty;
	for (size_t at = 0; at < pretty.size();) {
		const size_t end = pretty.find('\n', at);
		const std::string line = pretty.substr(at, end - at);
		const size_t blanks = line.find_first_not_of(' ');
		four += std::string(blanks == std::string::npos ? 0 : blanks * 2, ' ') +
		        line.substr(blanks == std::string::npos ? line.size() : blanks) + "\n";
		at = end + 1;
	}
	CHECK(same(mnu::serialize(doc, true, 4), four, "pretty, 4:"), "the canonical bytes, four a step");
	// The writer's bytes read back as a model that, written and read again, is itself (the BARE window
	// holds no element, so the reader creates none, as retail's factory does not).
	mnu::Document back;
	CHECK(parse(kCanonicalPretty, back), "parse");
	CHECK(back.screens.size() == 2 && main_window(back).children.size() == 11, "the canonical file as the reader reads it");
	mnu::Document expected = minted();
	expected.screens[0].roots[0].children.pop_back(); // BARE: no element, not created
	std::string where;
	if (!mnu::equal(back, expected, &where)) std::fprintf(stderr, "  differs at %s\n", where.c_str());
	CHECK(where.empty(), "the canonical file reads back as the minted model");
	CHECK(reads_back(back), "the canonical file, read, reads back");
	return true;
}

// A menu written by hand: a comment before it, CR LF lines and tabs, a blank line, a comment on an
// element's line, an attribute's repeat, unquoted and oddly spaced attributes, a tag in another
// case, POSITION edges as ULX and WIDTH, a scroll row closed by another element's name, an element
// no parse reads, an entity, a RAW_TEXT block.
const std::string kHand =
    "<!-- a menu written by hand -->\r\n"
    "<SCREEN junk=\"1\">\r\n"
    "\t<NAME>LAYOUT</NAME>\r\n"
    "\t<MusicVar> 2</MusicVar>\r\n"
    "\t<WINDOW  type=window   name=MAIN DISABLED >\r\n"
    "\t\t<!-- the backdrop -->\r\n"
    "\t\t<APPEARANCE TYPE=\"IMAGE\" state=\"default\" map_state=\"0 height=\"14\">back&amp;drop.tga</APPEARANCE>"
    "  <!-- same line -->\r\n"
    "\t\t<POSITION>\r\n"
    "\t\t\t<ULX>10</ULX>\r\n"
    "\t\t\t<WIDTH>0100</WIDTH>\r\n"
    "\t\t\t<TOP>5</TOP>\r\n"
    "\t\t</POSITION>\r\n"
    "\r\n"
    "\t\t<SCROLLUP type=\"image\" state=\"default\">up.tga</APPEARANCE>\r\n"
    "\t\t<UNKNOWN a=\"b\">ignored</UNKNOWN>\r\n"
    "\t\t<WINDOW type=\"button\" name=\"OK\" name=\"DUP\">\r\n"
    "\t\t\t<STRING type=\"id\" justify=\"CENTER\">OK_KEY</STRING>\r\n"
    "\t\t\t<SOUND state=\"selected\" trigger=\"CLICK_SELECT\" LOOP>menu.lwf</SOUND>\r\n"
    "\t\t</WINDOW>\r\n"
    "\t\t<!-- the cancel button -->\r\n"
    "\t\t<WINDOW type=\"button\" name=\"CANCEL\">\r\n"
    "\t\t\t<STRING><RAW_TEXT>a<b</RAW_TEXT></STRING>\r\n"
    "\t\t</WINDOW>\r\n"
    "\t</WINDOW>\r\n"
    "</SCREEN>\r\n";

// A hand-written menu comes out in the writer's form, its comments and spacing dropped, and reads back
// as the model it was read as.
bool test_hand_written() {
	mnu::Document doc;
	CHECK(parse(kHand, doc), "parse");
	CHECK(main_window(doc).name == "MAIN" && main_window(doc).children.size() == 2, "the menu as the reader reads it");
	std::string written;
	CHECK(reads_back(doc, &written), "the written menu reads back as the model");
	CHECK(written.find("<!--") == std::string::npos && written.find('\r') == std::string::npos &&
	          written.find('\t') == std::string::npos,
	      "no comment, CR or tab of the file's is kept");
	CHECK(written.rfind("<SCREEN>\n  <NAME>LAYOUT</NAME>\n", 0) == 0, "the writer's layout");
	return true;
}

mnu::Appearance appearance(const char *state, const char *value) {
	mnu::Appearance a;
	a.state = state;
	a.type = "IMAGE";
	a.value = value;
	return a;
}

const char *const kTable =
    "<SCREEN>\n"
    "  <NAME>T</NAME>\n"
    "  <WINDOW type=\"table\" name=\"TBL\">\n"
    "    <COLUMN count=\"3\">\n"
    "      <HEADER width=\"10\">A</HEADER>\n"
    "      <HEADER column=\"1\" PRIMARY_SORT>B</HEADER>\n"
    "      <HEADER>C</HEADER>\n"
    "      <BODY CUSTOM_DRAW BITMAP_DRAW></BODY>\n"
    "    </COLUMN>\n"
    "    <POSITION><LEFT>0</LEFT></POSITION>\n"
    "  </WINDOW>\n"
    "</SCREEN>\n";

// Edits over a document, each written and read back as the edited model.
struct CorpusEdit {
	const char *what;
	std::function<void(mnu::Document &)> edit;
};

void each_window(mnu::Window &w, const std::function<void(mnu::Window &)> &f) {
	f(w);
	for (mnu::WindowPart *part : {&w.list_box, &w.spinup, &w.spindown, &w.scrollbar})
		if (part->get()) each_window(*part->get(), f);
	for (mnu::Window &child : w.children) each_window(child, f);
}

void each_window(mnu::Document &doc, const std::function<void(mnu::Window &)> &f) {
	for (mnu::Screen &s : doc.screens)
		for (mnu::Window &root : s.roots) each_window(root, f);
}

const std::vector<CorpusEdit> &corpus_edits() {
	static const std::vector<CorpusEdit> edits = {
	    {"every window renamed", [](mnu::Document &d) { each_window(d, [](mnu::Window &w) { if (!w.name.empty()) w.name += "X"; }); }},
	    {"every left edge moved", [](mnu::Document &d) {
		     each_window(d, [](mnu::Window &w) { if (w.position.has_left) w.position.left += 7; });
	     }},
	    {"every first child window removed", [](mnu::Document &d) {
		     each_window(d, [](mnu::Window &w) { if (!w.children.empty()) w.children.erase(w.children.begin()); });
	     }},
	    {"an APPEARANCE added to every root", [](mnu::Document &d) {
		     for (mnu::Screen &s : d.screens)
			     for (mnu::Window &root : s.roots) root.appearances.push_back(appearance("DISABLED", "added.tga"));
	     }},
	    {"every root's windows reversed", [](mnu::Document &d) {
		     for (mnu::Screen &s : d.screens)
			     for (mnu::Window &root : s.roots) std::reverse(root.children.begin(), root.children.end());
	     }},
	    {"every STRING and ACTION text changed", [](mnu::Document &d) {
		     each_window(d, [](mnu::Window &w) {
			     if (w.string_data.present) w.string_data.value += "_";
			     for (mnu::Action &a : w.actions) a.target += "_";
		     });
	     }},
	    {"every root hidden toggled", [](mnu::Document &d) {
		     for (mnu::Screen &s : d.screens)
			     for (mnu::Window &root : s.roots) root.hidden = !root.hidden;
	     }},
	    {"every last SOUND removed", [](mnu::Document &d) {
		     each_window(d, [](mnu::Window &w) { if (!w.sounds.empty()) w.sounds.pop_back(); });
	     }},
	    {"every ITEM's text changed and a row added", [](mnu::Document &d) {
		     each_window(d, [](mnu::Window &w) {
			     for (mnu::Item &i : w.items.items) i.text += "+";
			     // A row of the kind the window's type reads (a TABLE's are ROWs of cells).
			     if (w.items.present && mnu::schema_reads(w.type, "items.item") == mnu::SchemaApplies::Reads) {
				     mnu::Item extra;
				     extra.text = "added";
				     w.items.items.push_back(extra);
			     }
		     });
	     }},
	    {"every table's first header widened", [](mnu::Document &d) {
		     each_window(d, [](mnu::Window &w) {
			     auto &headers = w.table_data.column.headers;
			     if (!headers.empty()) {
				     headers[0].has_width = true;
				     headers[0].width += 5;
			     }
		     });
	     }},
	};
	return edits;
}

bool test_edits_read_back() {
	for (const char *text : {kHand.c_str(), kTable, kCanonicalPretty}) {
		mnu::Document doc;
		CHECK(parse(text, doc), "parse");
		for (const CorpusEdit &e : corpus_edits()) {
			mnu::Document d = doc;
			e.edit(d);
			if (!mnu::write_issues(d).empty()) continue; // the writer refuses it
			if (!reads_back(d)) {
				std::fprintf(stderr, "  edit: %s\n", e.what);
				CHECK(false, "an edit reads back as the edited model");
			}
		}
	}
	return true;
}

// A window as the shipped menus write a button: its ACTION, its APPEARANCEs, its SOUNDs, then its
// POSITION and STRING.
const char *const kButton =
    "<SCREEN>\r\n"
    "\t<NAME>B</NAME>\r\n"
    "\t<WINDOW type=\"button\" name=\"SINGLE_PLAYER\">\r\n"
    "\t\t<ACTION type=\"SCREEN\" file=\"sp.mnu\">SP</ACTION>\r\n"
    "\t\t<APPEARANCE type=\"image\" state=\"default\">d.tga</APPEARANCE>\r\n"
    "\t\t<APPEARANCE type=\"image\" state=\"mouseover\">m.tga</APPEARANCE>\r\n"
    "\t\t<APPEARANCE type=\"image\" state=\"selected\">s.tga</APPEARANCE>\r\n"
    "\t\t<APPEARANCE type=\"image\" state=\"disabled\">x.tga</APPEARANCE>\r\n"
    "\t\t<SOUND state=\"MOUSEIN\" trigger=\"MOUSE_OVER\">in.lwf</SOUND>\r\n"
    "\t\t<SOUND state=\"selected\" trigger=\"CLICK_SELECT\">sel.lwf</SOUND>\r\n"
    "\t\t<POSITION><LEFT>1</LEFT><TOP>2</TOP></POSITION>\r\n"
    "\t\t<STRING type=\"id\">SP_KEY</STRING>\r\n"
    "\t</WINDOW>\r\n"
    "</SCREEN>\r\n";

// --- seeded rounds of list edits ---------------------------------------------------------------

// One list of a window: how many records it holds, and an edit of it (ListEdit).
enum ListEdit { kLastToFront, kFirstToEnd, kReversed, kNewAtFront, kNewAfterFirst, kMiddleRemoved, kListEdits };
const char *const kListEditNames[] = {"the last moved to the front", "the first moved to the end", "reversed",
                                      "a new record at the front", "a new record after the first",
                                      "the middle record removed"};

template <typename T> void edit_list(std::vector<T> &v, int kind, const T &fresh) {
	switch (kind) {
	case kLastToFront:
		if (v.size() > 1) std::rotate(v.begin(), v.end() - 1, v.end());
		break;
	case kFirstToEnd:
		if (v.size() > 1) std::rotate(v.begin(), v.begin() + 1, v.end());
		break;
	case kReversed: std::reverse(v.begin(), v.end()); break;
	case kNewAtFront: v.insert(v.begin(), fresh); break;
	case kNewAfterFirst: v.insert(v.begin() + std::min<std::ptrdiff_t>(1, std::ptrdiff_t(v.size())), fresh); break;
	case kMiddleRemoved:
		if (!v.empty()) v.erase(v.begin() + std::ptrdiff_t(v.size() / 2));
		break;
	}
}

struct WindowList {
	const char *name;
	std::function<void(mnu::Window &, int)> edit;
	const char *path = ""; // the list's schema path, when only some types read it (the edit skips the others)
};

mnu::Window fresh_window() {
	mnu::Window w;
	w.name = "FRESH";
	w.type = mnu::WindowType::Static;
	w.position.has_left = true;
	w.position.left = 5;
	return w;
}

const std::vector<WindowList> &window_lists() {
	static const std::vector<WindowList> lists = {
	    {"APPEARANCE", [](mnu::Window &w, int k) { edit_list(w.appearances, k, appearance("DISABLED", "fresh.tga")); }},
	    {"SOUND", [](mnu::Window &w, int k) {
		     mnu::Sound s;
		     s.state = "MOUSEOUT";
		     s.trigger = "MOUSE_OVER";
		     s.file = "fresh.lwf";
		     edit_list(w.sounds, k, s);
	     }},
	    {"ACTION", [](mnu::Window &w, int k) {
		     mnu::Action a;
		     a.type = "SCREEN";
		     a.file = "fresh.mnu";
		     a.target = "FRESH";
		     edit_list(w.actions, k, a);
	     }},
	    {"HOTKEY", [](mnu::Window &w, int k) {
		     mnu::Hotkey h;
		     h.value = "Z";
		     edit_list(w.hotkeys, k, h);
	     }},
	    {"WINDOW", [](mnu::Window &w, int k) { edit_list(w.children, k, fresh_window()); }},
	    {"SHUTTLE", [](mnu::Window &w, int k) { edit_list(w.shuttle, k, appearance("DEFAULT", "fresh.tga")); }, "shuttle"},
	    {"SCROLLDOWN", [](mnu::Window &w, int k) { edit_list(w.scrolldown, k, appearance("DEFAULT", "fresh.tga")); },
	     "scrolldown"},
	    {"DATASOURCE", [](mnu::Window &w, int k) { edit_list(w.datasources, k, std::string("fresh.kda")); }, "datasource"},
	    {"ITEM", [](mnu::Window &w, int k) {
		     mnu::Item i;
		     i.text = "fresh";
		     edit_list(w.items.items, k, i);
	     }, "items.item"},
	    {"ITEMS APPEARANCE", [](mnu::Window &w, int k) { edit_list(w.items.appearances, k, appearance("DEFAULT", "row.tga")); },
	     "items.appearance"},
	    {"HEADER", [](mnu::Window &w, int k) {
		     mnu::TableHeader h;
		     h.text = "H";
		     h.has_column = true;
		     edit_list(w.table_data.column.headers, k, h);
	     }, "column"},
	};
	return lists;
}

std::vector<mnu::Window *> all_windows(mnu::Document &doc) {
	std::vector<mnu::Window *> out;
	each_window(doc, [&](mnu::Window &w) { out.push_back(&w); });
	return out;
}

struct FuzzCount {
	int rounds = 0;
	int refused = 0;   // a write issue: the writer refuses the edited model
	int canonical = 0; // the writer's form does not read back as the model (a model no file holds)
	int failed = 0;
};

// `rounds` seeded rounds over a document, each one to three list edits of windows drawn at random:
// each edited model, written, reads back as the edited model. A model the writer refuses is counted
// apart, and so is one whose order of records the reader reads otherwise.
void fuzz_lists(const mnu::Document &doc, const std::string &label, int rounds, uint32_t seed, FuzzCount &count) {
	std::mt19937 rng(seed);
	const std::vector<WindowList> &lists = window_lists();
	for (int r = 0; r < rounds; ++r) {
		mnu::Document d = doc;
		std::string what;
		const int edits = 1 + int(rng() % 3);
		for (int k = 0; k < edits; ++k) {
			const std::vector<mnu::Window *> windows = all_windows(d);
			if (windows.empty()) break;
			mnu::Window &w = *windows[rng() % windows.size()];
			const WindowList &list = lists[rng() % lists.size()];
			const int kind = int(rng() % kListEdits);
			if (*list.path && mnu::schema_reads(w.type, list.path) != mnu::SchemaApplies::Reads) continue;
			list.edit(w, kind);
			what += std::string(what.empty() ? "" : "; ") + list.name + " of " + (w.name.empty() ? "?" : w.name) + ": " +
			        kListEditNames[kind];
		}
		++count.rounds;
		if (!mnu::write_issues(d).empty()) {
			++count.refused;
			continue;
		}
		const std::string own = canonical(d);
		mnu::Document again;
		std::string error;
		if (!mnu::parse(own, again, error)) {
			++count.failed;
			if (count.failed <= 3)
				std::fprintf(stderr, "  %s, round %d (%s): does not read: %s\n", label.c_str(), r, what.c_str(), error.c_str());
		} else if (canonical(again) != own) {
			++count.canonical;
		}
	}
}

bool test_list_rounds() {
	std::vector<std::string> texts = {kHand, kTable, kCanonicalPretty, kButton};
	FuzzCount count;
	for (size_t t = 0; t < texts.size(); ++t) {
		mnu::Document doc;
		CHECK(parse(texts[t], doc), "parse");
		fuzz_lists(doc, "text " + std::to_string(t), 120, uint32_t(1234 + t), count);
	}
	std::printf("(%d rounds, %d refused, %d read in another order) ", count.rounds, count.refused, count.canonical);
	CHECK(count.failed == 0, "every round of list edits reads back as the edited model");
	return true;
}

// --- the retail corpus -------------------------------------------------------------------------

struct Corpus {
	int checked = 0;
	int edits = 0;
	int refused = 0; // edits that leave a write issue (a window with no element): the writer refuses them
	int failed = 0;
	std::set<std::string> distinct; // each distinct file's bytes: the list rounds run once a file
	FuzzCount rounds;
};

void check_menu(Corpus &corpus, const std::string &label, const std::string &bytes) {
	mnu::Document doc;
	std::string error;
	++corpus.checked;
	if (!mnu::parse(bytes, doc, error)) {
		std::printf("  FAIL %s (parse: %s)\n", label.c_str(), error.c_str());
		++corpus.failed;
		return;
	}
	// Read, written and read again: the model it was.
	if (!reads_back(doc)) {
		std::printf("  FAIL %s (written, it reads back as another model)\n", label.c_str());
		++corpus.failed;
		return;
	}
	if (corpus.distinct.insert(bytes).second) {
		const int failed = corpus.rounds.failed;
		fuzz_lists(doc, label, 30, uint32_t(corpus.distinct.size()), corpus.rounds);
		if (corpus.rounds.failed != failed) {
			std::printf("  FAIL %s (%d round(s) of list edits do not read)\n", label.c_str(), corpus.rounds.failed - failed);
			++corpus.failed;
		}
	}
	for (const CorpusEdit &e : corpus_edits()) {
		mnu::Document d = doc;
		e.edit(d);
		if (!mnu::write_issues(d).empty()) {
			++corpus.refused;
			continue;
		}
		++corpus.edits;
		if (!reads_back(d)) {
			std::printf("  FAIL %s (%s: the written menu reads back as another model)\n", label.c_str(), e.what);
			++corpus.failed;
		}
	}
	std::printf("  OK   %s (%zu bytes; it and the edits read back)\n", label.c_str(), bytes.size());
}

void sweep_mount(Corpus &corpus, const std::string &install, const std::string &expansion) {
	opennova::Vfs vfs;
	if (!vfs.mount_game(install, expansion, opennova::VfsMountMode::Packed)) {
		std::printf("  FAIL mount_game(%s, %s): %s\n", install.c_str(), expansion.c_str(), vfs.last_error().c_str());
		++corpus.failed;
		return;
	}
	for (const auto &loc : vfs.list_files()) {
		const std::string &name = loc.logical_name;
		if (name.size() < 4) continue;
		std::string ext = name.substr(name.size() - 4);
		for (char &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		if (ext != ".mnu") continue;
		std::vector<uint8_t> bytes;
		if (!vfs.read_file_raw(name, bytes)) {
			std::printf("  FAIL %s (unreadable)\n", name.c_str());
			++corpus.failed;
			continue;
		}
		check_menu(corpus, (expansion.empty() ? std::string("<install>/") : expansion + "/") + name,
		           std::string(bytes.begin(), bytes.end()));
	}
}

int retail_leg() {
	const std::string install = retail::install();
	if (install.empty()) return retail::skip_leg("OPENNOVA_JO_DIR (the packed install's .mnu set)");
	Corpus corpus;
	sweep_mount(corpus, install, std::string());
	for (const std::string &expansion : retail::expansions()) sweep_mount(corpus, install, expansion);
	// The reference set's shipped menus, when the asset tree is there too.
	static const char *const kMenus[] = {"jo_main", "jo_sp", "jo_mp", "jo_options", "jo_game", "jo_player",
	                                     "jo_weapon", "jo_loadout", "jo_color", "jo_cmap", "jo_stat", "jo_death",
	                                     "jo_vehicle", "jo_item_db", "jo_splash"};
	int reference = 0;
	for (const char *name : kMenus) {
		const std::string path = retail::reference_fixture((std::string("mnu/") + name + ".mnu").c_str());
		std::string bytes;
		if (path.empty() || !test_io::read_file_text(path.c_str(), bytes)) continue;
		++reference;
		check_menu(corpus, std::string("reference/") + name + ".mnu", bytes);
	}
	if (corpus.checked == 0) {
		std::fprintf(stderr, "\nthe packed install served no .mnu\n");
		return 1;
	}
	std::printf("retail leg: %d menu(s) (%d from the reference set; %zu distinct files), each read back as the model "
	            "it was; %d edited menus read back as edited (%d edits refused for a write issue); %d rounds of list "
	            "edits over the distinct files (%d refused, %d read in another order); %d failure(s)\n",
	            corpus.checked, reference, corpus.distinct.size(), corpus.edits, corpus.refused, corpus.rounds.rounds,
	            corpus.rounds.refused, corpus.rounds.canonical, corpus.failed);
	return corpus.failed == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
#define RUN(name)                                                    \
	do {                                                             \
		std::printf("Running " #name "... ");                        \
		std::fflush(stdout);                                         \
		std::printf("%s\n", name() ? "OK" : "FAILED");               \
	} while (0)
	RUN(test_canonical_bytes);
	RUN(test_hand_written);
	RUN(test_edits_read_back);
	RUN(test_list_rounds);
	if (g_failed > 0) {
		std::fprintf(stderr, "\n%d check(s) FAILED\n", g_failed);
		return 1;
	}
	return retail_leg();
}
