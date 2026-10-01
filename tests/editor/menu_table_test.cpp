// The menu's table (editor/documents/mnu_table, ADR 0046 S9h; rows of the one table shape since S13
// D10, from the format's property table it was): every member the reader fills is reached by exactly
// one field path or list, so a menu rebuilt from an empty screen through the table alone writes the
// same bytes as the parsed one and setting one field changes no other; a Set of a field's own value
// changes nothing; a Clear leaves the element out; every list's default record writes and reads back;
// the references a sibling decides; the two texts written as attribute names take only the reader's
// tokens; and the shape's parts as the menu fills them (each kind's labelled fields, its choices with
// their words, its lists by their tokens). Over a synthetic menu holding every element, and (a
// SKIP-LEG without OPENNOVA_JO_ASSETS) the fifteen shipped revx02 menus of the reference fixture set.
// Which window type reads what is the format's rule (tests/mnu/mnu_schema_test.cpp).
#include <cstdlib>
#include <functional>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <editor/documents/mnu_table.h>
#include <formats/mnu/mnu.h>

#include "common/retail_paths.h"

namespace {

namespace mnu = opennova::mnu;
using opennova::editor::DetachedRecord;
using opennova::editor::FieldSchema;
using opennova::editor::ListOps;
using opennova::editor::MenuKind;
using opennova::editor::NodeKind;
using opennova::editor::RecordHandle;
using opennova::editor::ReferenceKind;
using opennova::editor::RecordTable;
using opennova::editor::TableKind;
using opennova::editor::TableList;
using opennova::editor::Value;
using opennova::editor::menu_kind;
using opennova::editor::menu_table;
using opennova::editor::menu_window_list;
using opennova::editor::node_kind;

#define CHECK(cond, msg)                                                   \
	do {                                                                   \
		if (!(cond)) {                                                     \
			std::cerr << "FAIL: " << msg << " at line " << __LINE__ << "\n"; \
			return false;                                                  \
		}                                                                  \
	} while (0)

const RecordTable &T() { return menu_table(); }
const TableKind &kind_of(const RecordHandle &record) { return *T().kind(record.kind); }
const char *token(NodeKind kind) { return T().kind(kind)->row().token; }
RecordHandle screen_record(mnu::Screen &screen) { return {node_kind(MenuKind::Screen), &screen}; }
RecordHandle window_record(mnu::Window &window) { return {node_kind(MenuKind::Window), &window}; }

// A block's own toggle (STRING, TOGGLE_STRING, ITEMS, a part's), which says whether the block is
// written; the members of a block are the fields named under it ("string.value").
bool is_block(const std::string &id) {
	static const std::set<std::string> blocks = {"string", "toggle_string", "items", "list_box", "spinup",
	                                             "spindown", "scrollbar"};
	return blocks.count(id) != 0;
}
std::string block_of(const std::string &id) {
	const size_t dot = id.find('.');
	return dot != std::string::npos && is_block(id.substr(0, dot)) ? id.substr(0, dot) : std::string();
}

bool get(const RecordHandle &record, const std::string &id, Value &out) {
	const TableKind &kind = kind_of(record);
	const size_t place = kind.find(id);
	return place != TableKind::npos && kind.value(place).get(record, out);
}
bool set(const RecordHandle &record, const std::string &id, const Value &value, std::string &error) {
	const TableKind &kind = kind_of(record);
	const size_t place = kind.find(id);
	if (place == TableKind::npos || !kind.value(place).set) {
		error = "no set";
		return false;
	}
	return kind.value(place).set(record, value, error);
}
bool present(const RecordHandle &record, const std::string &id) {
	const TableKind &kind = kind_of(record);
	const size_t place = kind.find(id);
	return place != TableKind::npos && (!kind.value(place).present || kind.value(place).present(record));
}
bool set_present(const RecordHandle &record, const std::string &id, bool written, std::string &error) {
	const TableKind &kind = kind_of(record);
	const size_t place = kind.find(id);
	if (place == TableKind::npos || !kind.value(place).set_present) {
		error = "always written";
		return false;
	}
	return kind.value(place).set_present(record, written, error);
}
// What the field names on this record: its kind's per-record answer where its sibling decides it,
// else its schema's.
ReferenceKind reference(const RecordHandle &record, const std::string &id) {
	const TableKind &kind = kind_of(record);
	const size_t place = kind.find(id);
	if (place == TableKind::npos) return ReferenceKind::None;
	return kind.reference(place) ? kind.reference(place)(record) : kind.fields()[place].reference;
}
const ListOps &ops_of(const RecordHandle &owner, size_t list) { return kind_of(owner).lists()[list].ops; }

bool parse(const std::string &text, mnu::Document &doc, std::vector<mnu::ParseNote> *notes = nullptr) {
	std::string error;
	if (!mnu::parse(text, doc, error, notes)) {
		std::cerr << "  parse: " << error << "\n";
		return false;
	}
	return true;
}

// Every modeled element and attribute, built in code: a TABLE holding every list and every part (a
// part's own part included), a second root.
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

// A record and where it sits: the screen's index, then each list's records' kind token and index.
struct Walked {
	std::string key; // "0/window:0/action:1"
	RecordHandle record;
};

void walk(const RecordHandle &record, const std::string &key, std::vector<Walked> &out) {
	out.push_back({key, record});
	const std::vector<TableList> &lists = kind_of(record).lists();
	for (size_t l = 0; l < lists.size(); ++l)
		for (size_t i = 0; i < lists[l].ops.size(record); ++i)
			walk(lists[l].ops.at(record, i), key + "/" + token(lists[l].spec.kind) + ":" + std::to_string(i), out);
}

std::vector<Walked> walk(mnu::Document &doc) {
	std::vector<Walked> out;
	for (size_t s = 0; s < doc.screens.size(); ++s) walk(screen_record(doc.screens[s]), std::to_string(s), out);
	return out;
}

// Every field of every record: its value and whether it is written.
std::vector<std::string> snapshot(mnu::Document &doc) {
	std::vector<std::string> out;
	for (const Walked &at : walk(doc))
		for (const FieldSchema &field : kind_of(at.record).fields()) {
			Value value;
			get(at.record, field.id, value);
			std::ostringstream line;
			line << at.key << "|" << field.id << "=";
			if (const auto *text = std::get_if<std::string>(&value)) line << "'" << *text << "'";
			else line << std::get<int64_t>(value);
			line << (present(at.record, field.id) ? " written" : " left out");
			out.push_back(line.str());
		}
	return out;
}

// `to` rebuilt as `from` through the table alone: each list emptied and refilled with default records
// made over again, then every field set (a block's members before the block's own toggle, an optional
// field's presence after its value).
bool rebuild(const RecordHandle &from, const RecordHandle &to, std::string &error) {
	const std::vector<TableList> &lists = kind_of(from).lists();
	for (size_t l = 0; l < lists.size(); ++l) {
		const ListOps &ops = lists[l].ops;
		while (ops.size(to)) ops.erase(to, 0);
		for (size_t i = 0; i < ops.size(from); ++i) {
			if (!ops.insert(to, i, nullptr, error)) return false;
			if (!rebuild(ops.at(from, i), ops.at(to, i), error)) return false;
		}
	}
	for (const bool blocks : {false, true})
		for (const FieldSchema &field : kind_of(from).fields()) {
			if (is_block(field.id) != blocks) continue;
			Value value;
			if (!get(from, field.id, value)) { error = "no value: " + field.id; return false; }
			if (!set(to, field.id, value, error)) { error = field.id + ": " + error; return false; }
			if (field.optional && !set_present(to, field.id, present(from, field.id), error)) return false;
		}
	return true;
}

bool rebuilds(mnu::Document &doc, const std::string &label) {
	mnu::Document built;
	built.source_encoding = doc.source_encoding;
	built.screens.resize(doc.screens.size());
	std::string error;
	for (size_t s = 0; s < doc.screens.size(); ++s)
		if (!rebuild(screen_record(doc.screens[s]), screen_record(built.screens[s]), error)) {
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

bool is_flag(const FieldSchema &field) {
	return field.choices.size() == 2 && field.choices[0].name == "no" && field.choices[1].name == "yes";
}

// A value other than `value` that the field takes (another of its tokens when it has them).
Value changed(const FieldSchema &field, const Value &value) {
	if (const auto *text = std::get_if<std::string>(&value)) {
		for (const auto &choice : field.choices)
			if (!choice.name.empty() && *text != choice.name) return choice.name;
		std::string next = *text + "Z";
		if (next.size() >= field.width) next = text->empty() ? "Z" : std::string(text->size(), text->front() == 'Z' ? 'Y' : 'Z');
		return next;
	}
	const int64_t number = std::get<int64_t>(value);
	return is_flag(field) ? int64_t(number ? 0 : 1) : number + 1;
}

// --- the tests --------------------------------------------------------------------------

// The table's shape: each kind at its place, each list naming a kind of the table, each field's id its
// own; the child windows last (file order); a part a window with the owner's TYPE; the labelled fields
// and their choices with the words the editor shows.
bool test_table_shape() {
	CHECK(T().well_formed(), "the table is well formed");
	const NodeKind window = node_kind(MenuKind::Window);
	const std::vector<TableList> &window_lists = T().kind(window)->lists();
	CHECK(std::string(token(window_lists.back().spec.kind)) == "window", "the child windows last (file order)");
	for (const TableList &list : window_lists)
		for (const TableList &other : window_lists)
			CHECK(&list == &other || list.spec.kind != other.spec.kind, "one list per kind on a window");
	const TableKind &part = *T().kind(menu_kind("list_box"));
	CHECK(part.fields().size() + 1 == T().kind(window)->fields().size() && part.find("type") == TableKind::npos &&
	              T().kind(window)->find("type") != TableKind::npos,
	      "a part is a window with the owner's TYPE");
	const TableKind &w = *T().kind(window);
	const FieldSchema &left = w.fields()[w.find("position.left")];
	CHECK(left.optional && left.label == "Left" && left.section == "Position", "position.left: optional, Left, Position");
	const FieldSchema &type = w.fields()[w.find("type")];
	CHECK(!type.choices.empty() && type.choices.front().label == "Window (generic)", "the window types' words");
	const FieldSchema &value = w.fields()[w.find("string.value")];
	CHECK(value.multiline && value.reference == ReferenceKind::None && w.reference(w.find("string.value")),
	      "the STRING's text: several lines, its reference its record's");
	CHECK(T().kind(node_kind(MenuKind::Screen))->row().top && std::string(T().kind(node_kind(MenuKind::Screen))->row().add_label) == "Add screen",
	      "a screen is a row of the file");
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

// Exactly one path: a field set to another value changes that field and nothing else, but for the two
// couplings the format has: a block's toggle decides whether its members are written, and a BODY's draw
// kind and its three flags stay in step.
bool test_no_field_aliases_another() {
	mnu::Document doc = every_element();
	const std::vector<std::string> before = snapshot(doc);
	const std::vector<Walked> records = walk(doc);
	for (size_t r = 0; r < records.size(); ++r) {
		for (const FieldSchema &field : kind_of(records[r].record).fields()) {
			mnu::Document copy = doc;
			const Walked target = walk(copy)[r];
			Value value;
			CHECK(get(target.record, field.id, value), "reads " << field.id);
			std::string error;
			// A part's toggle only shows or hides a part the window holds (one is added to its list,
			// never made by the toggle).
			const std::vector<TableList> &lists = kind_of(target.record).lists();
			bool absent_part = false;
			for (const TableList &list : lists)
				if (list.spec.max == 1 && field.id == token(list.spec.kind) && !list.ops.size(target.record))
					absent_part = true;
			if (absent_part) {
				CHECK(!set(target.record, field.id, int64_t(1), error), "no part from its toggle");
				continue;
			}
			CHECK(set(target.record, field.id, changed(field, value), error),
			      "takes another value: " << target.key << " " << field.id << ": " << error);
			const std::vector<std::string> after = snapshot(copy);
			CHECK(after.size() == before.size(), "the shape stays");
			for (size_t i = 0; i < after.size(); ++i) {
				if (after[i] == before[i]) continue;
				const std::string same_record = target.key + "|";
				const bool on_record = after[i].compare(0, same_record.size(), same_record) == 0;
				const std::string path = line_path(after[i]);
				const bool self = on_record && path == field.id;
				// A block's toggle decides whether its members are written; a member set to a new value
				// authors its block.
				const std::string own_block = block_of(field.id);
				const bool member = on_record && ((is_block(field.id) && block_of(path) == field.id) ||
				                                  (!own_block.empty() && (path == own_block || block_of(path) == own_block)));
				const bool draw_kind = on_record && std::string(token(target.record.kind)) == "column.body" &&
				                       (path == "display" || path == "custom_draw" || path == "bitmap_draw" ||
				                        path == "bitmap_text");
				CHECK(self || member || draw_kind, target.key << " " << field.id << " also changed " << after[i]);
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
		for (const FieldSchema &field : kind_of(records[r].record).fields()) {
			Value value;
			std::string error;
			CHECK(get(records[r].record, field.id, value), "reads");
			CHECK(set(records[r].record, field.id, value, error) && mnu::serialize(doc) == original,
			      "a Set of its own value: " << records[r].key << " " << field.id);
			if (!field.optional || !present(records[r].record, field.id)) continue;
			mnu::Document copy = doc;
			const Walked target = walk(copy)[r];
			CHECK(set_present(target.record, field.id, false, error), "Clear " << field.id);
			const std::string cleared = mnu::serialize(copy);
			CHECK(cleared != original, "the Clear leaves " << target.key << " " << field.id << " out");
			mnu::Document back;
			std::vector<mnu::ParseNote> notes;
			// The reader writes the running column index into a HEADER, BODY or SUBST that authors none
			// (the model's one derived index), so that Clear reads back as the index it read; every other
			// field reads back left out.
			const std::string kind = token(target.record.kind);
			const bool derived = field.id == "column" &&
			                     (kind == "column.header" || kind == "column.body" || kind == "column.subst");
			// A window whose one element the Clear took is refused by the writer (retail would not create
			// it), so there is nothing to read back.
			const bool refused = !mnu::write_issues(copy).empty();
			CHECK(refused || (parse(cleared, back, &notes) && mnu::serialize(back) == (derived ? original : cleared)),
			      "reads back: " << target.key << " " << field.id);
			if (!derived && !refused) {
				const std::vector<Walked> reread = walk(back);
				CHECK(r < reread.size() && reread[r].key == target.key && !present(reread[r].record, field.id),
				      target.key << " " << field.id << " reads back left out");
			}
			CHECK(set_present(target.record, field.id, true, error) && mnu::serialize(copy) == original,
			      "Write puts it back: " << field.id);
		}
	}
	return true;
}

// Every list's default record writes and reads back as written; a part's toggle leaves the part out and
// brings it back as it was.
bool test_defaults_and_parts() {
	const std::vector<TableList> &lists = T().kind(node_kind(MenuKind::Window))->lists();
	for (size_t l = 0; l < lists.size(); ++l) {
		const std::string path = token(lists[l].spec.kind);
		mnu::Document doc;
		CHECK(parse("<SCREEN><NAME>S</NAME><WINDOW type=\"static\" name=\"W\"><POSITION><LEFT>0</LEFT></POSITION>"
		            "</WINDOW></SCREEN>",
		            doc),
		      "parse");
		const RecordHandle window = window_record(doc.screens[0].roots[0]);
		const ListOps &ops = lists[l].ops;
		std::string error;
		CHECK(ops.insert(window, 0, nullptr, error), path << ": " << error);
		CHECK(ops.size(window) == 1 && (!ops.present || ops.present(window)), "added and written");
		CHECK(mnu::write_issues(doc).empty(), path << ": the default writes");
		const std::string text = mnu::serialize(doc);
		mnu::Document back;
		std::vector<mnu::ParseNote> notes;
		CHECK(parse(text, back, &notes) && notes.empty() && mnu::serialize(back) == text,
		      path << ": the default reads back as written");
		CHECK(ops.size(window_record(back.screens[0].roots[0])) == 1, path << " survives");
		if (lists[l].spec.max == 1) {
			CHECK(!ops.insert(window, 0, nullptr, error), "a part at most once");
			CHECK(set(window, path, int64_t(0), error), "the toggle off");
			CHECK(!ops.present(window) && ops.size(window) == 1, "left out, kept");
			CHECK(mnu::serialize(doc) != text, "not written");
			CHECK(set(window, path, int64_t(1), error) && mnu::serialize(doc) == text, "back as it was");
		}
	}
	// An ACTION's default verb takes no operand; a toggle cannot make a part that never was.
	mnu::Document doc;
	CHECK(parse("<SCREEN><NAME>S</NAME><WINDOW type=\"static\" name=\"W\"><POSITION><LEFT>0</LEFT></POSITION>"
	            "</WINDOW></SCREEN>",
	            doc),
	      "parse");
	mnu::Window &w = doc.screens[0].roots[0];
	const RecordHandle window = window_record(w);
	std::string error;
	CHECK(ops_of(window, menu_window_list("action")).insert(window, 0, nullptr, error) && w.actions[0].type == "POP_SCREEN",
	      "POP_SCREEN");
	CHECK(!set(window, "list_box", int64_t(1), error) && !w.list_box.latent(), "no LIST_BOX from its toggle");
	// Adding an ITEMS row authors the ITEMS block.
	CHECK(!w.items.present && ops_of(window, menu_window_list("items.item")).insert(window, 0, nullptr, error) &&
	              w.items.present,
	      "the block authored");
	// A member of a block authors it; its own value does not.
	CHECK(set(window, "string.value", std::string(), error) && !w.string_data.present,
	      "a Set of the value an absent block reads");
	CHECK(set(window, "string.value", std::string("Hi"), error) && w.string_data.present, "a new value authors the STRING");
	CHECK(set(window, "string", int64_t(0), error) && !w.string_data.present && w.string_data.value == "Hi",
	      "the block left out keeps its content");
	// A list takes a record of its own kind only, and a copy reads back as the record it was.
	const ListOps &actions = ops_of(window, menu_window_list("action"));
	DetachedRecord copied = actions.copy(window, 0);
	CHECK(copied.data && copied.kind == menu_kind("action"), "a copy of the action");
	DetachedRecord wrong = copied;
	wrong.kind = menu_kind("sound");
	CHECK(!actions.insert(window, 1, &wrong, error) && error == "The Actions take Action records only.",
	      "an action list refuses a sound");
	CHECK(actions.insert(window, 1, &copied, error) && w.actions.size() == 2 && w.actions[1].type == "POP_SCREEN" &&
	              actions.erase(window, 1) && w.actions.size() == 1,
	      "the copy in and out");
	return true;
}

// The references a sibling field decides.
bool test_references() {
	using R = ReferenceKind;
	const auto at = [](const char *kind, void *data) { return RecordHandle{menu_kind(kind), data}; };
	mnu::Appearance a;
	a.type = "image";
	CHECK(reference(at("appearance", &a), "value") == R::MenuTexture, "IMAGE");
	a.type = "IMAGEROW";
	CHECK(reference(at("appearance", &a), "value") == R::MenuTexture, "IMAGEROW");
	a.type = "outline";
	CHECK(reference(at("appearance", &a), "value") == R::StyleVar, "OUTLINE");
	a.type = "custom";
	CHECK(reference(at("appearance", &a), "value") == R::None, "CUSTOM");
	mnu::Item item;
	item.type = "id";
	CHECK(reference(at("items.item", &item), "text") == R::TextId, "ITEM ID");
	item.type = "BITMAP";
	CHECK(reference(at("item", &item), "text") == R::MenuTexture, "ITEM BITMAP");
	item.type.clear();
	CHECK(reference(at("items.item", &item), "text") == R::None, "literal text");
	mnu::TableHeader header;
	header.type = "id";
	CHECK(reference(at("column.header", &header), "text") == R::TextId, "HEADER ID");
	mnu::TableSubst subst;
	subst.is_file = true;
	CHECK(reference(at("column.subst", &subst), "file") == R::MenuTexture, "SUBST FILE");
	subst.is_url = true;
	CHECK(reference(at("column.subst", &subst), "file") == R::None, "a URL is fetched");
	mnu::Window w;
	w.string_data.type = "ID";
	CHECK(reference(window_record(w), "string.value") == R::TextId &&
	              reference(window_record(w), "toggle_string.value") == R::None,
	      "STRING ID");
	CHECK(reference(window_record(w), "font.name") == R::Font && reference(window_record(w), "cursor.file") == R::MenuTexture,
	      "the fixed references");
	// An ACTION's target by its verb: a screen for SCREEN, a window for WINDOW, TAB and the two filters;
	// the slot a window for URL; the rest name nothing the file defines.
	mnu::Action action;
	action.type = "screen";
	CHECK(reference(at("action", &action), "target") == R::MenuScreen && reference(at("action", &action), "field") == R::None &&
	              reference(at("action", &action), "file") == R::Menu,
	      "SCREEN");
	for (const char *verb : {"WINDOW", "TAB", "GLB_FILTER", "glb_filter_num"}) {
		action.type = verb;
		CHECK(reference(at("action", &action), "target") == R::MenuWindow, verb);
	}
	action.type = "URL";
	CHECK(reference(at("action", &action), "target") == R::None && reference(at("action", &action), "field") == R::MenuWindow,
	      "URL's slot");
	for (const char *verb : {"POP_SCREEN", "MNX", "GLB_LOAD", "APPMSG", "NOT_A_VERB"}) {
		action.type = verb;
		CHECK(reference(at("action", &action), "target") == R::None, verb);
	}
	mnu::Sound sound;
	std::string source = "nlist.kda";
	CHECK(reference(at("sound", &sound), "file") == R::SoundBank && reference(at("datasource", &source), "value") == R::Credits,
	      "a SOUND's bank, a DATASOURCE's credits file");
	return true;
}

// The two texts the writer puts down as attribute names (an ACTION's FIELD / SOURCE / NAME slot, the
// table's primary sort key) take only the tokens the reader matches, in any case, kept as the reader
// spells them; the rest changes nothing.
bool test_name_tokens() {
	mnu::Document doc;
	CHECK(parse("<SCREEN><NAME>S</NAME><WINDOW type=\"table\" name=\"T\"><POSITION><LEFT>0</LEFT></POSITION>"
	            "<ACTION type=\"URL\" SOURCE=\"slot\">t</ACTION>"
	            "<COLUMN count=\"1\"><HEADER column=\"0\" PRIMARY_SORT>H</HEADER></COLUMN></WINDOW></SCREEN>",
	            doc),
	      "parse");
	const RecordHandle table = window_record(doc.screens[0].roots[0]);
	const RecordHandle action = ops_of(table, menu_window_list("action")).at(table, 0);
	CHECK(action && action.kind == menu_kind("action"), "the action row");
	const std::string original = mnu::serialize(doc);
	std::string error;
	for (const char *bad : {"X", "A B", "FIELD=\"x\" Y", "SORT", "PRIMARY"}) {
		CHECK(!set(action, "field_attr", std::string(bad), error), "field_attr refuses " << bad);
		CHECK(!set(table, "column.primary_sort_token", std::string(bad), error), "the sort key refuses " << bad);
	}
	CHECK(mnu::serialize(doc) == original, "a refused token changes nothing");
	CHECK(set(action, "field_attr", std::string("name"), error) && doc.screens[0].roots[0].actions[0].field_attr == "NAME",
	      "NAME, as the reader spells it");
	CHECK(set(table, "column.primary_sort_token", std::string("default_sort"), error) &&
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

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	int failed = 0;
#define RUN_TEST(name)                         \
	do {                                       \
		std::cout << "Running " #name "... "; \
		if (name()) {                          \
			std::cout << "OK\n";               \
		} else {                               \
			std::cout << "FAILED\n";           \
			++failed;                          \
		}                                      \
	} while (0)
	RUN_TEST(test_table_shape);
	RUN_TEST(test_every_member_reached);
	RUN_TEST(test_no_field_aliases_another);
	RUN_TEST(test_same_value_and_clear);
	RUN_TEST(test_defaults_and_parts);
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
