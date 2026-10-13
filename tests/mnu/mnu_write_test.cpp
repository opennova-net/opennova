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

// The edited model, written, reads back as the edited model.
bool reads_back(const mnu::Document &edited, std::string *written = nullptr) {
	const std::string text = mnu::serialize(edited);
	if (written) *written = text;
	mnu::Document again;
	std::string error;
	if (!mnu::parse(text, again, error)) {
		std::fprintf(stderr, "  the written menu does not read: %s\n", error.c_str());
		return false;
	}
	if (canonical(again) != canonical(edited)) {
		std::fprintf(stderr, "  the written menu reads back as another model:\n%s\n", text.c_str());
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

// The bytes the writer puts down for the document below (captured once from the writer at
// f1c26a276): pretty with two spaces a step, and on one line.
const char *const kCanonicalPretty =
    "<SCREEN>\n"
    "  <NAME>EVERY</NAME>\n"
    "  <MUSICVAR>3</MUSICVAR>\n"
    "  <WINDOW type=\"TABLE\" name=\"ALL\" DRAW_FRAME HIDDEN MODAL READONLY DISABLE CHECKED AS_BUTTON NUMBER MINVAL=\"-1\" MAXVAL=\"99\" MAXCHAR=\"2\" GLOBAL_VAR PASSWORD FORM=\"3\" SERVERLIST PLAYERLIST>\n"
    "    <GROUP>2</GROUP>\n"
    "    <HOTKEY VIRTUAL>VK_ESCAPE</HOTKEY>\n"
    "    <HOTKEY>=</HOTKEY>\n"
    "    <ACTION type=\"WINDOW\" state=\"SHOW\" file=\"f.mnu\" NAME=\"slot\" target_form=\"0\" TOGGLE test=\"EQ\" EXTERNAL_BROWSER> PANEL </ACTION>\n"
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
    "    <WIDTH>20</WIDTH>\n"
    "    <ORIENTATION>HORIZONTAL</ORIENTATION>\n"
    "    <APPEARANCE type=\"image\" state=\"default\" map_state=\"1\" height=\"24\" flags=\"STANDARD_TRANSPARENT\">a.tga</APPEARANCE>\n"
    "    <APPEARANCE type=\"image\" state=\"mouseover\" map_state=\"1\" height=\"24\" flags=\"STANDARD_TRANSPARENT\">b&amp;c&lt;d>.tga</APPEARANCE>\n"
    "    <SHUTTLE type=\"image\" state=\"default\" map_state=\"1\" height=\"24\" flags=\"STANDARD_TRANSPARENT\">s1.tga</SHUTTLE>\n"
    "    <SCROLLUP type=\"image\" state=\"default\" map_state=\"1\" height=\"24\" flags=\"STANDARD_TRANSPARENT\">u1.tga</SCROLLUP>\n"
    "    <SCROLLDOWN type=\"image\" state=\"default\" map_state=\"1\" height=\"24\" flags=\"STANDARD_TRANSPARENT\">d1.tga</SCROLLDOWN>\n"
    "    <TEXT_RSRC>menutxt.BIN</TEXT_RSRC>\n"
    "    <DATASOURCE>nlist.kda</DATASOURCE>\n"
    "    <DATASOURCE>credits.ini</DATASOURCE>\n"
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
    "    <ITEMS justify=\"LEFT\" vjustify=\"TOP\" MULTISELECT>\n"
    "      <APPEARANCE type=\"image\" state=\"default\" map_state=\"1\" height=\"24\" flags=\"STANDARD_TRANSPARENT\">row.tga</APPEARANCE>\n"
    "      <ITEM type=\"ID\" value=\"1\" justify=\"RIGHT\" vjustify=\"CENTER\" PAIRS_LIST>T</ITEM>\n"
    "      <ROW>\n"
    "        <ITEM type=\"BITMAP\" column=\"1\">c.tga</ITEM>\n"
    "        <ITEM type=\"BITMAP\" column=\"1\">c.tga</ITEM>\n"
    "      </ROW>\n"
    "      <ROW>\n"
    "      </ROW>\n"
    "    </ITEMS>\n"
    "    <LIST_BOX sb_edge_pad=\"6\" name=\"DROP\">\n"
    "      <POSITION>\n"
    "        <TOP>9</TOP>\n"
    "      </POSITION>\n"
    "      <SCROLLBAR>\n"
    "        <SHUTTLE type=\"image\" state=\"default\" map_state=\"1\" height=\"24\" flags=\"STANDARD_TRANSPARENT\">sh.tga</SHUTTLE>\n"
    "      </SCROLLBAR>\n"
    "    </LIST_BOX>\n"
    "    <SPINUP>\n"
    "      <APPEARANCE type=\"image\" state=\"default\" map_state=\"1\" height=\"24\" flags=\"STANDARD_TRANSPARENT\">up.tga</APPEARANCE>\n"
    "    </SPINUP>\n"
    "    <SPINDOWN>\n"
    "      <STRING>-</STRING>\n"
    "    </SPINDOWN>\n"
    "    <COLUMN count=\"3\" spacing=\"2\">\n"
    "      <HEADER justify=\"CENTER\" SECONDARY_SORT TERTIARY_SORT column=\"0\" sort=\"A\" width=\"100\" type=\"id\">H0</HEADER>\n"
    "      <HEADER justify=\"CENTER\" DEFAULT_SORT column=\"2\" sort=\"A\" width=\"100\" type=\"id\">H2</HEADER>\n"
    "      <BODY column=\"1\" BITMAP_TEXT CUSTOM_DRAW BITMAP_DRAW BITMAP_FLAGS=\"STANDARD\" SCALE_BITMAP></BODY>\n"
    "      <SUBST column=\"0\" value=\"1\" URL FILE>x.tga</SUBST>\n"
    "    </COLUMN>\n"
    "    <MIN_ITEM_HEIGHT>20</MIN_ITEM_HEIGHT>\n"
    "    <FIXED_HEADER_HEIGHT>0</FIXED_HEADER_HEIGHT>\n"
    "    <SCROLLBAR>\n"
    "      <SCROLLDOWN type=\"image\" state=\"default\" map_state=\"1\" height=\"24\" flags=\"STANDARD_TRANSPARENT\">dn.tga</SCROLLDOWN>\n"
    "    </SCROLLBAR>\n"
    "    <JOIN_BUTTON name=\"J\" BARE EMPTY=\"\">note<TARGET>http://x</TARGET></JOIN_BUTTON>\n"
    "    <FILTERS>\n"
    "      <TARGET>http://x</TARGET>\n"
    "      <JOIN_BUTTON name=\"J\" BARE EMPTY=\"\">note<TARGET>http://x</TARGET></JOIN_BUTTON>\n"
    "    </FILTERS>\n"
    "    <TARGET>http://x</TARGET>\n"
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
    "<SCREEN><NAME>EVERY</NAME><MUSICVAR>3</MUSICVAR><"
    "WINDOW type=\"TABLE\" name=\"ALL\" DRAW_FRAME HIDDEN MODAL READONLY DISABLE CHECKED AS_BUTTON NUMBER MINVAL=\"-1\" MAXVAL=\"99\" MAXCHAR=\"2\" GLOBAL_VAR PASSWORD FORM=\"3\" SERVERLIST PLAYERLIST><"
    "GROUP>2</GROUP><HOTKEY VIRTUAL>VK_ESCAPE</HOTKEY><HOTKEY>=</HOTKEY><"
    "ACTION type=\"WINDOW\" state=\"SHOW\" file=\"f.mnu\" NAME=\"slot\" target_form=\"0\" TOGGLE test=\"EQ\" EXTERNAL_BROWSER> PANEL </ACTION><"
    "ACTION type=\"SCREEN\" file=\"next.mnu\">NEXT</ACTION><FRAME><"
    "STENCIL size=\"8\" insetx=\"5\">s.tga</STENCIL><BRUSH>b.tga</BRUSH><MONOGRAM>m.tga</MONOGRAM><"
    "/FRAME><POSITION><LEFT>1</LEFT><TOP>2</TOP><RIGHT>300</RIGHT><BOTTOM>40</BOTTOM><"
    "/POSITION><WIDTH>20</WIDTH><ORIENTATION>HORIZONTAL</ORIENTATION><"
    "APPEARANCE type=\"image\" state=\"default\" map_state=\"1\" height=\"24\" flags=\"STANDARD_TRANSPARENT\">a.tga</APPEARANCE><"
    "APPEARANCE type=\"image\" state=\"mouseover\" map_state=\"1\" height=\"24\" flags=\"STANDARD_TRANSPARENT\">b&amp;c&lt;d>.tga</APPEARANCE><"
    "SHUTTLE type=\"image\" state=\"default\" map_state=\"1\" height=\"24\" flags=\"STANDARD_TRANSPARENT\">s1.tga</SHUTTLE><"
    "SCROLLUP type=\"image\" state=\"default\" map_state=\"1\" height=\"24\" flags=\"STANDARD_TRANSPARENT\">u1.tga</SCROLLUP><"
    "SCROLLDOWN type=\"image\" state=\"default\" map_state=\"1\" height=\"24\" flags=\"STANDARD_TRANSPARENT\">d1.tga</SCROLLDOWN><"
    "TEXT_RSRC>menutxt.BIN</TEXT_RSRC><DATASOURCE>nlist.kda</DATASOURCE><"
    "DATASOURCE>credits.ini</DATASOURCE><PRIVATE_DATA>p</PRIVATE_DATA><CURSOR><"
    "FILE>c.tga</FILE><FLAGS>STANDARD</FLAGS></CURSOR><FONT><NAME>f.fnt</NAME><"
    "DEFAULT_FG>FF000001</DEFAULT_FG><MOUSEOVER_BG>%DEF_BG%</MOUSEOVER_BG><"
    "DISABLED_FG>3</DISABLED_FG></FONT><"
    "STRING type=\"ID\" justify=\"CENTER\" vjustify=\"BOTTOM\" edge=\"4\" WRAP>KEY</STRING><"
    "TOGGLE_STRING type=\"ID\">ALT</TOGGLE_STRING><"
    "SOUND state=\"MOUSEIN\" trigger=\"MOUSE_OVER\">menu.lwf</SOUND><"
    "ITEMS justify=\"LEFT\" vjustify=\"TOP\" MULTISELECT><"
    "APPEARANCE type=\"image\" state=\"default\" map_state=\"1\" height=\"24\" flags=\"STANDARD_TRANSPARENT\">row.tga</APPEARANCE><"
    "ITEM type=\"ID\" value=\"1\" justify=\"RIGHT\" vjustify=\"CENTER\" PAIRS_LIST>T</ITEM><ROW><"
    "ITEM type=\"BITMAP\" column=\"1\">c.tga</ITEM><ITEM type=\"BITMAP\" column=\"1\">c.tga</ITEM><"
    "/ROW><ROW></ROW></ITEMS><LIST_BOX sb_edge_pad=\"6\" name=\"DROP\"><POSITION><TOP>9</TOP><"
    "/POSITION><SCROLLBAR><"
    "SHUTTLE type=\"image\" state=\"default\" map_state=\"1\" height=\"24\" flags=\"STANDARD_TRANSPARENT\">sh.tga</SHUTTLE><"
    "/SCROLLBAR></LIST_BOX><SPINUP><"
    "APPEARANCE type=\"image\" state=\"default\" map_state=\"1\" height=\"24\" flags=\"STANDARD_TRANSPARENT\">up.tga</APPEARANCE><"
    "/SPINUP><SPINDOWN><STRING>-</STRING></SPINDOWN><COLUMN count=\"3\" spacing=\"2\"><"
    "HEADER justify=\"CENTER\" SECONDARY_SORT TERTIARY_SORT column=\"0\" sort=\"A\" width=\"100\" type=\"id\">H0</HEADER><"
    "HEADER justify=\"CENTER\" DEFAULT_SORT column=\"2\" sort=\"A\" width=\"100\" type=\"id\">H2</HEADER><"
    "BODY column=\"1\" BITMAP_TEXT CUSTOM_DRAW BITMAP_DRAW BITMAP_FLAGS=\"STANDARD\" SCALE_BITMAP><"
    "/BODY><SUBST column=\"0\" value=\"1\" URL FILE>x.tga</SUBST></COLUMN><"
    "MIN_ITEM_HEIGHT>20</MIN_ITEM_HEIGHT><FIXED_HEADER_HEIGHT>0</FIXED_HEADER_HEIGHT><"
    "SCROLLBAR><"
    "SCROLLDOWN type=\"image\" state=\"default\" map_state=\"1\" height=\"24\" flags=\"STANDARD_TRANSPARENT\">dn.tga</SCROLLDOWN><"
    "/SCROLLBAR><JOIN_BUTTON name=\"J\" BARE EMPTY=\"\">note<TARGET>http://x</TARGET><"
    "/JOIN_BUTTON><FILTERS><TARGET>http://x</TARGET><"
    "JOIN_BUTTON name=\"J\" BARE EMPTY=\"\">note<TARGET>http://x</TARGET></JOIN_BUTTON></FILTERS><"
    "TARGET>http://x</TARGET><WINDOW type=\"static\" name=\"CHILD\"><POSITION><LEFT>7</LEFT><"
    "/POSITION></WINDOW><WINDOW type=\"window\" name=\"BARE\"></WINDOW></WINDOW><"
    "WINDOW type=\"static\" name=\"CHILD\"><POSITION><LEFT>7</LEFT></POSITION></WINDOW></SCREEN><"
    "SCREEN></SCREEN>";

mnu::Appearance image_row(const char *state, const char *value) {
	mnu::Appearance a;
	a.state = state;
	a.type = "image";
	a.value = value;
	a.has_map_state = true;
	a.map_state = 1;
	a.has_height = true;
	a.height = 24;
	a.flags = "STANDARD_TRANSPARENT";
	return a;
}

// A document built in code with no source text: every record the writer puts down, so its
// canonical bytes pin the writer's own layout.
mnu::Document minted() {
	mnu::Window w;
	w.name = "ALL";
	w.type = mnu::WindowType::Table;
	w.type_token = "TABLE";
	w.hidden = w.disabled = w.checked = w.draw_frame = w.modal = w.readonly = w.as_button = true;
	w.number = w.global_var = w.password = true;
	w.has_group = true;
	w.group = 2;
	w.has_minval = w.has_maxval = w.has_maxchar = w.has_form = true;
	w.minval = -1;
	w.maxval = 99;
	w.maxchar = 2;
	w.form = 3;
	w.orientation = "HORIZONTAL";
	w.has_scroll_extent = true;
	w.scroll_extent = 20;
	w.scroll_extent_is_width = true;
	w.position.has_left = w.position.has_top = w.position.has_right = w.position.has_bottom = true;
	w.position.left = 1;
	w.position.top = 2;
	w.position.right = 300;
	w.position.bottom = 40;
	w.appearances = {image_row("default", "a.tga"), image_row("mouseover", "b&c<d>.tga")};
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
	action.field_attr = "NAME";
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
	w.items.present = w.items.multiselect = true;
	w.items.justify = "LEFT";
	w.items.vjustify = "TOP";
	w.items.appearances = {image_row("default", "row.tga")};
	mnu::Item item;
	item.type = "ID";
	item.value = "1";
	item.text = "T";
	item.justify = "RIGHT";
	item.vjustify = "CENTER";
	item.pairs_list = true;
	mnu::Item cell;
	cell.type = "BITMAP";
	cell.text = "c.tga";
	cell.has_column = true;
	cell.column = 1;
	w.items.items = {item};
	mnu::TableRow row;
	row.cells = {cell, cell};
	w.items.rows = {row, mnu::TableRow{}};
	mnu::Window &list = w.list_box.author(mnu::WindowType::List);
	list.name = "DROP";
	list.position.has_top = true;
	list.position.top = 9;
	list.scrollbar.author(mnu::WindowType::Scroll).shuttle = {image_row("default", "sh.tga")};
	w.has_sb_edge_pad = true;
	w.sb_edge_pad = 6;
	w.spinup.author(mnu::WindowType::Button).appearances = {image_row("default", "up.tga")};
	w.spindown.author(mnu::WindowType::Button).string_data.present = true;
	w.spindown->string_data.value = "-";
	w.scrollbar.author(mnu::WindowType::Scroll).scrolldown = {image_row("default", "dn.tga")};
	w.cursor.file = "c.tga";
	w.cursor.flags = "STANDARD";
	w.has_text_rsrc = true;
	w.text_rsrc = "menutxt.BIN";
	w.private_data = "p";
	w.datasources = {"nlist.kda", "credits.ini"};
	mnu::Hotkey escape;
	escape.value = "VK_ESCAPE";
	escape.virtual_key = true;
	mnu::Hotkey equals;
	equals.value = "=";
	w.hotkeys = {escape, equals};
	w.shuttle = {image_row("default", "s1.tga")};
	w.scrollup = {image_row("default", "u1.tga")};
	w.scrolldown = {image_row("default", "d1.tga")};
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
	header.type = "id";
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
	body.bitmap_flags = "STANDARD";
	column.bodies = {body};
	mnu::TableSubst subst;
	subst.has_column = true;
	subst.value = "1";
	subst.is_file = subst.is_url = true;
	subst.file = "x.tga";
	column.substitutions = {subst};
	column.primary_sort = 2;
	column.primary_sort_token = "DEFAULT_SORT";
	column.secondary_sort = 0;
	column.tertiary_sort = 0;
	w.table_data.has_min_item_height = true;
	w.table_data.min_item_height = 20;
	w.table_data.has_fixed_header_height = true;
	w.table_data.fixed_header_height = 0;
	mnu::Element target;
	target.tag = "TARGET";
	target.text = "http://x";
	mnu::Element join;
	join.tag = "JOIN_BUTTON";
	join.attributes = {{"name", "J", true}, {"BARE", "", false}, {"EMPTY", "", true}};
	join.text = "note";
	join.children = {target};
	mnu::Element filters;
	filters.tag = "FILTERS";
	filters.text = "\n";
	filters.children = {target, join};
	w.extras = {join, filters, target};
	w.extra_attributes = {{"SERVERLIST", "", false}, {"PLAYERLIST", "", false}};
	mnu::Window child;
	child.name = "CHILD";
	child.type = mnu::WindowType::Static;
	child.position.has_left = true;
	child.position.left = 7;
	mnu::Window bare;
	bare.name = "BARE";
	w.children = {child, bare};

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
	CHECK(back.screens.size() == 2 && main_window(back).children.size() == 1, "the canonical file as the reader reads it");
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
	a.type = "image";
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
			     for (mnu::Window &root : s.roots) root.appearances.push_back(appearance("disabled", "added.tga"));
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
			     if (w.items.present) {
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
	    {"APPEARANCE", [](mnu::Window &w, int k) { edit_list(w.appearances, k, appearance("disabled", "fresh.tga")); }},
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
	    {"SHUTTLE", [](mnu::Window &w, int k) { edit_list(w.shuttle, k, appearance("default", "fresh.tga")); }},
	    {"SCROLLDOWN", [](mnu::Window &w, int k) { edit_list(w.scrolldown, k, appearance("default", "fresh.tga")); }},
	    {"DATASOURCE", [](mnu::Window &w, int k) { edit_list(w.datasources, k, std::string("fresh.kda")); }},
	    {"ITEM", [](mnu::Window &w, int k) {
		     mnu::Item i;
		     i.text = "fresh";
		     edit_list(w.items.items, k, i);
	     }},
	    {"ITEMS APPEARANCE", [](mnu::Window &w, int k) { edit_list(w.items.appearances, k, appearance("default", "row.tga")); }},
	    {"HEADER", [](mnu::Window &w, int k) {
		     mnu::TableHeader h;
		     h.text = "H";
		     h.has_column = true;
		     edit_list(w.table_data.column.headers, k, h);
	     }},
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
