// The modeled text layout of a menu (formats/mnu/mnu_text_layout.h, D-MNU-22): a menu read and
// written again comes back byte-identical because the writer generates it so, from the records
// and the layout modeled beside them. Synthetic legs: a hand-written menu with comments, CR LF
// lines, tabs, odd spacing and quoting, aliases and a RAW_TEXT block round-trips; a value edit
// changes only its own tokens; an added record takes the writer's own layout in the file's style
// beside its neighbours; a removed one takes its own text and comments; a reordered list is
// written in the model's order; a document made in code writes the canonical bytes the writer
// put down before the layout model (pinned below). Every edit is checked against the reader: the
// written menu reads back as the edited model. The retail leg (--retail) sweeps every .mnu the
// packed install serves and the reference set's shipped menus: each reads and writes back
// byte-identical, and a set of edits over each reads back as the edited model.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <functional>
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

// The writer's own layout of a document: what it reads as, made comparable.
std::string canonical(const mnu::Document &doc) {
	mnu::Document copy = doc;
	copy.text_layout.reset();
	return mnu::serialize(copy);
}

bool parse(const std::string &text, mnu::Document &doc) {
	std::string error;
	if (!mnu::parse(text, doc, error)) {
		std::fprintf(stderr, "  parse: %s\n", error.c_str());
		return false;
	}
	return true;
}

std::string bytes_of(const mnu::Document &doc) {
	std::vector<uint8_t> bytes;
	std::string error;
	if (!mnu::serialize_bytes(doc, bytes, error)) return "<refused: " + error + ">";
	return std::string(bytes.begin(), bytes.end());
}

// The edited model written in its layout reads back as the edited model.
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

// --- a document made in code: the writer's own layout ---------------------------------------------

// The bytes the writer put down for the document below before the text layout model existed
// (captured once from the writer at f1c26a276): a document made in code still writes exactly
// these, pretty with two spaces a step and on one line.
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

bool test_canonical_without_layout() {
	const mnu::Document doc = minted();
	CHECK(!doc.text_layout, "a document made in code holds no layout");
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
	// The writer's own bytes, read and written again, come back as they are.
	mnu::Document back;
	CHECK(parse(kCanonicalPretty, back), "parse");
	CHECK(back.text_layout != nullptr, "a document read from a file holds its layout");
	CHECK(same(mnu::serialize(back), kCanonicalPretty, "re-read:"), "a canonical file round-trips");
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

std::string replaced(std::string text, const std::string &from, const std::string &to) {
	const size_t at = text.find(from);
	if (at == std::string::npos) return "<missing: " + from + ">";
	return text.replace(at, from.size(), to);
}

mnu::Window &main_window(mnu::Document &doc) { return doc.screens.at(0).roots.at(0); }

bool test_hand_round_trip() {
	mnu::Document doc;
	CHECK(parse(kHand, doc), "parse");
	const mnu::Window &w = main_window(doc);
	CHECK(w.name == "MAIN" && w.children.size() == 2 && w.position.left == 10 && w.position.right == 110 &&
	          w.appearances.at(0).value == "back&drop.tga" && w.children[1].string_data.value == "a<b",
	      "the model as the reader reads it");
	CHECK(same(bytes_of(doc), kHand, "round trip:"), "a hand-written menu comes back byte-identical");
	// What follows the text's NUL is never read, and comes back too.
	const std::string tail = kHand + std::string("\0 after the end\r\n", 17);
	CHECK(parse(tail, doc) && same(bytes_of(doc), tail, "NUL tail:"), "what follows the NUL");
	// A UTF-8 file with its mark, and a UTF-16 one, each come back as they were.
	const std::string bom = "\xEF\xBB\xBF" + kHand;
	CHECK(parse(bom, doc) && doc.source_encoding == mnu::SourceEncoding::Utf8Bom && same(bytes_of(doc), bom, "BOM:"),
	      "UTF-8 with its mark");
	std::string wide = "\xFF\xFE";
	for (char c : kHand) {
		wide.push_back(c);
		wide.push_back('\0');
	}
	CHECK(parse(wide, doc) && doc.source_encoding == mnu::SourceEncoding::Utf16LE && same(bytes_of(doc), wide, "UTF-16:"),
	      "UTF-16 LE");
	return true;
}

bool test_value_edits() {
	mnu::Document doc;
	CHECK(parse(kHand, doc), "parse");
	struct Edit {
		const char *what;
		std::function<void(mnu::Document &)> edit;
		const char *from;
		const char *to;
	};
	const Edit edits[] = {
	    {"a STRING's text", [](mnu::Document &d) { main_window(d).children[0].string_data.value = "YES_KEY"; },
	     ">OK_KEY<", ">YES_KEY<"},
	    {"a keyword", [](mnu::Document &d) { main_window(d).appearances[0].state = "mouseover"; },
	     "state=\"default\" map_state", "state=\"mouseover\" map_state"},
	    {"an unquoted value, the writer's quoting", [](mnu::Document &d) { main_window(d).name = "MAIN2"; },
	     "name=MAIN ", "name=\"MAIN2\" "},
	    {"a text spelled with an entity", [](mnu::Document &d) { main_window(d).appearances[0].value = "front.tga"; },
	     "back&amp;drop.tga", "front.tga"},
	    {"a RAW_TEXT block's text", [](mnu::Document &d) { main_window(d).children[1].string_data.value = "c<d"; },
	     "<RAW_TEXT>a<b</RAW_TEXT>", "c&lt;d"},
	    {"a number spelled with a blank", [](mnu::Document &d) { d.screens[0].music_var = 3; },
	     "<MusicVar> 2</MusicVar>", "<MusicVar>3</MusicVar>"},
	    {"a value whose spelling held another attribute",
	     [](mnu::Document &d) { main_window(d).appearances[0].map_state = 1; }, "map_state=\"0 height=\"14\"",
	     "map_state=\"1\""},
	    {"the attribute a repeat stands beside: the repeat goes", [](mnu::Document &d) { main_window(d).children[0].name = "YES"; },
	     " name=\"OK\" name=\"DUP\"", " name=\"YES\""},
	    {"a scroll row closed by another name", [](mnu::Document &d) { main_window(d).scrollup[0].value = "dn.tga"; },
	     ">up.tga</APPEARANCE>", ">dn.tga</APPEARANCE>"},
	};
	for (const Edit &e : edits) {
		mnu::Document d = doc;
		e.edit(d);
		std::string written;
		if (!reads_back(d, &written)) {
			std::fprintf(stderr, "  edit: %s\n", e.what);
			CHECK(false, "an edit reads back as the edited model");
		}
		if (!same(written, replaced(kHand, e.from, e.to), e.what)) CHECK(false, "only the edited token changes");
	}
	// The left edge moved: ULX takes the new left, and WIDTH the number that keeps the right edge.
	mnu::Document d = doc;
	main_window(d).position.left = 20;
	std::string written;
	CHECK(reads_back(d, &written), "the edges read back");
	CHECK(same(written, replaced(replaced(kHand, "<ULX>10</ULX>", "<ULX>20</ULX>"), "<WIDTH>0100</WIDTH>", "<WIDTH>90</WIDTH>"),
	           "edges:"),
	      "WIDTH generated from the left before it");
	return true;
}

mnu::Appearance appearance(const char *state, const char *value) {
	mnu::Appearance a;
	a.state = state;
	a.type = "image";
	a.value = value;
	return a;
}

bool test_added_records() {
	mnu::Document doc;
	CHECK(parse(kHand, doc), "parse");
	std::string written;
	// A new APPEARANCE after the last one the window holds, at its indentation, in the file's style.
	mnu::Document d = doc;
	main_window(d).appearances.push_back(appearance("disabled", "x.tga"));
	CHECK(reads_back(d, &written), "reads back");
	CHECK(same(written, replaced(kHand, "<!-- same line -->", "<!-- same line -->\r\n\t\t<APPEARANCE type=\"image\" state=\"disabled\">x.tga</APPEARANCE>"),
	           "a new APPEARANCE:"),
	      "a new APPEARANCE beside the one before it");
	// A new child window after the last, each line of it in the file's style.
	d = doc;
	mnu::Window child;
	child.name = "NEW";
	child.type = mnu::WindowType::Static;
	child.position.has_left = true;
	child.position.left = 1;
	main_window(d).children.push_back(child);
	CHECK(reads_back(d, &written), "reads back");
	CHECK(same(written,
	           replaced(kHand, "a<b</RAW_TEXT></STRING>\r\n\t\t</WINDOW>",
	                    "a<b</RAW_TEXT></STRING>\r\n\t\t</WINDOW>\r\n\t\t<WINDOW type=\"static\" name=\"NEW\">\r\n\t\t\t"
	                    "<POSITION>\r\n\t\t\t\t<LEFT>1</LEFT>\r\n\t\t\t</POSITION>\r\n\t\t</WINDOW>"),
	           "a new WINDOW:"),
	      "a new WINDOW in the file's style");
	// New attributes after the writer's attribute before them, before the blanks that close a tag.
	d = doc;
	main_window(d).hidden = true;
	main_window(d).children[1].modal = true;
	CHECK(reads_back(d, &written), "reads back");
	CHECK(same(written,
	           replaced(replaced(kHand, "name=MAIN DISABLED >", "name=MAIN HIDDEN DISABLED >"), "name=\"CANCEL\">",
	                    "name=\"CANCEL\" MODAL>"),
	           "new attributes:"),
	      "new attributes beside their neighbours");
	// A new POSITION edge after the one before it in the writer's order.
	d = doc;
	main_window(d).position.has_bottom = true;
	main_window(d).position.bottom = 50;
	CHECK(reads_back(d, &written), "reads back");
	CHECK(same(written, replaced(kHand, "<WIDTH>0100</WIDTH>", "<WIDTH>0100</WIDTH>\r\n\t\t\t<BOTTOM>50</BOTTOM>"), "a new edge:"),
	      "a new edge");
	// A new element in an element that held none gets its close tag back on a line of its own.
	CHECK(parse("<SCREEN>\n  <NAME>S</NAME>\n  <WINDOW type=\"list\" name=\"L\">\n    <ITEMS></ITEMS>\n  </WINDOW>\n</SCREEN>\n", d),
	      "parse");
	mnu::Item item;
	item.text = "one";
	main_window(d).items.items.push_back(item);
	CHECK(reads_back(d, &written), "reads back");
	CHECK(same(written, "<SCREEN>\n  <NAME>S</NAME>\n  <WINDOW type=\"list\" name=\"L\">\n    <ITEMS>\n      <ITEM>one</ITEM>\n"
	                    "    </ITEMS>\n  </WINDOW>\n</SCREEN>\n",
	           "into an empty element:"),
	      "into an empty element");
	return true;
}

bool test_removed_records() {
	mnu::Document doc;
	CHECK(parse(kHand, doc), "parse");
	std::string written;
	mnu::Document d = doc;
	main_window(d).children.erase(main_window(d).children.begin());
	CHECK(reads_back(d, &written), "reads back");
	CHECK(same(written,
	           replaced(kHand,
	                    "\t\t<WINDOW type=\"button\" name=\"OK\" name=\"DUP\">\r\n\t\t\t<STRING type=\"id\" justify=\"CENTER\">"
	                    "OK_KEY</STRING>\r\n\t\t\t<SOUND state=\"selected\" trigger=\"CLICK_SELECT\" LOOP>menu.lwf</SOUND>\r\n"
	                    "\t\t</WINDOW>\r\n",
	                    ""),
	           "a removed WINDOW:"),
	      "a removed WINDOW takes its lines; the next one's comment stays");
	d = doc;
	main_window(d).children.pop_back();
	CHECK(reads_back(d, &written), "reads back");
	CHECK(same(written,
	           replaced(kHand,
	                    "\t\t<!-- the cancel button -->\r\n\t\t<WINDOW type=\"button\" name=\"CANCEL\">\r\n\t\t\t<STRING>"
	                    "<RAW_TEXT>a<b</RAW_TEXT></STRING>\r\n\t\t</WINDOW>\r\n",
	                    ""),
	           "a removed WINDOW and its comment:"),
	      "the comment before a removed WINDOW goes with it");
	d = doc;
	main_window(d).appearances.clear();
	CHECK(reads_back(d, &written), "reads back");
	CHECK(same(written,
	           replaced(kHand,
	                    "\t\t<!-- the backdrop -->\r\n\t\t<APPEARANCE TYPE=\"IMAGE\" state=\"default\" map_state=\"0 height=\"14\">"
	                    "back&amp;drop.tga</APPEARANCE>  <!-- same line -->\r\n",
	                    ""),
	           "a removed APPEARANCE:"),
	      "the comments before and on a removed element's line go with it");
	d = doc;
	main_window(d).position = mnu::Position{};
	main_window(d).position.has_top = true;
	main_window(d).position.top = 5;
	CHECK(reads_back(d, &written), "reads back");
	CHECK(same(written, replaced(kHand, "\t\t\t<ULX>10</ULX>\r\n\t\t\t<WIDTH>0100</WIDTH>\r\n", ""), "removed edges:"),
	      "removed edges");
	return true;
}

bool test_reordered_records() {
	mnu::Document doc;
	CHECK(parse(kHand, doc), "parse");
	mnu::Document d = doc;
	std::swap(main_window(d).children[0], main_window(d).children[1]);
	std::string written;
	CHECK(reads_back(d, &written), "the model's order reads back");
	const size_t cancel = written.find("<!-- the cancel button -->\r\n\t\t<WINDOW type=\"button\" name=\"CANCEL\">");
	const size_t ok = written.find("name=\"OK\" name=\"DUP\"");
	CHECK(cancel != std::string::npos && ok != std::string::npos && cancel < ok,
	      "the moved WINDOW keeps its own comment and layout");
	return true;
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

// [orig: CTableWnd_ParseXMLContentDefinition @ 0x6427d0] The running index and the table sort
// keys read relative to the rows before them: the writer keeps the file's while they read as the
// model's and puts down its own where they no longer do.
bool test_column_rules() {
	mnu::Document doc;
	CHECK(parse(kTable, doc), "parse");
	mnu::TableColumn &column = main_window(doc).table_data.column;
	CHECK(column.headers[2].column == 1 && column.primary_sort == 0 && column.bodies[0].display == "CUSTOM_DRAW",
	      "the table as the reader reads it");
	CHECK(same(bytes_of(doc), kTable, "table:"), "a table comes back byte-identical");
	std::string written;
	mnu::Document d = doc;
	main_window(d).table_data.column.headers[0].column = 2;
	CHECK(reads_back(d, &written), "a row's COLUMN put down where the running index no longer carries it");
	CHECK(written.find("<HEADER column=\"2\" PRIMARY_SORT width=\"10\">A</HEADER>") != std::string::npos &&
	          written.find("<HEADER column=\"1\">B</HEADER>") != std::string::npos &&
	          written.find("<HEADER>C</HEADER>") != std::string::npos,
	      "the sort key moved to where it reads the model's");
	d = doc;
	main_window(d).table_data.column.primary_sort = 1;
	CHECK(reads_back(d, &written), "a sort key the model moved");
	CHECK(written.find("<HEADER PRIMARY_SORT column=\"1\">B</HEADER>") != std::string::npos, "on its own row");
	d = doc;
	main_window(d).table_data.column.bodies[0].display = "BITMAP_DRAW";
	CHECK(reads_back(d, &written), "a draw kind the model changed");
	CHECK(written.find("<BODY BITMAP_DRAW CUSTOM_DRAW></BODY>") != std::string::npos, "put down in the writer's order");
	d = doc;
	main_window(d).table_data.column.headers[1].text = "BB";
	CHECK(reads_back(d, &written), "a header's text");
	CHECK(same(written, replaced(kTable, ">B<", ">BB<"), "header text:"), "only its token changes");
	return true;
}

// Edits over a document, each written in its layout and read back as the edited model.
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

// Texts the shipped menus do not hold, each with the reader quirk it leans on: each comes back
// byte-identical, and every edit of it reads back as the edited model.
bool test_unusual_texts() {
	const char *const kTexts[] = {
	    // A tag name runs to a blank or '>', so <BR/> stays open and holds what follows; a close tag
	    // pops one level whatever it names, so </WINDOW> closes BR/ and the SCREEN is never closed.
	    "<SCREEN><NAME>U</NAME><WINDOW type=\"static\" name=\"A\"><BR/><POSITION><LEFT>1</LEFT></POSITION>"
	    "</WINDOW></SCREEN>",
	    // Blanks after '<', repeated singletons read into one record (two POSITIONs, two STRINGs, two
	    // FONT NAMEs) and one a later one replaces (TEXT_RSRC).
	    "<SCREEN>\n<NAME>R</NAME>\n< WINDOW type=\"static\" name=\"R\">\n <POSITION><LEFT>1</LEFT></POSITION>\n"
	    " <TEXT_RSRC>a</TEXT_RSRC>\n <POSITION><TOP>2</TOP></POSITION>\n <TEXT_RSRC>b</TEXT_RSRC>\n"
	    " <STRING>x</STRING>\n <STRING justify=\"LEFT\">y</STRING>\n <FONT><NAME>f1</NAME><NAME>f2</NAME></FONT>\n"
	    "</WINDOW>\n</SCREEN>\n",
	    // Two LIST_BOX elements read into one embedded list.
	    "<SCREEN><NAME>C</NAME><WINDOW type=\"combobox\" name=\"C\"><LIST_BOX name=\"L1\" sb_edge_pad=\"2\">"
	    "<POSITION><TOP>1</TOP></POSITION></LIST_BOX><LIST_BOX HIDDEN><APPEARANCE state=\"default\">a</APPEARANCE>"
	    "</LIST_BOX></WINDOW></SCREEN>",
	    // An APPEARANCE with no attribute stops the window's parse: what follows it is read for nothing.
	    "<SCREEN><NAME>S</NAME><WINDOW type=\"static\" name=\"S\"><APPEARANCE state=\"default\">a</APPEARANCE>"
	    "<APPEARANCE>stop</APPEARANCE><POSITION><LEFT>1</LEFT></POSITION><STRING>kept</STRING>"
	    "<WINDOW type=\"static\" name=\"LOST\"><POSITION><LEFT>1</LEFT></POSITION></WINDOW></WINDOW></SCREEN>",
	    // An element inside a text, entities, an attribute's trailing junk, an empty name, unquoted values.
	    "<SCREEN><NAME>T</NAME><WINDOW type=static name=\"T\"junk =\"x\"><POSITION><LEFT>1</LEFT></POSITION>"
	    "<STRING>a<FOO>b</FOO>c &quot;q&quot; &#65; &nbsp;</STRING></WINDOW></SCREEN>",
	    // The GOPHER's extra elements: text beside children, an element of blanks and children.
	    "<SCREEN><NAME>G</NAME>\r\n<WINDOW type=\"gopher\" name=\"G\">\r\n\t<POSITION><LEFT>1</LEFT></POSITION>\r\n"
	    "\t<TARGET url=\"x\">http</TARGET>\r\n\t<FILTERS>\r\n\t\t<TARGET>one</TARGET>\r\n\t</FILTERS>\r\n"
	    "\t<JOIN_BUTTON a b=\"1\" a>t<X>y</X>z</JOIN_BUTTON>\r\n</WINDOW>\r\n</SCREEN>\r\n",
	    // Two screens around an element the document level does not read, and a comment with no
	    // blank, whose search for "-->" starts after its name and runs to the end.
	    "<SCREEN><NAME>ONE</NAME><WINDOW type=\"static\" name=\"A\"><POSITION><LEFT>1</LEFT></POSITION></WINDOW>"
	    "</SCREEN>\n<OTHER>x</OTHER>\n<SCREEN><NAME>TWO</NAME><MUSICVAR>4</MUSICVAR></SCREEN>\n<!--x-->",
	    // Table sort keys before and after a row's COLUMN, a row the running index carries.
	    "<SCREEN><NAME>K</NAME><WINDOW type=\"table\" name=\"K\"><POSITION><LEFT>1</LEFT></POSITION>"
	    "<COLUMN count=\"4\"><HEADER DEFAULT_SORT column=\"2\" SECONDARY_SORT>a</HEADER><HEADER>b</HEADER>"
	    "<SUBST value=\"1\" FILE>x.tga</SUBST><HEADER column=\"3\" TERTIARY_SORT>c</HEADER>"
	    "<BODY BITMAP_TEXT CUSTOM_DRAW></BODY></COLUMN></WINDOW></SCREEN>",
	};
	// The one the layout does not keep: a close tag's attributes go to the element created last
	// [orig: NapiXML_ParseElementTree @ 0x769d70], here the APPEARANCE it closes. The writer puts the
	// value it read down in that element's own tag, and the close tag in its own spelling, so the
	// file reads back the same but not byte for byte.
	{
		mnu::Document doc;
		CHECK(parse("<SCREEN><NAME>S</NAME><WINDOW type=\"static\" name=\"A\"><APPEARANCE state=\"default\">a.tga"
		            "</APPEARANCE flags=\"X\"></WINDOW></SCREEN>",
		            doc),
		      "parse");
		CHECK(main_window(doc).appearances.at(0).flags == "X", "the close tag's attribute is the APPEARANCE's");
		CHECK(same(mnu::serialize(doc), "<SCREEN><NAME>S</NAME><WINDOW type=\"static\" name=\"A\"><APPEARANCE "
		                                "state=\"default\" flags=\"X\">a.tga</APPEARANCE></WINDOW></SCREEN>",
		           "a close tag's attribute:"),
		      "written in the open tag it belongs to");
	}
	for (const char *text : kTexts) {
		mnu::Document doc;
		CHECK(parse(text, doc), "parse");
		if (!same(mnu::serialize(doc), text, "unusual:")) CHECK(false, "an unusual text comes back byte-identical");
		for (const CorpusEdit &e : corpus_edits()) {
			mnu::Document d = doc;
			e.edit(d);
			if (!mnu::write_issues(d).empty()) continue; // the writer refuses it
			if (!reads_back(d)) {
				std::fprintf(stderr, "  text: %s\n  edit: %s\n", text, e.what);
				CHECK(false, "an edit of an unusual text reads back as the edited model");
			}
		}
	}
	return true;
}

// A record the layout does not hold takes the file's style: its line ending and indent step, or
// one line in a file of one line. A second copy of a record is the writer's own layout.
bool test_style_of_new_records() {
	mnu::Window extra;
	extra.name = "NEW";
	extra.type = mnu::WindowType::Static;
	extra.position.has_left = true;
	extra.position.left = 3;
	mnu::Document doc;
	std::string written;
	CHECK(parse("<SCREEN><NAME>S</NAME><WINDOW type=\"window\" name=\"R\"><POSITION><LEFT>1</LEFT></POSITION></WINDOW></SCREEN>", doc),
	      "parse");
	main_window(doc).children.push_back(extra);
	CHECK(reads_back(doc, &written), "reads back");
	CHECK(same(written,
	           "<SCREEN><NAME>S</NAME><WINDOW type=\"window\" name=\"R\"><POSITION><LEFT>1</LEFT></POSITION><WINDOW "
	           "type=\"static\" name=\"NEW\"><POSITION><LEFT>3</LEFT></POSITION></WINDOW></WINDOW></SCREEN>",
	           "one line:"),
	      "a file of one line keeps one line");
	CHECK(parse("<SCREEN>\n    <NAME>S</NAME>\n    <WINDOW type=\"window\" name=\"R\">\n        <POSITION>\n"
	            "            <LEFT>1</LEFT>\n        </POSITION>\n    </WINDOW>\n</SCREEN>\n",
	            doc),
	      "parse");
	main_window(doc).children.push_back(extra);
	mnu::Screen second;
	second.name = "TWO";
	second.roots.push_back(extra);
	doc.screens.push_back(second);
	CHECK(reads_back(doc, &written), "reads back");
	CHECK(same(written,
	           "<SCREEN>\n    <NAME>S</NAME>\n    <WINDOW type=\"window\" name=\"R\">\n        <POSITION>\n"
	           "            <LEFT>1</LEFT>\n        </POSITION>\n        <WINDOW type=\"static\" name=\"NEW\">\n"
	           "            <POSITION>\n                <LEFT>3</LEFT>\n            </POSITION>\n        </WINDOW>\n"
	           "    </WINDOW>\n</SCREEN>\n<SCREEN>\n    <NAME>TWO</NAME>\n    <WINDOW type=\"static\" name=\"NEW\">\n"
	           "        <POSITION>\n            <LEFT>3</LEFT>\n        </POSITION>\n    </WINDOW>\n</SCREEN>\n",
	           "four spaces:"),
	      "four spaces a step and LF, a new screen too");
	// A copy of a window beside it: the original keeps its layout, the copy is the writer's own.
	CHECK(parse(kHand, doc), "parse");
	mnu::Window copy = main_window(doc).children[1];
	main_window(doc).children.push_back(copy);
	CHECK(reads_back(doc, &written), "reads back");
	CHECK(written.find("<!-- the cancel button -->") == written.rfind("<!-- the cancel button -->") &&
	          written.find("\t\t<WINDOW type=\"button\" name=\"CANCEL\">\r\n\t\t\t<STRING>a&lt;b</STRING>\r\n\t\t</WINDOW>") !=
	              std::string::npos,
	      "the copy in the writer's own layout, in the file's style");
	// A document whose encoding changed is written in the writer's own layout.
	CHECK(parse(kHand, doc), "parse");
	doc.source_encoding = mnu::SourceEncoding::Utf8Bom;
	CHECK(mnu::serialize(doc) == canonical(doc), "a layout read as another encoding is not applied");
	return true;
}

// --- the retail corpus -------------------------------------------------------------------------

struct Corpus {
	int checked = 0;
	int identical = 0;
	int edits = 0;
	int refused = 0; // edits that leave a write issue (a window with no element): the writer refuses them
	int failed = 0;
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
	const std::string written = bytes_of(doc);
	if (written != bytes) {
		std::printf("  FAIL %s (written again it differs %s)\n", label.c_str(), first_difference(written, bytes).c_str());
		++corpus.failed;
		return;
	}
	++corpus.identical;
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
	std::printf("  OK   %s (%zu bytes, byte-identical; the edits read back)\n", label.c_str(), bytes.size());
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
	std::printf("retail leg: %d menu(s) (%d from the reference set), %d byte-identical; %d edited menus read back "
	            "as edited (%d edits refused for a write issue); %d failure(s)\n",
	            corpus.checked, reference, corpus.identical, corpus.edits - corpus.failed, corpus.refused, corpus.failed);
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
	RUN(test_canonical_without_layout);
	RUN(test_hand_round_trip);
	RUN(test_value_edits);
	RUN(test_added_records);
	RUN(test_removed_records);
	RUN(test_reordered_records);
	RUN(test_column_rules);
	RUN(test_edits_read_back);
	RUN(test_unusual_texts);
	RUN(test_style_of_new_records);
	if (g_failed > 0) {
		std::fprintf(stderr, "\n%d check(s) FAILED\n", g_failed);
		return 1;
	}
	return retail_leg();
}
