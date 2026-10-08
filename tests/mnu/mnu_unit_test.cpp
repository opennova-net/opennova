// Unit tests for mnu: the typed layer over the retail reader, and the writer.
// Each reading case pins one rule of docs/mnu/menu-re.md "The reader" (the
// 2026-09-23 grill, sets A1-A3, B1-B3, C1-C3), each writing case one of "The
// writer"; the corpus checks are mnu_compat (the differential and the fixed point)
// and mnu_coverage (no lost key).
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include <formats/mnu/mnu.h>
#include <formats/mnu/mnu_layout.h>

namespace {

namespace mnu = opennova::mnu;

#define CHECK(cond, msg)                                                   \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::cerr << "FAIL: " << msg << " at line " << __LINE__ << "\n";     \
      return false;                                                        \
    }                                                                      \
  } while (0)

// A screen named S holding `body` as its one root WINDOW's content.
std::string screen_of(const std::string &window_attrs, const std::string &body) {
  return "<SCREEN><NAME>S</NAME><WINDOW " + window_attrs + ">" + body + "</WINDOW></SCREEN>";
}

const char *kPos = "<POSITION><LEFT>0</LEFT></POSITION>";

bool parse(const std::string &xml, mnu::Document &doc, std::vector<mnu::ParseNote> *notes = nullptr) {
  std::string err;
  if (!mnu::parse(xml, doc, err, notes)) {
    std::cerr << "  parse: " << err << "\n";
    return false;
  }
  return true;
}

// A note whose key ends with `suffix`.
bool noted(const std::vector<mnu::ParseNote> &notes, const std::string &suffix) {
  for (const mnu::ParseNote &n : notes)
    if (n.key.size() >= suffix.size() && n.key.compare(n.key.size() - suffix.size(), suffix.size(), suffix) == 0)
      return true;
  return false;
}

const mnu::Window &root(const mnu::Document &doc) { return doc.screens.at(0).roots.at(0); }

// serialize -> parse -> serialize reproduces the bytes (the writer writes what the
// reader reads back).
bool fixed_point(const mnu::Document &doc) {
  const std::string first = mnu::serialize(doc);
  mnu::Document again;
  std::vector<mnu::ParseNote> notes;
  if (!parse(first, again, &notes)) return false;
  if (!notes.empty()) {
    std::cerr << "  the written menu reads with a note: " << notes[0].key << " " << notes[0].message << "\n";
    return false;
  }
  if (mnu::serialize(again) != first) {
    std::cerr << "  not a fixed point:\n" << first << "\n---\n" << mnu::serialize(again) << "\n";
    return false;
  }
  return true;
}

// --- the screen [orig: CUIScene_ParseNodeAttributes @ 0x639630] -------------------

bool test_screen_reads_name_musicvar_windows() {
  mnu::Document doc;
  std::vector<mnu::ParseNote> notes;
  CHECK(parse("<SCREEN NAME=\"ATTR\" MUSICVAR=\"9\"><NAME>ONE</NAME><MUSICVAR>2</MUSICVAR>"
              "<TEXT_RSRC>screen.bin</TEXT_RSRC><CURSOR><FILE>s.tga</FILE></CURSOR>"
              "<WINDOW type=\"window\" name=\"A\">" + std::string(kPos) + "</WINDOW>"
              "<NAME>TWO</NAME><MUSICVAR>3</MUSICVAR>"
              "<WINDOW type=\"window\" name=\"B\">" + std::string(kPos) + "</WINDOW></SCREEN>",
              doc, &notes),
        "parse");
  const mnu::Screen &s = doc.screens[0];
  CHECK(s.name == "TWO" && s.has_music_var && s.music_var == 3, "the last NAME and MUSICVAR win");
  CHECK(s.roots.size() == 2 && s.roots[0].name == "A" && s.roots[1].name == "B", "every root, in order");
  CHECK(noted(notes, "/SCREEN[0]@NAME") && noted(notes, "/SCREEN[0]@MUSICVAR"), "SCREEN attributes are noted");
  CHECK(noted(notes, "/SCREEN[0]/TEXT_RSRC") && noted(notes, "/SCREEN[0]/CURSOR"), "SCREEN TEXT_RSRC and CURSOR");
  CHECK(noted(notes, "/SCREEN[0]/NAME"), "the replaced NAME");
  CHECK(fixed_point(doc), "fixed point");
  // Duplicate screen names: the last one is found, as retail's newest-first walk does.
  CHECK(parse("<SCREEN><NAME>X</NAME><MUSICVAR>1</MUSICVAR></SCREEN><SCREEN><NAME>x</NAME><MUSICVAR>2</MUSICVAR></SCREEN>",
              doc),
        "parse dup");
  CHECK(doc.find_screen("X") && doc.find_screen("X")->music_var == 2, "find_screen: the last of the name");
  // Only top-level SCREENs are read: a wrapper or an XML declaration holds everything.
  CHECK(parse("<MNU><SCREEN><NAME>W</NAME></SCREEN></MNU>", doc, &notes) && doc.screens.empty() &&
            noted(notes, "/MNU"),
        "a wrapper element is not read");
  CHECK(parse("<?xml version=\"1.0\"?><SCREEN><NAME>W</NAME></SCREEN>", doc) && doc.screens.empty(),
        "after an XML declaration no screen loads");
  return true;
}

// [orig: CUIScene_CreateWidgetByType @ 0x64f630] The first child element and the
// last TYPE's first token; no TYPE or no child element creates nothing.
bool test_window_creation() {
  mnu::Document doc;
  std::vector<mnu::ParseNote> notes;
  CHECK(parse(screen_of("type=\"window\" name=\"R\"",
                        std::string(kPos) + "<WINDOW name=\"NOTYPE\">" + kPos + "</WINDOW>"
                        "<WINDOW type=\"static\" name=\"EMPTY\"></WINDOW>"
                        "<WINDOW type=\"list\" type=\"STATIC\" name=\"TWO\">" + kPos + "</WINDOW>"
                        "<WINDOW type='static' name=\"SQ\">" + kPos + "</WINDOW>"
                        "<WINDOW type=\"combo\" name=\"ALIAS\">" + kPos + "</WINDOW>"),
              doc, &notes),
        "parse");
  const mnu::Window &r = root(doc);
  CHECK(r.children.size() == 3, "NOTYPE and EMPTY are not created");
  CHECK(noted(notes, "WINDOW[NOTYPE]") && noted(notes, "WINDOW[EMPTY]"), "both are noted");
  CHECK(r.children[0].type == mnu::WindowType::Static && r.children[0].type_token == "STATIC", "the last TYPE");
  CHECK(r.children[1].type == mnu::WindowType::Window && r.children[1].type_token == "static'",
        "a single-quoted keyword keeps its quote and never matches");
  CHECK(r.children[2].type == mnu::WindowType::Window && r.children[2].type_token == "combo",
        "an OpenNova-only alias builds a generic window, its token kept");
  for (const char *alias : {"check", "combo", "spin", "multi", "map", "globe", "label", "goto", "marquee",
                            "multilineedit", "window"})
    CHECK(mnu::parse_window_type(alias) == mnu::WindowType::Window, std::string("alias ") + alias);
  for (int i = 0; i < mnu::kWindowTypeCount; ++i) {
    const auto t = static_cast<mnu::WindowType>(i);
    CHECK(mnu::parse_window_type(mnu::window_type_name(t)) == t, "factory names round-trip");
  }
  CHECK(mnu::parse_window_type("MarQuee_Wnd") == mnu::WindowType::Marquee, "ignoring case");
  return true;
}

// Flags are presence-only; DISABLE is the flag (DISABLED is not read); GROUP is a child
// element; NAME and FORM are the first authored.
bool test_window_attributes() {
  mnu::Document doc;
  std::vector<mnu::ParseNote> notes;
  CHECK(parse(screen_of("type=\"radio\" name=\"A\" name=\"B\" HIDDEN=\"0\" DISABLED GROUP=\"4\" FORM=\"2\" FORM=\"5\" "
                        "CHECKED=\"false\" MODAL",
                        std::string(kPos) + "<GROUP>7</GROUP>"),
              doc, &notes),
        "parse");
  const mnu::Window &w = root(doc);
  CHECK(w.name == "A" && w.form == 2, "the first authored NAME and FORM");
  CHECK(w.hidden && w.checked && w.modal, "presence flags, whatever their value");
  CHECK(!w.disabled && noted(notes, "@DISABLED"), "DISABLED is not the flag");
  CHECK(w.has_group && w.group == 7 && noted(notes, "@GROUP"), "GROUP: the element; the attribute noted");
  CHECK(parse(screen_of("type=\"static\" name=\"X\" DISABLE", kPos), doc) && root(doc).disabled, "DISABLE");
  const std::string out = mnu::serialize(doc);
  CHECK(out.find(" DISABLE>") != std::string::npos && out.find("disabled") == std::string::npos, "written DISABLE");
  return true;
}

// [orig: @ 0x648226 / 0x648909] An APPEARANCE with no attribute or no STATE retail
// knows, or a SOUND without STATE and TRIGGER, ends the base parse: the elements after
// it and the child windows are lost; the type's own elements (STRING) still read.
bool test_base_parse_stops() {
  mnu::Document doc;
  std::vector<mnu::ParseNote> notes;
  CHECK(parse(screen_of("type=\"static\" name=\"W\"",
                        "<APPEARANCE type=\"image\" state=\"default\">a.tga</APPEARANCE>"
                        "<WINDOW type=\"static\" name=\"EARLY\">" + std::string(kPos) + "</WINDOW>"
                        "<APPEARANCE type=\"image\" state=\"hover\">b.tga</APPEARANCE>"
                        "<POSITION><LEFT>5</LEFT></POSITION><STRING>kept</STRING>"
                        "<WINDOW type=\"static\" name=\"LATE\">" + kPos + "</WINDOW>"),
              doc, &notes),
        "parse");
  const mnu::Window &w = root(doc);
  CHECK(w.appearances.size() == 1 && !w.position.has_left, "the rest of the base parse is lost");
  CHECK(w.children.empty(), "no child window is created after the stop");
  CHECK(w.string_data.present && w.string_data.value == "kept", "the type's STRING still reads");
  CHECK(noted(notes, "/APPEARANCE") && noted(notes, "/POSITION") && noted(notes, "WINDOW[EARLY]"), "noted");
  CHECK(parse(screen_of("type=\"static\" name=\"W\"",
                        "<SOUND state=\"mousein\">menu.lwf</SOUND>" + std::string(kPos)),
              doc, &notes) &&
            root(doc).sounds.empty() && !root(doc).position.has_left,
        "a SOUND with no TRIGGER");
  CHECK(parse(screen_of("type=\"static\" name=\"W\"", "<APPEARANCE></APPEARANCE>" + std::string(kPos)), doc) &&
            !root(doc).position.has_left,
        "an APPEARANCE with no attribute");
  CHECK(parse(screen_of("type=\"static\" name=\"W\"",
                        "<SOUND state=\"SELECTED\" trigger=\"CLICK\" LOOP>c.lwf</SOUND>" + std::string(kPos)),
              doc, &notes) &&
            root(doc).sounds.size() == 1 && root(doc).sounds[0].file == "c.lwf" && noted(notes, "@LOOP"),
        "LOOP has no effect and is noted");
  return true;
}

// Element text is kept whole: no trim, no collapse; entities decode in text only.
bool test_text_rules() {
  mnu::Document doc;
  CHECK(parse(screen_of("type=\"static\" name=\"W&amp;\"",
                        std::string(kPos) + "<STRING type=\"id\">  KEY \r\n</STRING><HOTKEY> V</HOTKEY>"
                        "<APPEARANCE type=\"image\" state=\"default\">a&amp;b&#233;.tga</APPEARANCE>"),
              doc),
        "parse");
  const mnu::Window &w = root(doc);
  CHECK(w.name == "W&amp;", "attributes are raw");
  CHECK(w.string_data.value == "  KEY \r\n", "untrimmed text");
  CHECK(w.hotkeys[0].value == " V", "an untrimmed HOTKEY");
  // &#233; is U+FFE9 to retail's reader; a code-page menu narrows it as every retail
  // consumer does (WideCharToMultiByte on 1252): '?'.
  CHECK(w.appearances[0].value == "a&b?.tga", "decoded text, the numeric entity narrowed");
  CHECK(fixed_point(doc), "fixed point");
  return true;
}

// [orig: @ 0x648ae6] POSITION's edges are child elements read in order.
bool test_position_rules() {
  mnu::Document doc;
  std::vector<mnu::ParseNote> notes;
  CHECK(parse(screen_of("type=\"static\" name=\"W\"",
                        "<POSITION left=\"9\"><WIDTH>100</WIDTH><ULX>10</ULX><ULY>20</ULY><HEIGHT>40</HEIGHT>"
                        "</POSITION><POSITION><TOP>30</TOP></POSITION>"),
              doc, &notes),
        "parse");
  const mnu::Position &p = root(doc).position;
  CHECK(p.left == 10 && p.right == 100, "WIDTH adds to the left as it stood (0)");
  CHECK(p.top == 30 && p.bottom == 60, "a second POSITION reads into the same fields");
  CHECK(noted(notes, "/POSITION@LEFT"), "POSITION attributes are noted");
  const std::string out = mnu::serialize(doc);
  CHECK(out.find("<RIGHT>100</RIGHT>") != std::string::npos && out.find("WIDTH") == std::string::npos,
        "written as LEFT/TOP/RIGHT/BOTTOM");
  return true;
}

// [orig: @ 0x648ee2] The target is the text; FIELD/SOURCE/NAME share a slot (the first
// authored); flags by presence; rows kept in document order.
bool test_action_rules() {
  mnu::Document doc;
  std::vector<mnu::ParseNote> notes;
  CHECK(parse(screen_of("type=\"button\" name=\"B\"",
                        std::string(kPos) +
                            "<ACTION type=\"WINDOW\" state=\"SHOW\" target=\"ATTR\" TOGGLE=\"0\">PANEL</ACTION>"
                            "<ACTION type=\"URL\" SOURCE=\"s\" FIELD=\"f\" EXTERNAL_BROWSER=\"0\"> addr </ACTION>"
                            "<ACTION type=\"FLY\" test=\"GE\">X</ACTION>"),
              doc, &notes),
        "parse");
  const auto &a = root(doc).actions;
  CHECK(a.size() == 3, "every row");
  CHECK(a[0].target == "PANEL" && a[0].toggle && noted(notes, "/ACTION@TARGET"), "the text; TARGET noted");
  CHECK(a[1].field == "s" && a[1].field_attr == "SOURCE" && a[1].external_browser && a[1].target == " addr ",
        "one slot, the first authored; EXTERNAL_BROWSER by presence; the text untrimmed");
  CHECK(noted(notes, "/ACTION@FIELD"), "the second of the slot is noted");
  CHECK(a[2].type == "FLY" && a[2].test == "GE", "an unknown TYPE is kept (code 0 at runtime)");
  CHECK(fixed_point(doc), "fixed point");
  return true;
}

// STRING (WRAP, the repeated merge), TOGGLE_STRING, PRIVATE_DATA, FONT and CURSOR.
bool test_text_elements() {
  mnu::Document doc;
  std::vector<mnu::ParseNote> notes;
  CHECK(parse(screen_of("type=\"button\" name=\"B\"",
                        std::string(kPos) +
                            "<STRING type=\"ID\" justify=\"RIGHT\" edge=\"3\">FIRST</STRING>"
                            "<STRING WRAP>SECOND</STRING>"
                            "<TOGGLE_STRING type=\"id\">ALT</TOGGLE_STRING><PRIVATE_DATA>p1</PRIVATE_DATA>"
                            "<FONT name=\"attr.fnt\"><NAME>a.fnt</NAME></FONT><FONT><DEFAULT_FG>FF0000</DEFAULT_FG></FONT>"
                            "<CURSOR file=\"x.tga\"><FILE>c.tga</FILE></CURSOR>"),
              doc, &notes),
        "parse");
  const mnu::Window &w = root(doc);
  CHECK(w.string_data.value == "SECOND" && w.string_data.type.empty(), "the last text, TYPE reset");
  CHECK(w.string_data.justify == "RIGHT" && w.string_data.edge == 3 && w.string_data.wrap,
        "JUSTIFY and EDGE carry over; WRAP");
  CHECK(w.toggle_string.present && w.toggle_string.type == "id" && w.toggle_string.value == "ALT", "TOGGLE_STRING");
  CHECK(w.private_data == "p1", "PRIVATE_DATA");
  CHECK(w.font.name == "a.fnt" && w.font.default_fg == "FF0000", "FONTs merge");
  CHECK(noted(notes, "/FONT@NAME") && noted(notes, "/CURSOR@FILE"), "attribute forms are noted");
  CHECK(w.cursor.file == "c.tga", "CURSOR FILE");
  CHECK(fixed_point(doc), "fixed point");
  return true;
}

// SPINUP / SPINDOWN / LIST_BOX / SCROLLBAR are whole windows of their owner's embedded
// widget; an empty one crashes retail and is left out; a repeated one merges.
bool test_parts() {
  mnu::Document doc;
  std::vector<mnu::ParseNote> notes;
  CHECK(parse(screen_of("type=\"spinlist\" name=\"SP\"",
                        std::string(kPos) +
                            "<SPINUP NAME=\"UP\" type=\"static\"><POSITION><LEFT>1</LEFT></POSITION>"
                            "<APPEARANCE type=\"image\" state=\"default\" map_state=\"0\" height=\"24\">r.tga</APPEARANCE>"
                            "<STRING>+</STRING><FONT><NAME>f.fnt</NAME></FONT><SOUND state=\"SELECTED\" "
                            "trigger=\"CLICK\">c.lwf</SOUND><HOTKEY>u</HOTKEY>"
                            "<WINDOW type=\"static\" name=\"IN\">" + kPos + "</WINDOW></SPINUP>"
                            "<SPINUP><TOGGLE_STRING>T</TOGGLE_STRING></SPINUP><SPINDOWN></SPINDOWN>"),
              doc, &notes),
        "parse");
  const mnu::Window &w = root(doc);
  CHECK(w.spinup && w.spinup->type == mnu::WindowType::Button && w.spinup->name == "UP", "a button part");
  CHECK(w.spinup->string_data.value == "+" && w.spinup->font.name == "f.fnt" && w.spinup->sounds.size() == 1 &&
            w.spinup->hotkeys.size() == 1 && w.spinup->children.size() == 1,
        "everything a button reads");
  CHECK(w.spinup->toggle_string.value == "T", "a repeated SPINUP merges");
  CHECK(!w.spindown && noted(notes, "/SPINDOWN"), "an empty SPINDOWN is left out");
  CHECK(noted(notes, "/SPINUP@TYPE"), "a part's TYPE is noted");
  CHECK(fixed_point(doc), "fixed point");

  CHECK(parse(screen_of("type=\"combobox\" name=\"C\"",
                        std::string(kPos) +
                            "<LIST_BOX sb_edge_pad=\"21\" HIDDEN><POSITION><TOP>20</TOP></POSITION>"
                            "<ITEMS MULTISELECT><ITEM PAIRS_LIST justify=\"LEFT\" value=\"2\">row</ITEM></ITEMS>"
                            "<MIN_ITEM_HEIGHT>18</MIN_ITEM_HEIGHT>"
                            "<SCROLLBAR><ORIENTATION>HORIZONTAL</ORIENTATION><WIDTH>12</WIDTH>"
                            "<SHUTTLE type=\"image\" state=\"default\">s.tga</SHUTTLE>"
                            "<SCROLLLEFT type=\"image\" state=\"default\">l.tga</SCROLLLEFT></SCROLLBAR></LIST_BOX>"),
              doc),
        "parse combo");
  const mnu::Window &c = root(doc);
  CHECK(c.has_sb_edge_pad && c.sb_edge_pad == 21, "sb_edge_pad is the combo's");
  CHECK(c.list_box && c.list_box->type == mnu::WindowType::List && c.list_box->hidden, "a list part");
  CHECK(c.list_box->items.multiselect && c.list_box->items.items[0].pairs_list &&
            c.list_box->items.items[0].justify == "LEFT",
        "ITEMS and ITEM attributes");
  CHECK(c.list_box->table_data.min_item_height == 18, "MIN_ITEM_HEIGHT on the list");
  const mnu::WindowPart &bar = c.list_box->scrollbar;
  CHECK(bar && bar->orientation == "HORIZONTAL" && bar->has_scroll_extent && bar->scroll_extent == 12 &&
            bar->scroll_extent_is_width,
        "a scroll part");
  CHECK(bar->shuttle.size() == 1 && bar->scrollup.size() == 1, "SCROLLLEFT reads as SCROLLUP");
  CHECK(fixed_point(doc), "fixed point");
  return true;
}

// [orig: CUIScrollWidget_ParseExtendedXMLDef @ 0x64c6d0] One HEIGHT/WIDTH slot (the
// last); a part row with no STATE ends a SCROLL's own parse.
bool test_scroll_rules() {
  mnu::Document doc;
  std::vector<mnu::ParseNote> notes;
  CHECK(parse(screen_of("type=\"scroll\" name=\"S\"",
                        std::string(kPos) + "<HEIGHT>9</HEIGHT><WIDTH>14</WIDTH>"
                        "<SHUTTLE type=\"image\" state=\"default\">a.tga</SHUTTLE>"
                        "<SCROLLUP type=\"image\">b.tga</SCROLLUP><ORIENTATION>HORIZONTAL</ORIENTATION>"),
              doc, &notes),
        "parse");
  const mnu::Window &w = root(doc);
  CHECK(w.has_scroll_extent && w.scroll_extent == 14 && w.scroll_extent_is_width, "the last of HEIGHT/WIDTH");
  CHECK(w.shuttle.size() == 1 && w.scrollup.empty() && w.orientation.empty(), "the stop");
  CHECK(noted(notes, "/SCROLLUP") && noted(notes, "/ORIENTATION") && noted(notes, "/HEIGHT"), "noted");
  return true;
}

// [orig: CListWnd_ParseXMLDefinition @ 0x645770; CTableWnd_ParseXMLContentDefinition
// @ 0x6427d0] An ITEMS APPEARANCE with no STATE ends a LIST's or a TABLE's own parse.
bool test_list_rules() {
  mnu::Document doc;
  std::vector<mnu::ParseNote> notes;
  CHECK(parse(screen_of("type=\"list\" name=\"L\"",
                        std::string(kPos) + "<ITEMS><ITEM>a</ITEM><APPEARANCE type=\"color\">FF</APPEARANCE>"
                        "<ITEM>b</ITEM></ITEMS><MIN_ITEM_HEIGHT>20</MIN_ITEM_HEIGHT>"),
              doc, &notes),
        "parse");
  CHECK(root(doc).items.items.size() == 1 && !root(doc).table_data.has_min_item_height, "the list parse ends");
  CHECK(noted(notes, "/ITEMS/APPEARANCE") && noted(notes, "/MIN_ITEM_HEIGHT"), "noted");
  // Any other type keeps the row as authored (it reads no ITEMS APPEARANCE).
  CHECK(parse(screen_of("type=\"spinlist\" name=\"L\"",
                        std::string(kPos) + "<ITEMS><APPEARANCE type=\"color\">FF</APPEARANCE><ITEM>b</ITEM></ITEMS>"),
              doc) &&
            root(doc).items.appearances.size() == 1 && root(doc).items.items.size() == 1,
        "a spin list keeps it");
  return true;
}

// TABLE: the running COLUMN index is written in; a HEADER past COUNT is noted; the
// draw kind keeps its first authored flag; ROW cells; IMAGEROW; a second COLUMN.
bool test_table_rules() {
  mnu::Document doc;
  std::vector<mnu::ParseNote> notes;
  CHECK(parse(screen_of("type=\"table\" name=\"T\"",
                        std::string(kPos) +
                            "<COLUMN count=\"2\" spacing=\"3\"><HEADER column=\"1\" width=\"50\" sort=\"A\" PRIMARY_SORT "
                            "type=\"id\">One</HEADER><BODY CUSTOM_DRAW BITMAP_DRAW BITMAP_FLAGS=\"STANDARD\"></BODY>"
                            "<SUBST value=\"1\" URL>http://x</SUBST><HEADER column=\"2\">Past</HEADER></COLUMN>"
                            "<COLUMN count=\"1\"></COLUMN>"
                            "<ITEMS MULTISELECT><APPEARANCE type=\"IMAGEROW\" state=\"selected\">row.tga</APPEARANCE>"
                            "<ROW><ITEM column=\"1\" type=\"BITMAP\" value=\"4\">cell.tga</ITEM></ROW></ITEMS>"
                            "<FIXED_HEADER_HEIGHT>22</FIXED_HEADER_HEIGHT><MIN_ITEM_HEIGHT>0</MIN_ITEM_HEIGHT>"),
              doc, &notes),
        "parse");
  const mnu::Window &w = root(doc);
  const mnu::TableColumn &c = w.table_data.column;
  CHECK(c.count == 2 && c.spacing == 3 && c.headers.size() == 2, "COLUMN");
  CHECK(c.headers[0].width == 50 && c.headers[0].sort == "A", "HEADER");
  // PRIMARY_SORT authored after the HEADER's COLUMN: the walk (last authored first)
  // meets it while the running index is still 0 [orig: @ 0x643240 / 0x64325f].
  CHECK(c.primary_sort == 0 && c.primary_sort_token == "PRIMARY_SORT" && c.secondary_sort == -1,
        "a sort key takes the index as the walk stands");
  CHECK(c.bodies[0].has_column && c.bodies[0].column == 1, "BODY takes the running index");
  CHECK(c.bodies[0].display == "CUSTOM_DRAW" && c.bodies[0].bitmap_draw, "the first authored draw kind");
  CHECK(c.substitutions[0].column == 1 && c.substitutions[0].is_url, "SUBST URL");
  CHECK(noted(notes, "/COLUMN/HEADER") && noted(notes, "/COLUMN"), "HEADER past COUNT, the second COLUMN");
  CHECK(w.items.rows.size() == 1 && w.items.rows[0].cells[0].column == 1 && w.items.rows[0].cells[0].type == "BITMAP",
        "ROW cells");
  CHECK(w.items.appearances[0].type == "IMAGEROW", "IMAGEROW");
  CHECK(w.table_data.has_fixed_header_height && w.table_data.fixed_header_height == 22, "FIXED_HEADER_HEIGHT");
  // A table MIN_ITEM_HEIGHT of 0 divides by zero in retail: the writer refuses it.
  const auto issues = mnu::write_issues(doc);
  CHECK(!issues.empty() && issues[0].field == "min_item_height" && issues[0].locator == "0/window:0",
        "MIN_ITEM_HEIGHT 0 is a write issue");
  mnu::Document ok = doc;
  ok.screens[0].roots[0].table_data.min_item_height = 20;
  ok.screens[0].roots[0].table_data.column.headers.pop_back(); // the one past COUNT reads with its note
  CHECK(fixed_point(ok), "fixed point");
  CHECK(mnu::serialize(ok).find("column=\"1\" PRIMARY_SORT") != std::string::npos,
        "the key is written after COLUMN, where it reads the index before it");
  return true;
}

// [orig: CTableWnd_ParseXMLContentDefinition @ 0x6427d0] A key authored before the
// HEADER's COLUMN takes that HEADER's index; the last write wins; WIDTH (100 at the
// COLUMN element) and SORT carry from HEADER to HEADER; no COUNT leaves one column.
bool test_table_header_walk() {
  mnu::Document doc;
  std::vector<mnu::ParseNote> notes;
  CHECK(parse(screen_of("type=\"table\" name=\"T\"",
                        std::string(kPos) +
                            "<COLUMN count=\"0\" count=\"4\"><HEADER>A</HEADER>"
                            "<HEADER SECONDARY_SORT column=\"1\" width=\"60\" sort=\"1\">B</HEADER>"
                            "<HEADER column=\"2\" DEFAULT_SORT>C</HEADER>"
                            "<HEADER TERTIARY_SORT column=\"3\" sort=\"x\">D</HEADER>"
                            "<HEADER SECONDARY_SORT column=\"3\" sort=\"a\" width=\"0\">E</HEADER></COLUMN>"),
              doc, &notes),
        "parse");
  const mnu::TableColumn &c = root(doc).table_data.column;
  CHECK(c.count == 4 && noted(notes, "/COLUMN@COUNT"), "the first COUNT of 1 or more; the other noted");
  CHECK(c.primary_sort == 1 && c.primary_sort_token == "DEFAULT_SORT", "after COLUMN: the index before it");
  CHECK(c.secondary_sort == 3 && c.tertiary_sort == 3, "before COLUMN: the HEADER's own; the last write wins");
  CHECK(noted(notes, "/HEADER@SECONDARY_SORT"), "the replaced key is noted");
  const std::vector<mnu::TableHeaderSetup> setup = mnu::table_header_setup(c);
  CHECK(setup.size() == 5 && setup[0].column == 0 && setup[0].width == 100 && !setup[0].numeric_sort,
        "100 and the text compare at the COLUMN element");
  CHECK(setup[1].column == 1 && setup[1].width == 60 && setup[1].numeric_sort, "authored");
  CHECK(setup[2].width == 60 && setup[2].numeric_sort && setup[3].numeric_sort, "carried (an unknown SORT keeps it)");
  CHECK(setup[4].width == 0 && !setup[4].numeric_sort, "'a' is the text compare; WIDTH 0 is authored");
  CHECK(fixed_point(doc), "fixed point");
  mnu::Document written;
  std::vector<mnu::ParseNote> again;
  CHECK(parse(mnu::serialize(doc), written, &again), "reparse");
  const mnu::TableColumn &w = root(written).table_data.column;
  CHECK(w.primary_sort == 1 && w.secondary_sort == 3 && w.tertiary_sort == 3, "the keys read back");
  CHECK(mnu::table_header_setup(w)[2].width == 60, "the carried width reads back");
  // No COUNT: the table keeps its one column, so index 1 is not set up.
  CHECK(parse(screen_of("type=\"table\" name=\"T\"",
                        std::string(kPos) + "<COLUMN><HEADER column=\"0\">A</HEADER><HEADER column=\"1\">B</HEADER>"
                                            "</COLUMN>"),
              doc, &notes),
        "parse no COUNT");
  const std::vector<mnu::TableHeaderSetup> one = mnu::table_header_setup(root(doc).table_data.column);
  CHECK(one[0].set_up && !one[1].set_up && noted(notes, "/HEADER"), "one column without a COUNT");
  // A key no HEADER's index reaches cannot be written.
  mnu::Document bad = doc;
  bad.screens[0].roots[0].table_data.column.primary_sort = 7;
  CHECK(!mnu::write_issues(bad).empty(), "an unplaceable sort key");
  return true;
}

// An ORIENTATION only ever sets horizontal; every DATASOURCE loads; an empty TEXT_RSRC
// is a table.
bool test_repeated_type_elements() {
  mnu::Document doc;
  std::vector<mnu::ParseNote> notes;
  CHECK(parse(screen_of("type=\"scroll\" name=\"S\"",
                        std::string(kPos) + "<ORIENTATION>VERTICAL</ORIENTATION><ORIENTATION>horizontal"
                                            "</ORIENTATION><ORIENTATION>VERTICAL</ORIENTATION>"),
              doc, &notes),
        "parse");
  CHECK(root(doc).orientation == "horizontal" && noted(notes, "/ORIENTATION"), "any HORIZONTAL decides");
  CHECK(parse(screen_of("type=\"marquee_wnd\" name=\"M\"",
                        std::string(kPos) + "<DATASOURCE>a.kda</DATASOURCE><DATASOURCE>b.kda</DATASOURCE>"
                                            "<TEXT_RSRC></TEXT_RSRC>"),
              doc, &notes),
        "parse marquee");
  const mnu::Window &m = root(doc);
  CHECK(m.datasources == std::vector<std::string>({"a.kda", "b.kda"}) && notes.empty(), "every DATASOURCE");
  CHECK(m.has_text_rsrc && m.text_rsrc.empty(), "an empty TEXT_RSRC is kept");
  CHECK(mnu::serialize(doc).find("<TEXT_RSRC></TEXT_RSRC>") != std::string::npos, "and written");
  CHECK(fixed_point(doc), "fixed point");
  return true;
}

// What retail crashes or hangs on is a fatal note, where retail reads it.
bool test_fatal_notes() {
  auto fatal = [](const std::string &xml, const std::string &suffix) {
    mnu::Document doc;
    std::vector<mnu::ParseNote> notes;
    std::string error;
    if (!mnu::parse(xml, doc, error, &notes)) return -1;
    for (const mnu::ParseNote &n : notes)
      if (n.key.size() >= suffix.size() && n.key.compare(n.key.size() - suffix.size(), suffix.size(), suffix) == 0)
        return n.fatal ? 1 : 0;
    return -2;
  };
  CHECK(fatal(screen_of("type=\"button\" name=\"B\"", std::string(kPos) + "<ACTION type=\"\">X</ACTION>"),
              "/ACTION@TYPE") == 1,
        "an empty ACTION TYPE");
  CHECK(fatal(screen_of("type=\"button\" name=\"B\"",
                        std::string(kPos) + "<ACTION type=\"URL\" FIELD=\"f\" SOURCE=\"\">X</ACTION>"),
              "/ACTION@SOURCE") == 1,
        "an empty slot attribute retail walks past");
  CHECK(fatal(screen_of("type=\"spinlist\" name=\"S\"", std::string(kPos) + "<SPINDOWN></SPINDOWN>"), "/SPINDOWN") ==
            1,
        "an empty part its owner reads");
  CHECK(fatal(screen_of("type=\"static\" name=\"S\"", std::string(kPos) + "<SPINDOWN></SPINDOWN>"), "/SPINDOWN") ==
            0,
        "an empty part its owner does not read");
  CHECK(fatal(screen_of("type=\"edit\" name=\"E\" MAXCHAR=\"\"", kPos), "@MAXCHAR") == 1, "MAXCHAR on an edit");
  CHECK(fatal(screen_of("type=\"combobox\" name=\"C\" MAXCHAR=\"\"", kPos), "@MAXCHAR") == 0,
        "MAXCHAR on a combo (not read)");
  CHECK(fatal(screen_of("type=\"window\" name=\"W\"", std::string(kPos) + "<STRING justify=\"\">x</STRING>"),
              "/STRING@JUSTIFY") == 0,
        "STRING on a generic window (not read)");
  CHECK(fatal("<SCREEN><NAME>S</NAME><WINDOW type=\"\" name=\"W\">" + std::string(kPos) + "</WINDOW></SCREEN>",
              "WINDOW[W]") == 1,
        "a WINDOW with an empty TYPE");
  CHECK(fatal("<SCREEN><NAME>S</NAME><MUSICVAR></MUSICVAR></SCREEN>", "/MUSICVAR") == 1, "an empty MUSICVAR");
  CHECK(fatal(screen_of("type=\"static\" name=\"W\"", std::string(kPos) + "<GROUP>1</GROUP>"), "/GROUP") == -2,
        "a note-free menu");
  // A number holding a stylesheet variable: retail reads the variable's value, the model
  // only the number wcstol reads here, so a save would lose it (D-MNU-1).
  CHECK(fatal(screen_of("type=\"static\" name=\"W\"", "<POSITION><LEFT>%X%</LEFT></POSITION>"),
              "/POSITION/LEFT") == 1,
        "a POSITION edge holding a variable");
  CHECK(fatal(screen_of("type=\"static\" name=\"W\"", "<POSITION><LEFT>1%X% </LEFT></POSITION>"),
              "/POSITION/LEFT") == 1,
        "a variable inside a POSITION edge");
  CHECK(fatal(screen_of("type=\"edit\" name=\"E\" MAXCHAR=\"%X%\"", kPos), "@MAXCHAR") == 1,
        "an attribute number holding a variable");
  CHECK(fatal(screen_of("type=\"combobox\" name=\"C\" MAXCHAR=\"%X%\"", kPos), "@MAXCHAR") == 0,
        "a variable in a number retail does not read there");
  CHECK(fatal("<SCREEN><NAME>S</NAME><MUSICVAR>%X%</MUSICVAR></SCREEN>", "/MUSICVAR") == 1,
        "a MUSICVAR holding a variable");
  CHECK(fatal(screen_of("type=\"static\" name=\"W\"", "<POSITION><LEFT>50%</LEFT><TOP>%A B%</TOP></POSITION>"),
              "/POSITION/LEFT") == -2,
        "a '%' that opens no name is no variable");
  // Under a merged or replaced element's own note, a fatal note still comes out: retail
  // parses a repeated POSITION or STRING into the same fields.
  CHECK(fatal(screen_of("type=\"static\" name=\"W\"",
                        "<POSITION><TOP>7</TOP></POSITION><POSITION><LEFT>%X%</LEFT></POSITION>"),
              "/POSITION/LEFT") == 1,
        "a variable edge in a repeated POSITION");
  CHECK(fatal(screen_of("type=\"static\" name=\"W\"", std::string(kPos) +
                                                          "<STRING>a</STRING><STRING JUSTIFY=\"\">b</STRING>"),
              "/STRING@JUSTIFY") == 1,
        "an empty value in a repeated STRING");
  {
    // What retail ignores inside a merged element stays under its note.
    mnu::Document merged;
    std::vector<mnu::ParseNote> merged_notes;
    CHECK(parse(screen_of("type=\"static\" name=\"W\"",
                          "<POSITION><LEFT>0</LEFT></POSITION><POSITION FOO=\"1\"><LEFT>1</LEFT></POSITION>"),
                merged, &merged_notes) &&
              merged_notes.size() == 1 && !merged_notes[0].fatal,
          "one note for a merged element");
  }
  // A replaced element's value is overwritten by the later one: its variable is lost to nothing.
  CHECK(fatal(screen_of("type=\"radio\" name=\"R\"", std::string(kPos) + "<GROUP>%X%</GROUP><GROUP>2</GROUP>"),
              "/GROUP") == 0,
        "a variable in a replaced GROUP");
  // The HEADER walk (last authored first) converts every COLUMN, and a sort key keeps the
  // index as it stands: a repeated COLUMN it took is read, one it did not take is not.
  const std::string table = "type=\"table\" name=\"T\"";
  CHECK(fatal(screen_of(table, std::string(kPos) +
                                   "<COLUMN COUNT=\"2\"><HEADER COLUMN=\"0\" PRIMARY_SORT COLUMN=\"%X%\">H</HEADER>"
                                   "</COLUMN>"),
              "/HEADER@COLUMN") == 1,
        "a repeated COLUMN a sort key took");
  CHECK(fatal(screen_of(table, std::string(kPos) +
                                   "<COLUMN COUNT=\"2\"><HEADER PRIMARY_SORT COLUMN=\"0\" COLUMN=\"%X%\">H</HEADER>"
                                   "</COLUMN>"),
              "/HEADER@COLUMN") == 0,
        "a repeated COLUMN no sort key took");
  // A number field only some types read blocks only there (docs/mnu/menu-re.md, the
  // per-type table and the ITEMS forms).
  CHECK(fatal(screen_of("type=\"static\" name=\"W\"", std::string(kPos) + "<MIN_ITEM_HEIGHT>%X%</MIN_ITEM_HEIGHT>"),
              "/MIN_ITEM_HEIGHT") == 0,
        "MIN_ITEM_HEIGHT on a STATIC");
  CHECK(fatal(screen_of("type=\"list\" name=\"L\"", std::string(kPos) + "<MIN_ITEM_HEIGHT>%X%</MIN_ITEM_HEIGHT>"),
              "/MIN_ITEM_HEIGHT") == 1,
        "MIN_ITEM_HEIGHT on a LIST");
  CHECK(fatal(screen_of("type=\"list\" name=\"L\"",
                        std::string(kPos) + "<FIXED_HEADER_HEIGHT>%X%</FIXED_HEADER_HEIGHT>"),
              "/FIXED_HEADER_HEIGHT") == 0,
        "FIXED_HEADER_HEIGHT on a LIST");
  CHECK(fatal(screen_of(table, std::string(kPos) + "<FIXED_HEADER_HEIGHT>%X%</FIXED_HEADER_HEIGHT>"),
              "/FIXED_HEADER_HEIGHT") == 1,
        "FIXED_HEADER_HEIGHT on a TABLE");
  CHECK(fatal(screen_of("type=\"button\" name=\"B\"", std::string(kPos) + "<GROUP>%X%</GROUP>"), "/GROUP") == 0,
        "GROUP on a BUTTON");
  CHECK(fatal(screen_of("type=\"radioedit\" name=\"E\"", std::string(kPos) + "<GROUP>%X%</GROUP>"), "/GROUP") == 1,
        "GROUP on a RADIOEDIT");
  CHECK(fatal(screen_of("type=\"list\" name=\"L\"", std::string(kPos) + "<ITEMS><ITEM COLUMN=\"%X%\">a</ITEM></ITEMS>"),
              "/ITEM@COLUMN") == 0,
        "a LIST ITEM's COLUMN");
  CHECK(fatal(screen_of(table, std::string(kPos) + "<ITEMS><ROW><ITEM COLUMN=\"%X%\">a</ITEM></ROW></ITEMS>"),
              "/ITEM@COLUMN") == 1,
        "a TABLE cell's COLUMN");
  {
    mnu::Document held;
    std::vector<mnu::ParseNote> held_notes;
    CHECK(parse(screen_of("type=\"static\" name=\"W\"", "<POSITION><LEFT>%X%</LEFT><TOP>7</TOP></POSITION>"), held,
                &held_notes),
          "parse");
    CHECK(root(held).position.has_left && root(held).position.left == 0 && root(held).position.top == 7,
          "the model keeps the number wcstol reads here");
  }
  mnu::Document doc;
  std::vector<mnu::ParseNote> notes;
  std::string error;
  CHECK(mnu::parse("<SCREEN><NAME>S</NAME><WINDOW type=\"static\"", doc, error, &notes) && !notes.empty() &&
            notes[0].fatal,
        "the end of the text inside a tag (retail hangs)");
  CHECK(mnu::parse(screen_of("type=\"static\" name=\"W\" SCREENX=\"1\"", kPos), doc, error, &notes) &&
            notes.size() == 1 && !notes[0].fatal,
        "an attribute retail ignores is not fatal");
  return true;
}

// What only the GLB_TABLE, GOPHER and LAN_LIST parses read is kept as read.
bool test_extras() {
  mnu::Document doc;
  std::vector<mnu::ParseNote> notes;
  CHECK(parse(screen_of("type=\"glb_table\" name=\"G\" PLAYERLIST",
                        std::string(kPos) +
                            "<GLB_TABLE><COLUMN count=\"2\"><HEADER column=\"1\" FIELD=\"ping\">P</HEADER></COLUMN>"
                            "</GLB_TABLE><TARGET>http://x</TARGET><JOIN_BUTTON name=\"J\"><POSITION><LEFT>1</LEFT>"
                            "</POSITION></JOIN_BUTTON><BOGUS></BOGUS>"),
              doc, &notes),
        "parse");
  const mnu::Window &w = root(doc);
  CHECK(w.extra_attributes.size() == 1 && w.extra_attributes[0].name == "PLAYERLIST", "PLAYERLIST");
  CHECK(w.extras.size() == 3 && w.extras[0].tag == "GLB_TABLE" && w.extras[1].text == "http://x", "extras");
  CHECK(w.extras[0].children[0].children[0].attributes[1].value == "ping", "the subtree as read");
  CHECK(noted(notes, "/BOGUS") && notes.size() == 1, "only the unknown element is noted");
  CHECK(fixed_point(doc), "fixed point");
  return true;
}

// --- the writer -------------------------------------------------------------------

bool test_writer_forms() {
  mnu::Document doc;
  mnu::Screen screen;
  screen.name = "S";
  mnu::Window w;
  w.type = mnu::WindowType::SpinList;
  w.name = "A&B<C>";
  w.items.present = true;
  mnu::Item empty;
  empty.value = "33";
  w.items.items.push_back(empty);
  w.string_data.present = true;
  w.string_data.value = "x < y & z > w \"q\"";
  screen.roots.push_back(w);
  mnu::Window second;
  second.name = "SECOND";
  second.position.has_left = true;
  screen.roots.push_back(second);
  doc.screens.push_back(screen);
  const std::string out = mnu::serialize(doc);
  CHECK(out.find("/>") == std::string::npos, "never a self-closing tag");
  CHECK(out.find("<ITEM value=\"33\"></ITEM>") != std::string::npos, "an empty ITEM keeps its close tag");
  CHECK(out.find("name=\"A&B<C>\"") != std::string::npos, "attribute values raw");
  CHECK(out.find(">x &lt; y &amp; z > w \"q\"</STRING>") != std::string::npos, "text escapes '<' and '&' only");
  CHECK(out.find("<?xml") == std::string::npos, "no declaration");
  mnu::Document back;
  CHECK(parse(out, back), "parse");
  CHECK(back.screens[0].roots.size() == 2 && back.screens[0].roots[0].name == "A&B<C>" &&
            back.screens[0].roots[0].string_data.value == w.string_data.value,
        "every root, the values read back");
  CHECK(fixed_point(doc), "fixed point");
  return true;
}

// A value the file cannot hold, or one retail faults on, is a WriteIssue and the
// serialization refuses.
bool test_write_issues() {
  auto issue_for = [](mnu::Document doc) {
    std::vector<uint8_t> bytes;
    std::string error;
    const auto issues = mnu::write_issues(doc);
    const bool refused = !mnu::serialize_bytes(doc, bytes, error);
    return issues.empty() || !refused ? std::string() : issues[0].message;
  };
  mnu::Document base;
  CHECK(parse(screen_of("type=\"static\" name=\"W\"", kPos), base), "parse");
  CHECK(issue_for(base).empty(), "a plain menu writes");
  mnu::Document d = base;
  d.screens[0].roots[0].name = "a\"b";
  CHECK(issue_for(d).find("quote") != std::string::npos, "a '\"' in an attribute");
  d = base;
  d.screens[0].roots[0].string_data.present = true;
  d.screens[0].roots[0].string_data.justify = "it's";
  CHECK(issue_for(d).find("quote") != std::string::npos, "a '\\'' in an attribute");
  d = base;
  mnu::Action screen_action;
  screen_action.type = "SCREEN";
  screen_action.target = "NEXT";
  d.screens[0].roots[0].actions.push_back(screen_action);
  CHECK(issue_for(d).find("FILE") != std::string::npos, "a SCREEN action with no FILE");
  d = base;
  d.screens[0].roots[0].type = mnu::WindowType::Edit;
  d.screens[0].roots[0].type_token.clear();
  d.screens[0].roots[0].global_var = true;
  CHECK(issue_for(d).find("GLOBAL_VAR") != std::string::npos, "GLOBAL_VAR on an edit");
  d = base;
  d.screens[0].roots[0].type = mnu::WindowType::CheckBox;
  d.screens[0].roots[0].global_var = true;
  CHECK(issue_for(d).empty(), "GLOBAL_VAR on an unchecked checkbox writes");
  d.screens[0].roots[0].checked = true;
  CHECK(!issue_for(d).empty(), "GLOBAL_VAR on a checked checkbox");
  d = base;
  d.screens[0].name.clear();
  CHECK(issue_for(d).find("NAME") != std::string::npos, "a SCREEN with no NAME");
  d = base;
  d.screens[0].roots[0].position = mnu::Position{};
  CHECK(!issue_for(d).empty(), "a WINDOW with no child element");
  d = base;
  d.screens[0].roots[0].spinup.author(mnu::WindowType::Button);
  CHECK(issue_for(d).find("SPINUP") != std::string::npos, "an empty part");
  d = base;
  mnu::Appearance stateless;
  stateless.type = "image";
  stateless.value = "a.tga";
  d.screens[0].roots[0].appearances.push_back(stateless);
  CHECK(issue_for(d).find("STATE") != std::string::npos, "an APPEARANCE with no STATE");
  d = base;
  mnu::Sound silent;
  silent.state = "MOUSEIN";
  silent.file = "a.lwf";
  d.screens[0].roots[0].sounds.push_back(silent);
  CHECK(issue_for(d).find("TRIGGER") != std::string::npos, "a SOUND with no TRIGGER");
  // Each issue names the record (the property table's list paths) and the field.
  d = base;
  d.screens[0].roots[0].children.push_back(base.screens[0].roots[0]);
  d.screens[0].roots[0].children[0].actions.push_back(screen_action);
  d.screens[0].roots[0].children[0].actions.push_back(screen_action);
  d.screens[0].roots[0].children[0].actions[1].file.clear();
  d.screens[0].roots[0].children[0].actions[0].file = "a.mnu";
  const auto located = mnu::write_issues(d);
  CHECK(located.size() == 1 && located[0].locator == "0/window:0/window:0/action:1" && located[0].field == "file",
        "the action and its field");
  d = base;
  d.screens[0].roots[0].spinup.author(mnu::WindowType::Button);
  CHECK(mnu::write_issues(d)[0].locator == "0/window:0/spinup:0" && mnu::write_issues(d)[0].field.empty(),
        "the part itself");
  d.screens[0].roots[0].spinup.hide();
  CHECK(mnu::write_issues(d).empty(), "a part left out writes nothing, so it holds no issue");
  return true;
}

// Every modeled element and attribute, built in code (ADR 0003), writes and reads
// back to the same bytes with nothing left out.
bool test_every_element_fixed_point() {
  mnu::Appearance image;
  image.state = "default";
  image.type = "image";
  image.value = "a.tga";
  image.has_map_state = true;
  image.map_state = 0;
  image.has_height = true;
  image.height = 24;
  image.flags = "STANDARD_TRANSPARENT";
  mnu::Window w;
  w.name = "ALL";
  w.type = mnu::WindowType::Table;
  w.hidden = w.disabled = w.checked = w.draw_frame = w.modal = w.readonly = w.as_button = true;
  w.number = w.global_var = w.password = true;
  w.has_group = true;
  w.group = 0;
  w.has_minval = w.has_maxval = w.has_maxchar = w.has_form = true;
  w.minval = -1;
  w.maxval = 99;
  w.maxchar = 2;
  w.form = 3;
  w.orientation = "HORIZONTAL";
  w.has_scroll_extent = true;
  w.scroll_extent = 20;
  w.position = {1, 2, 3, 4, true, true, true, true};
  w.appearances = {image};
  w.sounds = {mnu::Sound{"MOUSEIN", "MOUSE_OVER", "menu.lwf"}};
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
  action.target = "PANEL";
  w.actions = {action};
  w.string_data = {true, "ID", "CENTER", "BOTTOM", true, 0, true, "KEY"};
  w.toggle_string = {true, "ID", "ALT"};
  w.font = {"f.fnt", "1", "2", "3", "4", "5", "6", "7", "8"};
  w.frame.stencil = "s.tga";
  w.frame.has_stencil_size = w.frame.has_insetx = w.frame.has_insety = true;
  w.frame.brush = "b.tga";
  w.frame.monogram = "m.tga";
  w.items.present = w.items.multiselect = true;
  w.items.justify = "LEFT";
  w.items.vjustify = "TOP";
  w.items.appearances = {image};
  mnu::Item item;
  item.type = "ID";
  item.value = "1";
  item.text = "T";
  item.justify = "RIGHT";
  item.vjustify = "CENTER";
  item.pairs_list = true;
  item.has_column = true;
  item.column = 0;
  w.items.items = {item};
  w.items.rows = {mnu::TableRow{{item}}};
  mnu::Window &list = w.list_box.author(mnu::WindowType::List);
  list.position.has_top = true;
  list.scrollbar.author(mnu::WindowType::Scroll).shuttle = {image};
  w.has_sb_edge_pad = true;
  w.sb_edge_pad = 0;
  w.spinup.author(mnu::WindowType::Button).appearances = {image};
  w.spindown.author(mnu::WindowType::Button).string_data = {true, "", "", "", false, 0, false, "-"};
  w.scrollbar.author(mnu::WindowType::Scroll).scrolldown = {image};
  w.cursor = {"c.tga", "STANDARD"};
  w.has_text_rsrc = true;
  w.text_rsrc = "menutxt.BIN";
  w.private_data = "p";
  w.datasources = {"nlist.kda", "credits.ini"};
  w.hotkeys = {mnu::Hotkey{"VK_ESCAPE", true}, mnu::Hotkey{"=", false}};
  w.shuttle = w.scrollup = w.scrolldown = {image};
  mnu::TableHeader header;
  header.has_column = header.has_width = true;
  header.width = 100;
  header.sort = "A";
  header.type = "id";
  header.text = "H";
  mnu::TableBody body;
  body.has_column = true;
  body.bitmap_draw = body.custom_draw = body.bitmap_text = body.scale_bitmap = true;
  body.display = "BITMAP_TEXT";
  body.bitmap_flags = "STANDARD";
  mnu::TableSubst subst;
  subst.has_column = true;
  subst.value = "1";
  subst.is_file = subst.is_url = true;
  subst.file = "x.tga";
  w.table_data.column = {true, 1, true, 0, {header}, {body}, {subst}};
  w.table_data.column.primary_sort = 0;
  w.table_data.column.primary_sort_token = "DEFAULT_SORT";
  w.table_data.column.secondary_sort = w.table_data.column.tertiary_sort = 0;
  w.table_data.has_min_item_height = true;
  w.table_data.min_item_height = 20;
  w.table_data.has_fixed_header_height = true;
  w.table_data.fixed_header_height = 0;
  mnu::Element target;
  target.tag = "TARGET";
  target.text = "http://x";
  mnu::Element join;
  join.tag = "JOIN_BUTTON";
  join.attributes = {{"name", "J", true}, {"BARE", "", false}};
  join.text = "note";
  join.children = {target};
  w.extras = {join, target};
  w.extra_attributes = {{"SERVERLIST", "", false}};
  mnu::Window child;
  child.name = "CHILD";
  child.type = mnu::WindowType::Static;
  child.position.has_left = true;
  w.children = {child};

  mnu::Document doc;
  mnu::Screen screen;
  screen.name = "EVERY";
  screen.has_music_var = true;
  screen.music_var = 0;
  screen.roots = {w, child};
  doc.screens = {screen};
  CHECK(mnu::write_issues(doc).empty(), "no write issue");
  CHECK(fixed_point(doc), "every element writes and reads back");
  mnu::Document back;
  CHECK(parse(mnu::serialize(doc), back), "parse");
  const mnu::Window &r = back.screens[0].roots[0];
  CHECK(r.table_data.column.bodies[0].display == "BITMAP_TEXT", "the draw kind written first");
  CHECK(r.actions[0].field_attr == "NAME" && r.actions[0].field == "slot", "the slot's spelling");
  CHECK(r.extras[0].text == "note" && r.extras[0].children.size() == 1, "text beside children, compact");
  CHECK(r.spindown && r.spindown->string_data.value == "-" && r.list_box->scrollbar, "parts");
  CHECK(back.screens[0].roots.size() == 2, "both roots");
  return true;
}

// --- helpers and encodings ---------------------------------------------------------

bool test_helpers() {
  // [orig: CUIElement_ParseXMLDefinition @ 0x648562 — COLOR / OUTLINE are
  // wcstoul(text, 16)]: six digits leave alpha 0, eight are the word.
  CHECK(mnu::color_value("FF0000") == 0x00FF0000u, "RRGGBB: alpha 0");
  CHECK(mnu::color_value("80FFFFFF") == 0x80FFFFFFu, "AARRGGBB");
  CHECK(mnu::color_value("%VAR%") == 0u, "an unresolved variable reads 0");
  std::string hotkey;
  int pos = -1;
  CHECK(mnu::strip_hotkey_marker("A{HOT}b{hot}c", &hotkey, &pos) == "A{HOT}bc" && hotkey == "c" && pos == 7,
        "the case-sensitive first marker");
  CHECK(mnu::strip_hotkey_marker("Tail{hot}", &hotkey, &pos) == "Tail" && hotkey.empty() && pos == 4, "trailing");
  return true;
}

std::vector<uint8_t> utf16le(const std::u16string &text) {
  std::vector<uint8_t> out{0xFF, 0xFE};
  for (char16_t u : text) {
    out.push_back(static_cast<uint8_t>(u & 0xFF));
    out.push_back(static_cast<uint8_t>(u >> 8));
  }
  return out;
}

// [orig: XML_ParseWithBOMDetection @ 0x76a690] The code page, UTF-8 with its mark and
// UTF-16 LE are kept; a big-endian file is read as little-endian and loads nothing.
bool test_source_encoding() {
  const std::string src = "<SCREEN><NAME>S</NAME><WINDOW type=\"window\" name=\"R\"><STRING>caf\xE9</STRING>"
                          "</WINDOW></SCREEN>";
  mnu::Document doc;
  std::string error;
  CHECK(mnu::parse(src, doc, error) && doc.source_encoding == mnu::SourceEncoding::CodePage, "code page");
  CHECK(root(doc).string_data.value == "caf\xE9", "the code page's bytes");
  std::vector<uint8_t> saved;
  CHECK(mnu::serialize_bytes(doc, saved, error) && std::string(saved.begin(), saved.end()).find("caf\xE9") !=
                                                        std::string::npos,
        "written back as bytes");
  const std::string bom = "\xEF\xBB\xBF<SCREEN><NAME>S</NAME><WINDOW type=\"window\" name=\"R\"><STRING>caf\xC3\xA9"
                          "</STRING></WINDOW></SCREEN>";
  CHECK(mnu::parse(bom, doc, error) && doc.source_encoding == mnu::SourceEncoding::Utf8Bom &&
            root(doc).string_data.value == "caf\xC3\xA9",
        "UTF-8 with its mark");
  const std::vector<uint8_t> wide =
      utf16le(u"<SCREEN><NAME>S</NAME><WINDOW type=\"window\" name=\"R\"><STRING>caf" + std::u16string(1, char16_t(0xE9)) +
              u"</STRING></WINDOW></SCREEN>");
  CHECK(mnu::parse(wide.data(), wide.size(), doc, error) && doc.source_encoding == mnu::SourceEncoding::Utf16LE &&
            root(doc).string_data.value == "caf\xC3\xA9",
        "UTF-16 LE");
  CHECK(mnu::serialize_bytes(doc, saved, error) && saved.size() > 2 && saved[0] == 0xFF && saved[1] == 0xFE,
        "written back as UTF-16 LE");
  mnu::Document again;
  CHECK(mnu::parse(saved.data(), saved.size(), again, error) && root(again).string_data.value == "caf\xC3\xA9",
        "and read back");
  std::vector<uint8_t> big{0xFE, 0xFF};
  for (size_t i = 2; i < wide.size(); i += 2) {
    big.push_back(wide[i + 1]);
    big.push_back(wide[i]);
  }
  CHECK(!mnu::parse(big.data(), big.size(), doc, error), "UTF-16 BE loads nothing");
  return true;
}

}  // namespace

int main() {
  int failed = 0;
#define RUN_TEST(name)                     \
  do {                                     \
    std::cout << "Running " #name "... "; \
    if (name()) {                          \
      std::cout << "OK\n";                 \
    } else {                               \
      std::cout << "FAILED\n";             \
      ++failed;                            \
    }                                      \
  } while (0)

  RUN_TEST(test_screen_reads_name_musicvar_windows);
  RUN_TEST(test_window_creation);
  RUN_TEST(test_window_attributes);
  RUN_TEST(test_base_parse_stops);
  RUN_TEST(test_text_rules);
  RUN_TEST(test_position_rules);
  RUN_TEST(test_action_rules);
  RUN_TEST(test_text_elements);
  RUN_TEST(test_parts);
  RUN_TEST(test_scroll_rules);
  RUN_TEST(test_list_rules);
  RUN_TEST(test_table_rules);
  RUN_TEST(test_table_header_walk);
  RUN_TEST(test_repeated_type_elements);
  RUN_TEST(test_fatal_notes);
  RUN_TEST(test_extras);
  RUN_TEST(test_writer_forms);
  RUN_TEST(test_write_issues);
  RUN_TEST(test_every_element_fixed_point);
  RUN_TEST(test_helpers);
  RUN_TEST(test_source_encoding);

  if (failed > 0) {
    std::cerr << "\n" << failed << " test(s) FAILED\n";
    return EXIT_FAILURE;
  }
  std::cout << "\nAll tests passed!\n";
  return EXIT_SUCCESS;
}
