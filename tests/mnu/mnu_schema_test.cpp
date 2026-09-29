// The mnu property table (formats/mnu/mnu_schema, ADR 0046 A16 and S9h): every member
// the reader fills is reached by exactly one field path or list, so a menu rebuilt from
// an empty screen through the table alone writes the same bytes as the parsed one and
// setting one field changes no other; a Set of a field's own value changes nothing; a
// Clear leaves the element out; every list's default record writes and reads back; the
// per-type applicability and the references a sibling decides; the two texts written as
// attribute names take only the reader's tokens. Over a synthetic menu
// holding every element, and (a SKIP-LEG without OPENNOVA_JO_ASSETS) the fifteen shipped
// revx02 menus of the reference fixture set.
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <formats/mnu/mnu.h>
#include <formats/mnu/mnu_schema.h>

#include "common/retail_paths.h"

namespace {

namespace mnu = opennova::mnu;
using mnu::SchemaRecord;
using mnu::SchemaShape;
using mnu::SchemaValue;

#define CHECK(cond, msg)                                                   \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::cerr << "FAIL: " << msg << " at line " << __LINE__ << "\n";     \
      return false;                                                        \
    }                                                                      \
  } while (0)

bool parse(const std::string &text, mnu::Document &doc, std::vector<mnu::ParseNote> *notes = nullptr) {
  std::string error;
  if (!mnu::parse(text, doc, error, notes)) {
    std::cerr << "  parse: " << error << "\n";
    return false;
  }
  return true;
}

// Every modeled element and attribute, built in code: a TABLE holding every list and
// every part (a part's own part included), a second root.
mnu::Document every_element() {
  mnu::Appearance image;
  image.state = "default";
  image.type = "image";
  image.value = "a.tga";
  image.has_map_state = image.has_height = true;
  image.map_state = 0;
  image.height = 24;
  image.flags = "STANDARD_TRANSPARENT";
  mnu::Window w;
  w.name = "ALL";
  w.type = mnu::WindowType::Table;
  w.type_token = "table";
  w.hidden = w.disabled = w.checked = w.draw_frame = w.modal = w.readonly = w.as_button = true;
  w.number = w.password = true;
  w.has_group = w.has_minval = w.has_maxval = w.has_maxchar = w.has_form = true;
  w.group = 2;
  w.minval = -1;
  w.maxval = 99;
  w.maxchar = 7;
  w.form = 3;
  w.orientation = "HORIZONTAL";
  w.has_scroll_extent = w.scroll_extent_is_width = true;
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
  action.target_form = 5;
  action.toggle = action.external_browser = true;
  action.test = "EQ";
  action.target = "PANEL";
  w.actions = {action};
  w.string_data = {true, "ID", "CENTER", "BOTTOM", true, 6, true, "KEY"};
  w.toggle_string = {true, "ID", "ALT"};
  w.font = {"f.fnt", "1", "2", "3", "4", "5", "6", "7", "8"};
  w.frame.stencil = "s.tga";
  w.frame.has_stencil_size = w.frame.has_insetx = w.frame.has_insety = true;
  w.frame.stencil_size = 9;
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
  list.name = "DROP";
  list.position.has_top = true;
  list.scrollbar.author(mnu::WindowType::Scroll).shuttle = {image};
  w.has_sb_edge_pad = true;
  w.sb_edge_pad = 4;
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
  w.table_data.column = {true, 1, true, 3, {header}, {body}, {subst}};
  w.table_data.column.primary_sort = 0;
  w.table_data.column.primary_sort_token = "DEFAULT_SORT";
  w.table_data.column.secondary_sort = w.table_data.column.tertiary_sort = 0;
  w.table_data.has_min_item_height = true;
  w.table_data.min_item_height = 20;
  w.table_data.has_fixed_header_height = true;
  w.table_data.fixed_header_height = 11;
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
  child.type_token = "static";
  child.position.has_left = true;
  w.children = {child};
  mnu::Document doc;
  mnu::Screen screen;
  screen.name = "EVERY";
  screen.has_music_var = true;
  screen.music_var = 3;
  screen.roots = {w, child};
  doc.screens = {screen};
  return doc;
}

// A record and where it sits: the screen's index, then each list and index.
struct Walked {
  std::string key; // "0/window:0/action:1"
  SchemaRecord record;
};

void walk(const SchemaRecord &record, const std::string &key, std::vector<Walked> &out) {
  out.push_back({key, record});
  const std::vector<mnu::SchemaList> &lists = mnu::schema_lists(record.shape);
  for (size_t l = 0; l < lists.size(); ++l)
    for (size_t i = 0; i < mnu::schema_list_size(record, l); ++i)
      walk(mnu::schema_list_at(record, l, i), key + "/" + lists[l].path + ":" + std::to_string(i), out);
}

std::vector<Walked> walk(mnu::Document &doc) {
  std::vector<Walked> out;
  for (size_t s = 0; s < doc.screens.size(); ++s) walk(mnu::schema_screen(doc.screens[s]), std::to_string(s), out);
  return out;
}

// Every field of every record: its value and whether it is written.
std::vector<std::string> snapshot(mnu::Document &doc) {
  std::vector<std::string> out;
  for (const Walked &at : walk(doc))
    for (const mnu::SchemaField &field : mnu::schema_fields(at.record.shape)) {
      SchemaValue value;
      mnu::schema_get(at.record, field.path, value);
      std::ostringstream line;
      line << at.key << "|" << field.path << "=";
      if (const auto *text = std::get_if<std::string>(&value)) line << "'" << *text << "'";
      else line << std::get<int64_t>(value);
      line << (mnu::schema_present(at.record, field.path) ? " written" : " left out");
      out.push_back(line.str());
    }
  return out;
}

// `to` rebuilt as `from` through the table alone: each list emptied and refilled with
// default records made over again, then every field set (a block's members before the
// block's own toggle, a Bit field's presence after its value).
bool rebuild(const SchemaRecord &from, const SchemaRecord &to, std::string &error) {
  const std::vector<mnu::SchemaList> &lists = mnu::schema_lists(from.shape);
  for (size_t l = 0; l < lists.size(); ++l) {
    while (mnu::schema_list_size(to, l)) mnu::schema_list_erase(to, l, 0);
    for (size_t i = 0; i < mnu::schema_list_size(from, l); ++i) {
      if (!mnu::schema_list_insert(to, l, i, nullptr, error)) return false;
      if (!rebuild(mnu::schema_list_at(from, l, i), mnu::schema_list_at(to, l, i), error)) return false;
    }
  }
  for (const mnu::SchemaPresence pass : {mnu::SchemaPresence::Always, mnu::SchemaPresence::Block})
    for (const mnu::SchemaField &field : mnu::schema_fields(from.shape)) {
      if ((field.presence == mnu::SchemaPresence::Block) != (pass == mnu::SchemaPresence::Block)) continue;
      SchemaValue value;
      if (!mnu::schema_get(from, field.path, value)) { error = std::string("no value: ") + field.path; return false; }
      if (!mnu::schema_set(to, field.path, value, error)) { error = std::string(field.path) + ": " + error; return false; }
      if (field.presence == mnu::SchemaPresence::Bit &&
          !mnu::schema_set_present(to, field.path, mnu::schema_present(from, field.path), error))
        return false;
    }
  return true;
}

bool rebuilds(mnu::Document &doc, const std::string &label) {
  mnu::Document built;
  built.source_encoding = doc.source_encoding;
  built.screens.resize(doc.screens.size());
  std::string error;
  for (size_t s = 0; s < doc.screens.size(); ++s)
    if (!rebuild(mnu::schema_screen(doc.screens[s]), mnu::schema_screen(built.screens[s]), error)) {
      std::cerr << "  " << label << ": " << error << "\n";
      return false;
    }
  if (mnu::serialize(built) != mnu::serialize(doc)) {
    std::cerr << "  " << label << ": the rebuilt menu writes other bytes\n";
    return false;
  }
  if (snapshot(built) != snapshot(doc)) {
    std::cerr << "  " << label << ": a field reads differently after the rebuild\n";
    return false;
  }
  return true;
}

// A value other than `value` that the field takes (another of its tokens when it has
// them).
SchemaValue changed(const mnu::SchemaField &field, const SchemaValue &value) {
  if (const auto *text = std::get_if<std::string>(&value)) {
    for (const mnu::SchemaChoice &choice : field.choices)
      if (*choice.name && *text != choice.name) return std::string(choice.name);
    std::string next = *text + "Z";
    if (next.size() >= field.width) next = text->empty() ? "Z" : std::string(text->size(), text->front() == 'Z' ? 'Y' : 'Z');
    return next;
  }
  const int64_t number = std::get<int64_t>(value);
  return field.type == mnu::SchemaType::Flag ? int64_t(number ? 0 : 1) : number + 1;
}

// --- the tests --------------------------------------------------------------------------

bool test_table_shape() {
  for (int s = 0; s <= int(SchemaShape::Attribute); ++s) {
    const auto &fields = mnu::schema_fields(SchemaShape(s));
    for (size_t i = 0; i < fields.size(); ++i)
      for (size_t k = i + 1; k < fields.size(); ++k)
        CHECK(std::string(fields[i].path) != fields[k].path, "one path per field: " << fields[i].path);
    const auto &lists = mnu::schema_lists(SchemaShape(s));
    for (size_t i = 0; i < lists.size(); ++i)
      for (size_t k = i + 1; k < lists.size(); ++k)
        CHECK(std::string(lists[i].path) != lists[k].path, "one path per list: " << lists[i].path);
  }
  const auto &window_lists = mnu::schema_lists(SchemaShape::Window);
  CHECK(std::string(window_lists.back().path) == "window", "the child windows last (file order)");
  CHECK(mnu::schema_fields(SchemaShape::Part).size() + 1 == mnu::schema_fields(SchemaShape::Window).size() &&
            !mnu::schema_field(SchemaShape::Part, "type") && mnu::schema_field(SchemaShape::Window, "type"),
        "a part is a window with the owner's TYPE");
  CHECK(mnu::schema_field(SchemaShape::Window, "position.left")->presence == mnu::SchemaPresence::Bit &&
            mnu::schema_field(SchemaShape::Window, "string")->presence == mnu::SchemaPresence::Block &&
            std::string(mnu::schema_field(SchemaShape::Window, "string.value")->block) == "string",
        "presence kinds");
  return true;
}

// Every member reached: the synthetic menu rebuilt through the table alone.
bool test_every_member_reached() {
  mnu::Document doc = every_element();
  CHECK(mnu::write_issues(doc).empty(), "the fixture writes");
  CHECK(rebuilds(doc, "every element"), "rebuilt through the table");
  // And once more after a save and a reload (the model the reader builds).
  mnu::Document back;
  CHECK(parse(mnu::serialize(doc), back), "reparse");
  CHECK(rebuilds(back, "every element, reloaded"), "rebuilt after a reload");
  return true;
}

// The field path of a snapshot line ("0/window:0|string.value='KEY' written").
std::string line_path(const std::string &line) {
  const size_t bar = line.find('|'), equals = line.find('=', bar);
  return line.substr(bar + 1, equals - bar - 1);
}

// Exactly one path: a field set to another value changes that field and nothing else,
// but for the two couplings the format has: a block's toggle decides whether its members
// are written, and a BODY's draw kind and its three flags stay in step.
bool test_no_field_aliases_another() {
  mnu::Document doc = every_element();
  const std::vector<std::string> before = snapshot(doc);
  const std::vector<Walked> records = walk(doc);
  for (size_t r = 0; r < records.size(); ++r) {
    for (const mnu::SchemaField &field : mnu::schema_fields(records[r].record.shape)) {
      mnu::Document copy = doc;
      const Walked target = walk(copy)[r];
      SchemaValue value;
      CHECK(mnu::schema_get(target.record, field.path, value), "reads " << field.path);
      std::string error;
      // A part's toggle only shows or hides a part the window holds (one is added to its
      // list, never made by the toggle).
      const auto &lists = mnu::schema_lists(target.record.shape);
      bool absent_part = false;
      for (size_t l = 0; l < lists.size(); ++l)
        if (lists[l].max == 1 && field.path == std::string(lists[l].path) && !mnu::schema_list_size(target.record, l))
          absent_part = true;
      if (absent_part) {
        CHECK(!mnu::schema_set(target.record, field.path, int64_t(1), error), "no part from its toggle");
        continue;
      }
      CHECK(mnu::schema_set(target.record, field.path, changed(field, value), error),
            "takes another value: " << target.key << " " << field.path << ": " << error);
      const std::vector<std::string> after = snapshot(copy);
      CHECK(after.size() == before.size(), "the shape stays");
      for (size_t i = 0; i < after.size(); ++i) {
        if (after[i] == before[i]) continue;
        const std::string same_record = target.key + "|";
        const bool on_record = after[i].compare(0, same_record.size(), same_record) == 0;
        const std::string path = line_path(after[i]);
        const mnu::SchemaField *other = mnu::schema_field(target.record.shape, path);
        const bool self = on_record && path == field.path;
        // A block's toggle decides whether its members are written; a member set to a new
        // value authors its block.
        const bool member = on_record && other &&
                            ((field.presence == mnu::SchemaPresence::Block && std::string(other->block) == field.path) ||
                             (*field.block && (path == field.block || std::string(other->block) == field.block)));
        const bool draw_kind = on_record && target.record.shape == SchemaShape::Body &&
                               (path == "display" || path == "custom_draw" || path == "bitmap_draw" ||
                                path == "bitmap_text");
        CHECK(self || member || draw_kind, target.key << " " << field.path << " also changed " << after[i]);
      }
    }
  }
  return true;
}

// A Set of a field's own value changes no byte; a Clear leaves the element out.
bool test_same_value_and_clear() {
  mnu::Document doc = every_element();
  const std::string original = mnu::serialize(doc);
  const std::vector<Walked> records = walk(doc);
  for (size_t r = 0; r < records.size(); ++r) {
    for (const mnu::SchemaField &field : mnu::schema_fields(records[r].record.shape)) {
      SchemaValue value;
      std::string error;
      CHECK(mnu::schema_get(records[r].record, field.path, value), "reads");
      CHECK(mnu::schema_set(records[r].record, field.path, value, error) && mnu::serialize(doc) == original,
            "a Set of its own value: " << records[r].key << " " << field.path);
      if (field.presence != mnu::SchemaPresence::Bit || !mnu::schema_present(records[r].record, field.path)) continue;
      mnu::Document copy = doc;
      const Walked target = walk(copy)[r];
      CHECK(mnu::schema_set_present(target.record, field.path, false, error), "Clear " << field.path);
      const std::string cleared = mnu::serialize(copy);
      CHECK(cleared != original, "the Clear leaves " << target.key << " " << field.path << " out");
      mnu::Document back;
      std::vector<mnu::ParseNote> notes;
      // The reader writes the running column index into a HEADER, BODY or SUBST that
      // authors none (the model's one derived index), so that Clear reads back as the index
      // it read; every other field reads back left out.
      const bool derived = std::string(field.path) == "column" &&
                           (target.record.shape == SchemaShape::Header || target.record.shape == SchemaShape::Body ||
                            target.record.shape == SchemaShape::Subst);
      // A window whose one element the Clear took is refused by the writer (retail would
      // not create it), so there is nothing to read back.
      const bool refused = !mnu::write_issues(copy).empty();
      CHECK(refused || (parse(cleared, back, &notes) && mnu::serialize(back) == (derived ? original : cleared)),
            "reads back: " << target.key << " " << field.path);
      if (!derived && !refused) {
        const std::vector<Walked> reread = walk(back);
        CHECK(r < reread.size() && reread[r].key == target.key && !mnu::schema_present(reread[r].record, field.path),
              target.key << " " << field.path << " reads back left out");
      }
      CHECK(mnu::schema_set_present(target.record, field.path, true, error) && mnu::serialize(copy) == original,
            "Write puts it back: " << field.path);
    }
  }
  return true;
}

// Every list's default record writes and reads back as written; a part's toggle leaves
// the part out and brings it back as it was.
bool test_defaults_and_parts() {
  const std::vector<mnu::SchemaList> &lists = mnu::schema_lists(SchemaShape::Window);
  for (size_t l = 0; l < lists.size(); ++l) {
    mnu::Document doc;
    CHECK(parse("<SCREEN><NAME>S</NAME><WINDOW type=\"static\" name=\"W\"><POSITION><LEFT>0</LEFT></POSITION>"
                "</WINDOW></SCREEN>",
                doc),
          "parse");
    const SchemaRecord window = mnu::schema_window(doc.screens[0].roots[0]);
    std::string error;
    CHECK(mnu::schema_list_insert(window, l, 0, nullptr, error), lists[l].path << ": " << error);
    CHECK(mnu::schema_list_size(window, l) == 1 && mnu::schema_list_present(window, l), "added and written");
    CHECK(mnu::write_issues(doc).empty(), lists[l].path << ": the default writes");
    const std::string text = mnu::serialize(doc);
    mnu::Document back;
    std::vector<mnu::ParseNote> notes;
    CHECK(parse(text, back, &notes) && notes.empty() && mnu::serialize(back) == text,
          lists[l].path << ": the default reads back as written");
    CHECK(mnu::schema_list_size(mnu::schema_window(back.screens[0].roots[0]), l) == 1, lists[l].path << " survives");
    if (lists[l].max == 1) {
      CHECK(!mnu::schema_list_insert(window, l, 0, nullptr, error), "a part at most once");
      const std::string toggle = lists[l].path;
      CHECK(mnu::schema_set(window, toggle, int64_t(0), error), "the toggle off");
      CHECK(!mnu::schema_list_present(window, l) && mnu::schema_list_size(window, l) == 1, "left out, kept");
      CHECK(mnu::serialize(doc) != text, "not written");
      CHECK(mnu::schema_set(window, toggle, int64_t(1), error) && mnu::serialize(doc) == text, "back as it was");
    }
  }
  // An ACTION's default verb takes no operand; a toggle cannot make a part that never was.
  mnu::Document doc;
  CHECK(parse("<SCREEN><NAME>S</NAME><WINDOW type=\"static\" name=\"W\"><POSITION><LEFT>0</LEFT></POSITION>"
              "</WINDOW></SCREEN>",
              doc),
        "parse");
  mnu::Window &w = doc.screens[0].roots[0];
  std::string error;
  CHECK(mnu::schema_list_insert(mnu::schema_window(w), 2, 0, nullptr, error) && w.actions[0].type == "POP_SCREEN",
        "POP_SCREEN");
  CHECK(!mnu::schema_set(mnu::schema_window(w), "list_box", int64_t(1), error) && !w.list_box.latent(),
        "no LIST_BOX from its toggle");
  // Adding an ITEMS row authors the ITEMS block.
  CHECK(!w.items.present && mnu::schema_list_insert(mnu::schema_window(w), 10, 0, nullptr, error) && w.items.present,
        "the block authored");
  // A member of a block authors it; its own value does not.
  CHECK(mnu::schema_set(mnu::schema_window(w), "string.value", std::string(), error) && !w.string_data.present,
        "a Set of the value an absent block reads");
  CHECK(mnu::schema_set(mnu::schema_window(w), "string.value", std::string("Hi"), error) && w.string_data.present,
        "a new value authors the STRING");
  CHECK(mnu::schema_set(mnu::schema_window(w), "string", int64_t(0), error) && !w.string_data.present &&
            w.string_data.value == "Hi",
        "the block left out keeps its content");
  return true;
}

// Which type reads what (grill set A3 and the ITEMS arms; docs/mnu/menu-re.md).
bool test_applicability() {
  using mnu::SchemaApplies;
  using T = mnu::WindowType;
  CHECK(mnu::schema_reads(T::Window, "string.value") == SchemaApplies::Ignored, "no STRING on a generic window");
  CHECK(mnu::schema_reads(T::Static, "string.value") == SchemaApplies::Reads, "STATIC reads STRING");
  CHECK(mnu::schema_reads(T::Combo, "toggle_string") == SchemaApplies::Ignored, "COMBOBOX chains to STATIC");
  CHECK(mnu::schema_reads(T::Radio, "group") == SchemaApplies::Reads &&
            mnu::schema_reads(T::Combo, "group") == SchemaApplies::Ignored,
        "GROUP on the R-chain");
  CHECK(mnu::schema_reads(T::Combo, "maxchar") == SchemaApplies::Ignored &&
            mnu::schema_reads(T::RadioEdit, "maxchar") == SchemaApplies::Reads,
        "the edit attributes");
  CHECK(mnu::schema_reads(T::Marquee, "datasource") == SchemaApplies::Reads &&
            mnu::schema_reads(T::Static, "datasource") == SchemaApplies::Ignored,
        "DATASOURCE on MARQUEE_WND only");
  CHECK(mnu::schema_reads(T::Table, "items.item") == SchemaApplies::Ignored &&
            mnu::schema_reads(T::Table, "items.row.item.value") == SchemaApplies::Reads,
        "a TABLE reads ROW cells, not ITEMs");
  CHECK(mnu::schema_reads(T::SpinList, "items.appearance") == SchemaApplies::Ignored &&
            mnu::schema_reads(T::SpinList, "items.item.text") == SchemaApplies::Reads &&
            mnu::schema_reads(T::SpinList, "items.item.pairs_list") == SchemaApplies::Ignored,
        "the SPINLIST form");
  CHECK(mnu::schema_reads(T::List, "items.item.column") == SchemaApplies::Ignored, "no COLUMN on a LIST ITEM");
  CHECK(mnu::schema_reads(T::Combo, "list_box") == SchemaApplies::Reads &&
            mnu::schema_reads(T::List, "list_box") == SchemaApplies::Ignored,
        "LIST_BOX on COMBOBOX");
  CHECK(mnu::schema_reads(T::MultilineEdit, "scrollbar") == SchemaApplies::Reads &&
            mnu::schema_reads(T::Edit, "scrollbar") == SchemaApplies::Ignored,
        "SCROLLBAR");
  CHECK(mnu::schema_reads(T::Button, "position.left") == SchemaApplies::Reads &&
            mnu::schema_reads(T::Gopher, "text_rsrc") == SchemaApplies::Reads,
        "the base parse: every type");
  CHECK(mnu::schema_element_reads(T::LanList, "JOIN_BUTTON") == SchemaApplies::Reads &&
            mnu::schema_element_reads(T::GlbTable, "JOIN_BUTTON") == SchemaApplies::Ignored &&
            mnu::schema_element_reads(T::Gopher, "target") == SchemaApplies::Reads,
        "the extras by tag");
  mnu::Action action;
  action.type = "window";
  CHECK(mnu::schema_action_reads(action, "state") == SchemaApplies::Reads &&
            mnu::schema_action_reads(action, "file") == SchemaApplies::Ignored,
        "WINDOW reads STATE, not FILE");
  action.type = "FORM_POST";
  CHECK(mnu::schema_action_reads(action, "target_form") == SchemaApplies::Unverified, "TARGET_FORM's use is open");
  action.type = "NOPE";
  CHECK(mnu::schema_action_reads(action, "target") == SchemaApplies::Ignored &&
            mnu::schema_action_reads(action, "type") == SchemaApplies::Reads,
        "code 0 is ignored");
  CHECK(mnu::schema_applies_both(SchemaApplies::Reads, SchemaApplies::Unverified) == SchemaApplies::Unverified &&
            mnu::schema_applies_both(SchemaApplies::Unverified, SchemaApplies::Ignored) == SchemaApplies::Ignored,
        "the stricter answer");
  return true;
}

// The references a sibling field decides.
bool test_references() {
  using R = mnu::SchemaReference;
  mnu::Appearance a;
  a.type = "image";
  CHECK(mnu::schema_reference({SchemaShape::Appearance, &a}, "value") == R::MenuTexture, "IMAGE");
  a.type = "IMAGEROW";
  CHECK(mnu::schema_reference({SchemaShape::Appearance, &a}, "value") == R::MenuTexture, "IMAGEROW");
  a.type = "outline";
  CHECK(mnu::schema_reference({SchemaShape::Appearance, &a}, "value") == R::StyleVar, "OUTLINE");
  a.type = "custom";
  CHECK(mnu::schema_reference({SchemaShape::Appearance, &a}, "value") == R::None, "CUSTOM");
  mnu::Item item;
  item.type = "id";
  CHECK(mnu::schema_reference({SchemaShape::Item, &item}, "text") == R::TextId, "ITEM ID");
  item.type = "BITMAP";
  CHECK(mnu::schema_reference({SchemaShape::Item, &item}, "text") == R::MenuTexture, "ITEM BITMAP");
  item.type.clear();
  CHECK(mnu::schema_reference({SchemaShape::Item, &item}, "text") == R::None, "literal text");
  mnu::TableHeader header;
  header.type = "id";
  CHECK(mnu::schema_reference({SchemaShape::Header, &header}, "text") == R::TextId, "HEADER ID");
  mnu::TableSubst subst;
  subst.is_file = true;
  CHECK(mnu::schema_reference({SchemaShape::Subst, &subst}, "file") == R::MenuTexture, "SUBST FILE");
  subst.is_url = true;
  CHECK(mnu::schema_reference({SchemaShape::Subst, &subst}, "file") == R::None, "a URL is fetched");
  mnu::Window w;
  w.string_data.type = "ID";
  CHECK(mnu::schema_reference(mnu::schema_window(w), "string.value") == R::TextId &&
            mnu::schema_reference(mnu::schema_window(w), "toggle_string.value") == R::None,
        "STRING ID");
  CHECK(mnu::schema_reference(mnu::schema_window(w), "font.name") == R::Font &&
            mnu::schema_reference(mnu::schema_window(w), "cursor.file") == R::MenuTexture,
        "the fixed references");
  // An ACTION's target by its verb: a screen for SCREEN, a window for WINDOW, TAB and the
  // two filters; the slot a window for URL; the rest name nothing the file defines.
  mnu::Action action;
  action.type = "screen";
  CHECK(mnu::schema_reference({SchemaShape::Action, &action}, "target") == R::Screen &&
            mnu::schema_reference({SchemaShape::Action, &action}, "field") == R::None &&
            mnu::schema_reference({SchemaShape::Action, &action}, "file") == R::Menu,
        "SCREEN");
  for (const char *verb : {"WINDOW", "TAB", "GLB_FILTER", "glb_filter_num"}) {
    action.type = verb;
    CHECK(mnu::schema_reference({SchemaShape::Action, &action}, "target") == R::Window, verb);
  }
  action.type = "URL";
  CHECK(mnu::schema_reference({SchemaShape::Action, &action}, "target") == R::None &&
            mnu::schema_reference({SchemaShape::Action, &action}, "field") == R::Window,
        "URL's slot");
  for (const char *verb : {"POP_SCREEN", "MNX", "GLB_LOAD", "APPMSG", "NOT_A_VERB"}) {
    action.type = verb;
    CHECK(mnu::schema_reference({SchemaShape::Action, &action}, "target") == R::None, verb);
  }
  mnu::Sound sound;
  std::string source = "nlist.kda";
  CHECK(mnu::schema_reference({SchemaShape::Sound, &sound}, "file") == R::Sound &&
            mnu::schema_reference({SchemaShape::Datasource, &source}, "value") == R::Credits,
        "a SOUND's bank, a DATASOURCE's credits file");
  return true;
}

// The two texts the writer puts down as attribute names (an ACTION's FIELD / SOURCE / NAME
// slot, the table's primary sort key) take only the tokens the reader matches, in any
// case, kept as the reader spells them; the rest changes nothing.
bool test_name_tokens() {
  mnu::Document doc;
  CHECK(parse("<SCREEN><NAME>S</NAME><WINDOW type=\"table\" name=\"T\"><POSITION><LEFT>0</LEFT></POSITION>"
              "<ACTION type=\"URL\" SOURCE=\"slot\">t</ACTION>"
              "<COLUMN count=\"1\"><HEADER column=\"0\" PRIMARY_SORT>H</HEADER></COLUMN></WINDOW></SCREEN>",
              doc),
        "parse");
  const SchemaRecord table = mnu::schema_window(doc.screens[0].roots[0]);
  const std::vector<mnu::SchemaList> &lists = mnu::schema_lists(SchemaShape::Window);
  size_t actions = lists.size();
  for (size_t l = 0; l < lists.size(); ++l)
    if (std::string(lists[l].path) == "action") actions = l;
  const SchemaRecord action = mnu::schema_list_at(table, actions, 0);
  CHECK(action && action.shape == SchemaShape::Action, "the action row");
  const std::string original = mnu::serialize(doc);
  std::string error;
  for (const char *bad : {"X", "A B", "FIELD=\"x\" Y", "SORT", "PRIMARY"}) {
    CHECK(!mnu::schema_set(action, "field_attr", std::string(bad), error), "field_attr refuses " << bad);
    CHECK(!mnu::schema_set(table, "column.primary_sort_token", std::string(bad), error), "the sort key refuses " << bad);
  }
  CHECK(mnu::serialize(doc) == original, "a refused token changes nothing");
  CHECK(mnu::schema_set(action, "field_attr", std::string("name"), error) &&
            doc.screens[0].roots[0].actions[0].field_attr == "NAME",
        "NAME, as the reader spells it");
  CHECK(mnu::schema_set(table, "column.primary_sort_token", std::string("default_sort"), error) &&
            doc.screens[0].roots[0].table_data.column.primary_sort_token == "DEFAULT_SORT",
        "DEFAULT_SORT, as the reader spells it");
  const std::string text = mnu::serialize(doc);
  CHECK(text.find(" NAME=\"slot\"") != std::string::npos && text.find("DEFAULT_SORT") != std::string::npos, "written");
  mnu::Document back;
  std::vector<mnu::ParseNote> notes;
  CHECK(parse(text, back, &notes) && notes.empty() && mnu::serialize(back) == text, "reads back as written");
  return true;
}

// The shipped menus, each rebuilt through the table alone.
bool test_retail_fixtures() {
  static const char *const kFixtures[] = {
      "jo_main", "jo_sp", "jo_mp", "jo_options", "jo_game", "jo_player", "jo_weapon", "jo_loadout",
      "jo_color", "jo_cmap", "jo_stat", "jo_death", "jo_vehicle", "jo_item_db", "jo_splash",
  };
  if (retail::reference_fixture("mnu/jo_main.mnu").empty()) {
    retail::skip_leg("OPENNOVA_JO_ASSETS/fixtures/mnu/jo_*.mnu (the shipped revx02 menus through the table)");
    return true;
  }
  size_t records = 0;
  for (const char *name : kFixtures) {
    const std::string path = retail::reference_fixture((std::string("mnu/") + name + ".mnu").c_str());
    CHECK(!path.empty(), "fixture " << name);
    mnu::Document doc;
    std::string error;
    CHECK(mnu::parse_file(path, doc, error), name << ": " << error);
    records += walk(doc).size();
    CHECK(rebuilds(doc, name), name << " rebuilt through the table");
  }
  std::cout << "  fixture leg: 15 menus, " << records << " records rebuilt through the table\n";
  return true;
}

}  // namespace

int main(int argc, char **argv) {
  retail::configure_mixed(argc, argv);
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
  RUN_TEST(test_table_shape);
  RUN_TEST(test_every_member_reached);
  RUN_TEST(test_no_field_aliases_another);
  RUN_TEST(test_same_value_and_clear);
  RUN_TEST(test_defaults_and_parts);
  RUN_TEST(test_applicability);
  RUN_TEST(test_references);
  RUN_TEST(test_name_tokens);
  RUN_TEST(test_retail_fixtures);
  if (failed > 0) {
    std::cerr << "\n" << failed << " test(s) FAILED\n";
    return EXIT_FAILURE;
  }
  std::cout << "\nAll tests passed!\n";
  return EXIT_SUCCESS;
}
