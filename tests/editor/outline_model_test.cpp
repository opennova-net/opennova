// S13 V3 (ADR 0046 S13): a document's outline model (ui/outline_model), with no ImGui, in its three
// modes. A list (a catalog): the rows, a row of another kind than the file's own after its kind's
// label (a weapon table's carry limit), filtered by name in any case, sorted by name as a display
// order that keeps each row's place in the file. A tree (a model): the rows first, a record's
// collections under it once it is open and a collection's records once that is, closed again with
// what they hold; a reveal opening what holds a nested record, which is then listed; a filter keeping
// the records that match with what holds them, held open (their arrows changing nothing), and
// leaving what is open otherwise as it was; a record kept for itself with nothing under it kept
// opening onto all it holds. Master and detail (a string table): every row as a master; the master
// row's strings as the detail, its columns the kind's text fields; a filter over them; every row's
// matches with Every; a text over several lines counted (eight at most). A filter of blanks alone
// keeps everything. A reveal of a record the filter hides clears the filter. The lines are made
// again only when what they read moves (lines_made): the document (another instance, or the same
// one loaded again, and its revision), what is open, the filter, the order, Every, the master row;
// the file-wide values come from the hook a type gives, their heading open as the model has it.
// ADR 0046 S14: a list or a tree lists the kinds its mask keeps and leaves out the rows its type's
// listed hook does until all rows are listed, a reveal winning over both; a click selects a record
// alone, with Ctrl joining or leaving, with Shift the lines from the primary's; and the Inspector's
// shared form spans records of kinds whose fields are alike (ui/inspector_layout). ADR 0046 S15: a
// mission's tree reads like a mission (its rows under its type's headings: each pool, its teams and
// groups where they tell rows apart, each with its count; a heading closed hides its rows, a reveal
// opens it, a filter by a record's words keeps its headings), its rows titled with the names given.
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <editor/documents/def_catalog_document.h>
#include <editor/documents/document_types.h>
#include <editor/documents/mission_document.h>
#include <editor/documents/mission_labels.h>
#include <editor/documents/model_document.h>
#include <editor/documents/strings_document.h>
#include <editor/ui/inspector_layout.h>
#include <editor/ui/outline_model.h>
#include <formats/rtxt/rtxt.h>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/pool_document.h"

using namespace opennova::editor;

namespace {

std::vector<uint8_t> bytes_of(const std::string &text) { return std::vector<uint8_t>(text.begin(), text.end()); }

template <class T> bool load(T &document, const std::vector<uint8_t> &bytes, const char *name, AssetKind kind) {
	Diagnostic error;
	return document.load_bytes(bytes, name, kind, "jo", error);
}

// The lines' texts, in order.
std::vector<std::string> texts(const std::vector<OutlineLine> &lines) {
	std::vector<std::string> out;
	for (const OutlineLine &line : lines) out.push_back(line.text);
	return out;
}

int test_list() {
	DefCatalogDocument items;
	TEST_EXPECT(load(items,
	                 bytes_of("begin \"Zulu Truck\"\nid 100300\ntype vehicle\nend\n"
	                          "begin \"alpha jeep\"\nid 100301\ntype vehicle\nend\n"
	                          "begin \"Mike Boat\"\nid 100302\ntype vehicle\nend\n"),
	                 "items.def", AssetKind::ItemDefs));
	OutlineModel list(OutlineMode::List);
	const std::vector<OutlineLine> &lines = list.lines(items);
	TEST_EXPECT(texts(lines) == std::vector<std::string>({"Zulu Truck", "alpha jeep", "Mike Boat"}));
	TEST_EXPECT(lines[1].index == 1 && lines[1].address.row == items.rows()[1]->id && !lines[1].collection);
	TEST_EXPECT(list.lines_made() == 1);
	// Asked again with nothing moved: the same lines, not made again.
	list.lines(items);
	TEST_EXPECT(list.lines_made() == 1);
	// By name, any case: a display order, each row keeping its place in the file.
	list.set_sort(true);
	TEST_EXPECT(texts(list.lines(items)) == std::vector<std::string>({"alpha jeep", "Mike Boat", "Zulu Truck"}));
	TEST_EXPECT(list.lines(items)[0].index == 1 && list.lines(items)[2].index == 0 && list.lines_made() == 2);
	// The filter, any case; the same filter again makes nothing.
	list.set_filter("TRUCK");
	TEST_EXPECT(texts(list.lines(items)) == std::vector<std::string>({"Zulu Truck"}) && list.lines_made() == 3);
	list.set_filter("TRUCK");
	list.lines(items);
	TEST_EXPECT(list.lines_made() == 3);
	TEST_EXPECT(list.line_of({items.rows()[0]->id, items.rows()[0]->kind, 0}) == 0);
	TEST_EXPECT(list.line_of({items.rows()[1]->id, items.rows()[1]->kind, 0}) == SIZE_MAX);
	// An edit (the document's revision alone) follows its change set (S13 V8): the renamed row's
	// line made again alone, which the filter drops now; its undo the same.
	Edit rename;
	rename.address = {items.rows()[0]->id, items.rows()[0]->kind, 0};
	rename.field = "display_name";
	rename.value = std::string("Zulu Car");
	Diagnostic error;
	TEST_EXPECT(items.apply(rename, error));
	const size_t rows_made = list.rows_made();
	TEST_EXPECT(list.lines(items).empty() && list.lines_made() == 3 && list.rows_made() == rows_made + 1);
	items.undo();
	TEST_EXPECT(list.lines(items).size() == 1 && list.lines_made() == 3 && list.rows_made() == rows_made + 2);
	// A filter of blanks alone keeps every row.
	list.set_filter("   ");
	TEST_EXPECT(!list.filtered() && list.lines(items).size() == 3);

	// A reveal of a row the filter hides clears the filter (the reveal wins): its line then; one
	// the filter shows leaves it.
	const NodeAddress jeep{items.rows()[1]->id, items.rows()[1]->kind, 0};
	OutlineModel shown(OutlineMode::List);
	shown.set_filter("truck");
	TEST_EXPECT(shown.lines(items).size() == 1);
	TEST_EXPECT(shown.reveal(items, {jeep}) == 1 && shown.filter().empty() && shown.lines(items).size() == 3);
	shown.set_filter("jeep");
	TEST_EXPECT(shown.reveal(items, {jeep}) == 0 && shown.filter() == "jeep");

	// A weapon table: a carry limit is a row of its own (the table keeps them first), after its
	// kind's label; a weapon, the file's own kind, by its name alone.
	DefCatalogDocument weapons;
	TEST_EXPECT(load(weapons,
	                 bytes_of("weapon \"WPN_ONE\"\ncategory 11\nend\nammoclass_max_carry CLASS_AK47 300\n"),
	                 "weapon.def", AssetKind::WeaponDefs));
	OutlineModel carry(OutlineMode::List);
	const std::vector<OutlineLine> &rows = carry.lines(weapons);
	TEST_EXPECT(rows.size() == 2);
	for (const OutlineLine &row : rows)
		TEST_EXPECT(row.text == (row.address.kind == node_kind(opennova::def::DefRecordKind::Carry) ? "Carry limit: CLASS_AK47" : "WPN_ONE"));
	// The filter reads the name, not the label.
	carry.set_filter("carry");
	TEST_EXPECT(carry.lines(weapons).empty());
	std::printf("test_list passed\n");
	return 0;
}

int test_tree() {
	const std::string repo = test_paths_repo_root(__FILE__);
	ModelDocument model;
	TEST_EXPECT(load(model, test_io::read_file(repo + "/fixtures/threedi/synth/armory.3di"), "armory.3di", AssetKind::Model));
	OutlineModel tree(OutlineMode::Tree);
	// The rows alone, each a branch while it holds collections, closed.
	const std::vector<OutlineLine> first = tree.lines(model);
	TEST_EXPECT(first.size() == model.rows().size());
	for (const OutlineLine &line : first) TEST_EXPECT(line.depth == 0 && !line.open && !line.collection);
	const OutlineLine row = first.front();
	TEST_EXPECT(row.branch);
	const std::vector<Document::Collection> held = model.collections_of(row.address);
	TEST_EXPECT(!held.empty());
	// The row opened: its collections under it, each by its label and count, closed.
	tree.set_open(row, true);
	const std::vector<OutlineLine> opened = tree.lines(model);
	TEST_EXPECT(opened.size() == first.size() + held.size() && tree.lines_made() == 2);
	TEST_EXPECT(opened[0].open && opened[1].collection && opened[1].depth == 1 && opened[1].kind == held[0].spec.kind);
	TEST_EXPECT(opened[1].text == std::string(held[0].spec.label) + " (" + std::to_string(held[0].ids.size()) + ")");
	// A collection that holds records opened: its records at depth 2.
	size_t at = SIZE_MAX;
	for (size_t i = 1; i <= held.size() && at == SIZE_MAX; ++i)
		if (opened[i].branch) at = i;
	TEST_EXPECT(at != SIZE_MAX);
	const OutlineLine collection = opened[at];
	tree.set_open(collection, true);
	const std::vector<OutlineLine> records = tree.lines(model);
	TEST_EXPECT(records.size() == opened.size() + collection.count && tree.lines_made() == 3);
	const OutlineLine nested = records[at + 1];
	TEST_EXPECT(!nested.collection && nested.depth == 2 && nested.index == 0 && nested.address.kind == collection.kind &&
	            nested.text == model.record_title(nested.address));
	// Opening what is open changes nothing: the lines are not made again.
	tree.set_open(collection, true);
	tree.lines(model);
	TEST_EXPECT(tree.lines_made() == 3);
	// The row closed: what it holds goes, what is open under it stays for when it opens.
	tree.set_open(row, false);
	TEST_EXPECT(tree.lines(model).size() == first.size() && tree.is_open(collection));
	tree.set_open(row, true);
	TEST_EXPECT(tree.lines(model).size() == records.size());

	// A reveal of the collection's last record from everything closed: its holders open, its line
	// the one it answers; revealing it again opens nothing and makes nothing.
	OutlineModel fresh(OutlineMode::Tree);
	const NodeAddress last{nested.address.row, nested.address.kind, held[at - 1].ids.back()};
	std::vector<NodeAddress> path = model.ancestors(last);
	path.push_back(last);
	TEST_EXPECT(fresh.lines(model).size() == first.size() && fresh.line_of(last) == SIZE_MAX);
	const size_t line = fresh.reveal(model, path);
	TEST_EXPECT(line != SIZE_MAX && line == fresh.line_of(last) &&
	            fresh.lines(model)[line].depth == int(path.size()) * 2 - 2);
	const size_t made = fresh.lines_made();
	TEST_EXPECT(fresh.reveal(model, path) == line && fresh.lines_made() == made);
	// Hidden by a filter, the record revealed clears it.
	OutlineModel hidden(OutlineMode::Tree);
	hidden.set_filter("NO_SUCH_RECORD_NAME");
	TEST_EXPECT(hidden.lines(model).empty());
	const size_t unhidden = hidden.reveal(model, path);
	TEST_EXPECT(unhidden != SIZE_MAX && hidden.filter().empty() && hidden.lines(model)[unhidden].address == last);

	// A filter keeps a record whose title holds it and what holds it, held open whatever is open
	// (forced: an arrow opening or closing one changes nothing); the filter gone, what was open is
	// listed again as it was.
	const std::string title = nested.text;
	OutlineModel filtered(OutlineMode::Tree);
	filtered.set_filter(title);
	const std::vector<OutlineLine> found = filtered.lines(model);
	bool reached = false;
	for (const OutlineLine &kept : found) reached = reached || (!kept.collection && kept.address == nested.address);
	TEST_EXPECT(reached && found.front().open && found.front().forced);
	for (const OutlineLine &kept : found)
		if (kept.collection || kept.address != nested.address)
			TEST_EXPECT(kept.open && kept.forced); // every other line holds a match
	const size_t filtered_made = filtered.lines_made();
	filtered.set_open(found.front(), true);
	filtered.lines(model);
	TEST_EXPECT(!filtered.is_open(found.front()) && filtered.lines_made() == filtered_made);
	filtered.set_filter("");
	TEST_EXPECT(filtered.lines(model).size() == first.size());
	filtered.set_filter("NO_SUCH_RECORD_NAME");
	TEST_EXPECT(filtered.lines(model).empty());

	// A record kept for itself with nothing under it kept (a row whose name no record under it
	// holds: the collision row, the armory's user point being "Armory" too): not held open, closed
	// as it is unfiltered, and opened onto every collection it holds (the filter keeps none of them).
	OutlineModel matched(OutlineMode::Tree);
	size_t row_at = SIZE_MAX;
	OutlineLine kept;
	for (const OutlineLine &candidate : first) {
		if (!candidate.branch || row_at != SIZE_MAX) continue;
		matched.set_filter(candidate.text);
		const std::vector<OutlineLine> &alone = matched.lines(model);
		for (size_t i = 0; i < alone.size() && row_at == SIZE_MAX; ++i)
			if (!alone[i].collection && alone[i].address == candidate.address && !alone[i].forced) {
				row_at = i;
				kept = alone[i];
			}
	}
	TEST_EXPECT(row_at != SIZE_MAX && !kept.open);
	matched.set_open(kept, true);
	const std::vector<OutlineLine> onto = matched.lines(model);
	size_t under = 0;
	for (size_t i = row_at + 1; i < onto.size() && onto[i].depth > 0; ++i) under += onto[i].collection && onto[i].depth == 1;
	TEST_EXPECT(onto[row_at].open && under == model.collections_of(kept.address).size());
	std::printf("test_tree passed\n");
	return 0;
}

int test_master_detail() {
	opennova::rtxt::File table;
	table.sections = {{"Menu", 3}, {"Help", 2}};
	table.entries = {{"MM_Play", "Play", {}, 0},
	                 {"MM_Quit", "Quit the game", {}, 0},
	                 {"MM_Long", "one\ntwo\nthree", {}, 0},
	                 {"HLP_Play", "How to play", {}, 1},
	                 {"HLP_Tall", "1\n2\n3\n4\n5\n6\n7\n8\n9\n10", {}, 1}};
	std::vector<uint8_t> bytes;
	std::string io_error;
	TEST_EXPECT(opennova::rtxt::write(table, bytes, io_error));
	StringsDocument strings;
	TEST_EXPECT(load(strings, bytes, "strings.bin", AssetKind::Strings));
	TEST_EXPECT(strings.rows().size() == 2);
	const NodeId menu = strings.rows()[0]->id, help = strings.rows()[1]->id;
	OutlineModel outline(OutlineMode::MasterDetail);
	// No master row: the masters and the columns, no detail.
	TEST_EXPECT(outline.lines(strings, 0).empty());
	TEST_EXPECT(texts(outline.masters()) == std::vector<std::string>({"Menu", "Help"}));
	TEST_EXPECT(outline.columns().size() == 2 && outline.columns()[0]->id == "key" && outline.columns()[1]->id == "text");
	TEST_EXPECT(std::string(outline.detail_label()) == "Strings" && outline.detail_kind() == node_kind(StringsKind::String));
	// The master row's strings, each its place, its row's name and the lines its tallest cell shows.
	const std::vector<OutlineLine> lines = outline.lines(strings, menu);
	TEST_EXPECT(lines.size() == 3 && outline.lines_made() == 2);
	TEST_EXPECT(lines[2].index == 2 && lines[2].row_name == "Menu" && lines[2].lines == 3 && lines[0].lines == 1);
	TEST_EXPECT(outline.lines(strings, help)[1].lines == 8 && outline.lines_made() == 3); // ten lines: eight shown
	// A filter over the key and the text, in any case: the master row's alone.
	outline.set_filter("play");
	TEST_EXPECT(outline.lines(strings, menu).size() == 1);
	// Every: every row's that match, each with its row's name.
	outline.set_every(true);
	TEST_EXPECT(outline.every_row());
	const std::vector<OutlineLine> every = outline.lines(strings, menu);
	TEST_EXPECT(every.size() == 2 && every[0].row_name == "Menu" && every[1].row_name == "Help");
	// Every with no filter, or one of blanks alone: the master row's alone.
	outline.set_filter("");
	TEST_EXPECT(!outline.every_row() && outline.lines(strings, menu).size() == 3);
	outline.set_filter("   ");
	TEST_EXPECT(!outline.filtered() && !outline.every_row() && outline.lines(strings, menu).size() == 3);
	outline.set_filter("");
	outline.lines(strings, menu);
	const size_t made = outline.lines_made();
	outline.lines(strings, menu);
	TEST_EXPECT(outline.lines_made() == made);
	TEST_EXPECT(outline.line_of(lines[1].address) == 1);
	// A string the filter hides, revealed: the filter cleared, its line the one it answers. A row
	// (the master column lists every one) reveals nothing there and keeps the filter.
	outline.set_every(false);
	outline.set_filter("quit");
	TEST_EXPECT(outline.lines(strings, menu).size() == 1);
	const NodeAddress section{menu, strings.rows()[0]->kind, 0};
	TEST_EXPECT(outline.reveal(strings, {section}, menu) == SIZE_MAX && outline.filter() == "quit");
	TEST_EXPECT(outline.reveal(strings, {section, lines[0].address}, menu) == 0 && outline.filter().empty());
	std::printf("test_master_detail passed\n");
	return 0;
}

// The lines follow the document they are asked of: another instance at the same revision makes them
// again, and so does the same instance loaded again (its revision back where it was).
int test_instances() {
	DefCatalogDocument a, b;
	TEST_EXPECT(load(a, bytes_of("begin \"Alpha\"\nid 100300\ntype vehicle\nend\n"), "a.def", AssetKind::ItemDefs));
	TEST_EXPECT(load(b, bytes_of("begin \"Bravo\"\nid 100301\ntype vehicle\nend\n"), "b.def", AssetKind::ItemDefs));
	TEST_EXPECT(a.revision() == b.revision() && a.identity() != b.identity());
	OutlineModel list(OutlineMode::List);
	TEST_EXPECT(texts(list.lines(a)) == std::vector<std::string>({"Alpha"}) && list.lines_made() == 1);
	TEST_EXPECT(texts(list.lines(b)) == std::vector<std::string>({"Bravo"}) && list.lines_made() == 2);
	TEST_EXPECT(texts(list.lines(a)) == std::vector<std::string>({"Alpha"}) && list.lines_made() == 3);
	const uint64_t revision = a.revision();
	TEST_EXPECT(load(a, bytes_of("begin \"Charlie\"\nid 100302\ntype vehicle\nend\n"), "a.def", AssetKind::ItemDefs));
	TEST_EXPECT(a.revision() == revision);
	TEST_EXPECT(texts(list.lines(a)) == std::vector<std::string>({"Charlie"}) && list.lines_made() == 4);
	std::printf("test_instances passed\n");
	return 0;
}

// S13 V8: the lines follow the document's change sets, the rows made again counted (rows_made). A
// list of five: an edit of one row makes its line alone; the rows moved re-ordered with none made;
// a row added makes it alone, a row removed none; in name order, a renamed row's line made alone and
// the list sorted again. A tree (a model): a user point's edit makes the model row's lines alone,
// the collision row's kept. Master and detail (a string table): a string's edit makes its section's
// lines alone. Everything made anew when the document cannot say (read again in place).
int test_change_sets() {
	DefCatalogDocument items;
	TEST_EXPECT(load(items,
	                 bytes_of("begin \"Echo\"\nid 100300\ntype vehicle\nend\n"
	                          "begin \"Delta\"\nid 100301\ntype vehicle\nend\n"
	                          "begin \"Charlie\"\nid 100302\ntype vehicle\nend\n"
	                          "begin \"Bravo\"\nid 100303\ntype vehicle\nend\n"
	                          "begin \"Alpha\"\nid 100304\ntype vehicle\nend\n"),
	                 "items.def", AssetKind::ItemDefs));
	OutlineModel list(OutlineMode::List);
	TEST_EXPECT(texts(list.lines(items)) == std::vector<std::string>({"Echo", "Delta", "Charlie", "Bravo", "Alpha"}));
	TEST_EXPECT(list.lines_made() == 1 && list.rows_made() == 5);
	Diagnostic error;
	const auto row_at = [&](size_t i) { return NodeAddress{items.rows()[i]->id, items.rows()[i]->kind, 0}; };
	Edit rename;
	rename.address = row_at(1);
	rename.field = "display_name";
	rename.value = std::string("Delta Two");
	TEST_EXPECT(items.apply(rename, error));
	TEST_EXPECT(texts(list.lines(items)) == std::vector<std::string>({"Echo", "Delta Two", "Charlie", "Bravo", "Alpha"}));
	TEST_EXPECT(list.lines_made() == 1 && list.rows_made() == 6);
	// The last row moved first: the lines re-ordered, each row its place, none made again.
	Edit move;
	move.operation = EditOperation::Move;
	move.address = row_at(4);
	move.position = 0;
	TEST_EXPECT(items.apply(move, error));
	const std::vector<OutlineLine> &moved = list.lines(items);
	TEST_EXPECT(texts(moved) == std::vector<std::string>({"Alpha", "Echo", "Delta Two", "Charlie", "Bravo"}));
	TEST_EXPECT(moved[0].index == 0 && moved[4].index == 4 && list.lines_made() == 1 && list.rows_made() == 6);
	// A row added: its line made alone; a row removed: none made.
	Edit add;
	add.operation = EditOperation::Add;
	add.address = {0, items.rows()[0]->kind, 0};
	TEST_EXPECT(items.apply(add, error));
	TEST_EXPECT(list.lines(items).size() == 6 && list.lines_made() == 1 && list.rows_made() == 7);
	Edit remove;
	remove.operation = EditOperation::Remove;
	remove.address = row_at(0);
	TEST_EXPECT(items.apply(remove, error));
	TEST_EXPECT(list.lines(items).size() == 5 && list.lines(items)[0].text == "Echo" && list.lines_made() == 1 &&
	            list.rows_made() == 7);
	// In name order (a new order: made anew), a renamed row's line made alone and sorted again.
	list.set_sort(true);
	TEST_EXPECT(list.lines(items).front().text == "Bravo" && list.lines_made() == 2);
	const size_t sorted_made = list.rows_made();
	rename.address = row_at(0);
	rename.value = std::string("Zulu");
	TEST_EXPECT(items.apply(rename, error));
	const std::vector<OutlineLine> &renamed = list.lines(items);
	TEST_EXPECT(renamed.back().text == "Zulu" && renamed.back().index == 0 && list.lines_made() == 2 &&
	            list.rows_made() == sorted_made + 1);
	// Read again in place (another load: the document cannot say): every row made anew.
	TEST_EXPECT(load(items, bytes_of("begin \"Solo\"\nid 100300\ntype vehicle\nend\n"), "items.def", AssetKind::ItemDefs));
	TEST_EXPECT(texts(list.lines(items)) == std::vector<std::string>({"Solo"}) && list.lines_made() == 3);

	// A tree: a user point's edit makes the model row's lines alone.
	const std::string repo = test_paths_repo_root(__FILE__);
	ModelDocument model;
	TEST_EXPECT(load(model, test_io::read_file(repo + "/fixtures/threedi/synth/armory.3di"), "armory.3di", AssetKind::Model));
	OutlineModel tree(OutlineMode::Tree);
	const std::vector<OutlineLine> first = tree.lines(model);
	tree.set_open(first.front(), true);
	const size_t opened = tree.lines(model).size();
	TEST_EXPECT(model.rows().size() == 2 && tree.lines_made() == 2 && tree.rows_made() == 4);
	const ModelRow *row = model.model_row();
	TEST_EXPECT(row && !row->ids.lists[kModelUserPoints].empty());
	if (!row || row->ids.lists[kModelUserPoints].empty()) return 1;
	Edit point;
	point.address = {row->id, node_kind(ModelKind::UserPoint), row->ids.lists[kModelUserPoints][0].id};
	point.field = "name";
	point.value = std::string("moved");
	TEST_EXPECT(model.apply(point, error));
	TEST_EXPECT(tree.lines(model).size() == opened && tree.lines_made() == 2 && tree.rows_made() == 5);

	// Master and detail: a string's edit makes its section's lines alone.
	opennova::rtxt::File table;
	table.sections = {{"Menu", 2}, {"Help", 1}};
	table.entries = {{"MM_Play", "Play", {}, 0}, {"MM_Quit", "Quit", {}, 0}, {"HLP_Play", "How to play", {}, 1}};
	std::vector<uint8_t> bytes;
	std::string io_error;
	TEST_EXPECT(opennova::rtxt::write(table, bytes, io_error));
	StringsDocument strings;
	TEST_EXPECT(load(strings, bytes, "strings.bin", AssetKind::Strings));
	OutlineModel outline(OutlineMode::MasterDetail);
	const NodeId menu = strings.rows()[0]->id;
	const std::vector<OutlineLine> &detail = outline.lines(strings, menu);
	TEST_EXPECT(detail.size() == 2 && outline.lines_made() == 1 && outline.rows_made() == 2);
	Edit text;
	text.address = detail[1].address;
	text.field = "text";
	text.value = std::string("Leave");
	TEST_EXPECT(strings.apply(text, error));
	TEST_EXPECT(outline.lines(strings, menu).size() == 2 && outline.masters().size() == 2 && outline.lines_made() == 1 &&
	            outline.rows_made() == 3);
	std::printf("test_change_sets passed\n");
	return 0;
}

// The file-wide values are the type's hook's: none without one.
bool two_values(const Document &, OutlineFileValues &out) {
	out.title = "Values";
	out.values = {7, 9};
	out.max = 3;
	return true;
}

int test_file_values() {
	DefCatalogDocument items;
	TEST_EXPECT(load(items, bytes_of("begin \"One\"\nid 100300\ntype vehicle\nend\n"), "items.def", AssetKind::ItemDefs));
	OutlineFileValues out;
	TEST_EXPECT(!OutlineModel(OutlineMode::List).file_values(items, out));
	OutlineModel values(OutlineMode::List, two_values);
	TEST_EXPECT(values.file_values(items, out) && out.values.size() == 2 && out.max == 3);
	// Their heading, closed until opened.
	TEST_EXPECT(!values.values_open());
	values.set_values_open(true);
	TEST_EXPECT(values.values_open());
	std::printf("test_file_values passed\n");
	return 0;
}

// The pool document's five rows: two crates (one weighing nothing), two barrels and a note.
bool load_pool(editor_test::PoolDocument &pool) {
	return load(pool, bytes_of("C alpha 5\nB bravo 3\nC charlie 0\nN delta words\nB echo 7\n"), "pool.txt", AssetKind::Unknown);
}

// The kinds a list or a tree lists (a bit per kind, every one until one is cleared) and the rows a
// type's listed hook leaves out until the "all rows" switch lists them: a row of a kind left out,
// or one the hook leaves out, has no line; either moving makes the lines anew; an edit that makes a
// row listed (a crate given a weight) makes its line through the change set; a reveal of a row
// either hides lists its kind, or all rows, and nothing more than it needs. Master and detail
// lists every row whatever they say.
int test_kinds_and_listed_rows() {
	using editor_test::kPoolBarrel;
	using editor_test::kPoolCrate;
	using editor_test::kPoolNote;
	editor_test::PoolDocument pool;
	TEST_EXPECT(load_pool(pool));
	const uint64_t crate = OutlineModel::kind_bit(pool, kPoolCrate), barrel = OutlineModel::kind_bit(pool, kPoolBarrel),
	               note = OutlineModel::kind_bit(pool, kPoolNote);
	TEST_EXPECT(crate == 1 && barrel == 2 && note == 4 && OutlineModel::kind_bit(pool, 9) == 0);
	for (const OutlineMode mode : {OutlineMode::List, OutlineMode::Tree}) {
		OutlineModel list(mode);
		TEST_EXPECT(list.kinds() == ~uint64_t(0) && !list.all_rows());
		TEST_EXPECT(list.lines(pool).size() == 5 && list.lines_made() == 1);
		// The barrels left out; the same mask again makes nothing.
		list.set_kinds(~barrel);
		const std::vector<OutlineLine> &kept = list.lines(pool);
		TEST_EXPECT(kept.size() == 3 && list.lines_made() == 2);
		for (const OutlineLine &line : kept) TEST_EXPECT(line.address.kind != kPoolBarrel);
		TEST_EXPECT(kept[1].index == 2 && kept[2].index == 3); // each row's place in the file
		list.set_kinds(~barrel);
		list.lines(pool);
		TEST_EXPECT(list.lines_made() == 2);
		// The crates alone, then nothing.
		list.set_kinds(crate);
		TEST_EXPECT(list.lines(pool).size() == 2 && list.lines_made() == 3);
		list.set_kinds(0);
		TEST_EXPECT(list.lines(pool).empty());
	}

	// The listed hook: a crate or a barrel weighing nothing is left out until all rows are listed.
	OutlineModel listed(OutlineMode::List, nullptr, editor_test::pool_row_listed);
	const auto row_at = [&](size_t i) { return NodeAddress{pool.rows()[i]->id, pool.rows()[i]->kind, 0}; };
	TEST_EXPECT(listed.lines(pool).size() == 4 && listed.line_of(row_at(2)) == SIZE_MAX);
	listed.set_all_rows(true);
	TEST_EXPECT(listed.lines(pool).size() == 5 && listed.line_of(row_at(2)) == 2 && listed.lines_made() == 2);
	listed.set_all_rows(false);
	TEST_EXPECT(listed.lines(pool).size() == 4 && listed.lines_made() == 3);
	// The crate given a weight: its row's line made through the change set, nothing anew; undone, gone.
	Edit weigh;
	weigh.address = row_at(2);
	weigh.field = "weight";
	weigh.value = int64_t(4);
	Diagnostic error;
	TEST_EXPECT(pool.apply(weigh, error));
	const size_t rows_made = listed.rows_made();
	TEST_EXPECT(listed.lines(pool).size() == 5 && listed.lines_made() == 3 && listed.rows_made() == rows_made + 1);
	pool.undo();
	TEST_EXPECT(listed.lines(pool).size() == 4 && listed.lines_made() == 3);

	// A reveal wins: a row whose kind is left out has its kind listed (the other kinds as they were),
	// one the hook leaves out has all rows listed; one that shows changes neither.
	OutlineModel revealed(OutlineMode::List, nullptr, editor_test::pool_row_listed);
	revealed.set_kinds(note);
	TEST_EXPECT(revealed.lines(pool).size() == 1);
	TEST_EXPECT(revealed.reveal(pool, {row_at(3)}) == 0 && revealed.kinds() == note && !revealed.all_rows());
	TEST_EXPECT(revealed.reveal(pool, {row_at(1)}) == 0 && revealed.kinds() == (note | barrel) && !revealed.all_rows());
	TEST_EXPECT(revealed.lines(pool).size() == 3);
	TEST_EXPECT(revealed.reveal(pool, {row_at(2)}) == 2 && revealed.kinds() == (note | barrel | crate) &&
	            revealed.all_rows() && revealed.lines(pool).size() == 5);

	// Master and detail lists every row as a master whatever the kinds and the hook say.
	OutlineModel masters(OutlineMode::MasterDetail, nullptr, editor_test::pool_row_listed);
	masters.set_kinds(0);
	masters.lines(pool, 0);
	TEST_EXPECT(masters.masters().size() == 5);
	std::printf("test_kinds_and_listed_rows passed\n");
	return 0;
}

// What a click on a record's line selects (OutlineModel::click): the record alone; with Ctrl the
// record joining or leaving the selection; with Shift every record line from the primary's to it,
// either way round, the clicked one the record and the rest named with it; a Shift click with no
// primary on the lines, or on the primary's own, the record alone; a collection's heading nothing.
// In a list the primary's row stands for a record it holds; under a sort the range is the lines'.
int test_clicks() {
	editor_test::PoolDocument pool;
	TEST_EXPECT(load_pool(pool));
	const auto row_at = [&](size_t i) { return NodeAddress{pool.rows()[i]->id, pool.rows()[i]->kind, 0}; };
	OutlineModel list(OutlineMode::List);
	list.lines(pool);
	OutlineClick click = list.click(1, row_at(0), false, false);
	TEST_EXPECT(click.record == row_at(1) && click.mode == SelectMode::Replace && click.records.empty());
	click = list.click(1, row_at(0), true, false);
	TEST_EXPECT(click.record == row_at(1) && click.mode == SelectMode::Toggle && click.records.empty());
	// Ctrl wins over Shift.
	click = list.click(3, row_at(0), true, true);
	TEST_EXPECT(click.mode == SelectMode::Toggle && click.records.empty());
	click = list.click(3, row_at(1), false, true);
	TEST_EXPECT(click.record == row_at(3) && click.mode == SelectMode::Replace &&
	            click.records == (std::vector<NodeAddress>{row_at(1), row_at(2)}));
	click = list.click(0, row_at(2), false, true);
	TEST_EXPECT(click.record == row_at(0) && click.records == (std::vector<NodeAddress>{row_at(1), row_at(2)}));
	click = list.click(2, row_at(2), false, true);
	TEST_EXPECT(click.record == row_at(2) && click.records.empty());
	click = list.click(2, NodeAddress(), false, true);
	TEST_EXPECT(click.record == row_at(2) && click.records.empty());
	TEST_EXPECT(!list.click(99, row_at(0), false, false).record.row);
	// The primary's line not listed (its kind left out): the record alone.
	list.set_kinds(~OutlineModel::kind_bit(pool, editor_test::kPoolBarrel));
	list.lines(pool);
	click = list.click(2, row_at(1), false, true);
	TEST_EXPECT(click.record == row_at(3) && click.records.empty());
	// And the range is over the lines listed: alpha to delta leaves the barrel between them out.
	click = list.click(2, row_at(0), false, true);
	TEST_EXPECT(click.record == row_at(3) && click.records == (std::vector<NodeAddress>{row_at(0), row_at(2)}));
	// By name: the lines' order (alpha, bravo, charlie, delta, echo is the file's too; another after
	// a rename of alpha to zulu: bravo, charlie, delta, echo, zulu; a list names a row of another
	// kind than the file's own after its kind's label).
	OutlineModel sorted(OutlineMode::List);
	sorted.set_sort(true);
	Edit rename;
	rename.address = row_at(0);
	rename.field = "name";
	rename.value = std::string("zulu");
	Diagnostic error;
	TEST_EXPECT(pool.apply(rename, error));
	TEST_EXPECT(texts(sorted.lines(pool)) ==
	            std::vector<std::string>({"Barrel: bravo", "charlie", "Note: delta", "Barrel: echo", "zulu"}));
	click = sorted.click(4, row_at(3), false, true);
	TEST_EXPECT(click.record == row_at(0) && click.records == (std::vector<NodeAddress>{row_at(3), row_at(4)}));

	// A tree: a collection's heading selects nothing, and a range leaves the headings out; a list's
	// primary that is a record a row holds stands by its row.
	const std::string repo = test_paths_repo_root(__FILE__);
	ModelDocument model;
	TEST_EXPECT(load(model, test_io::read_file(repo + "/fixtures/threedi/synth/armory.3di"), "armory.3di", AssetKind::Model));
	OutlineModel tree(OutlineMode::Tree);
	tree.set_open(tree.lines(model).front(), true);
	const std::vector<OutlineLine> lines = tree.lines(model);
	TEST_EXPECT(lines.size() > 3 && lines[1].collection && !lines.back().collection);
	TEST_EXPECT(!tree.click(1, lines[0].address, false, false).record.row);
	click = tree.click(lines.size() - 1, lines[0].address, false, true);
	TEST_EXPECT(click.record == lines.back().address && click.records == (std::vector<NodeAddress>{lines[0].address}));
	const ModelRow *row = model.model_row();
	TEST_EXPECT(row && !row->ids.lists[kModelUserPoints].empty());
	if (!row || row->ids.lists[kModelUserPoints].empty()) return 1;
	const NodeAddress point{row->id, node_kind(ModelKind::UserPoint), row->ids.lists[kModelUserPoints][0].id};
	OutlineModel rows(OutlineMode::List);
	const std::vector<OutlineLine> flat = rows.lines(model);
	TEST_EXPECT(flat.size() == 2 && flat[0].address.row == row->id);
	click = rows.click(1, point, false, true);
	TEST_EXPECT(click.record == flat[1].address && click.records == (std::vector<NodeAddress>{flat[0].address}));
	std::printf("test_clicks passed\n");
	return 0;
}

// The Inspector's form over several records (ui/inspector_layout): kinds are alike when their fields
// are (the same ids and types in the same order: the pool's crates and barrels, two lists apart in
// storage; never a note's), the shared plan spans records of kinds alike (each field every one has)
// and is empty of a field a record of a kind not alike lacks, and the heading counts the records by
// kind.
int test_kinds_alike() {
	using editor_test::kPoolBarrel;
	using editor_test::kPoolCrate;
	using editor_test::kPoolNote;
	editor_test::PoolDocument pool;
	TEST_EXPECT(load_pool(pool));
	const auto row_at = [&](size_t i) { return NodeAddress{pool.rows()[i]->id, pool.rows()[i]->kind, 0}; };
	TEST_EXPECT(&pool.fields(kPoolCrate) != &pool.fields(kPoolBarrel));
	TEST_EXPECT(kinds_alike(pool, kPoolCrate, kPoolCrate) && kinds_alike(pool, kPoolCrate, kPoolBarrel) &&
	            kinds_alike(pool, kPoolBarrel, kPoolCrate));
	TEST_EXPECT(!kinds_alike(pool, kPoolCrate, kPoolNote) && !kinds_alike(pool, kPoolNote, kPoolBarrel));
	const auto ids = [](const std::vector<InspectorSection> &plan) {
		std::vector<std::string> out;
		for (const InspectorSection &section : plan)
			for (const FieldUse &field : section.fields) out.push_back(field.schema->id);
		return out;
	};
	// A crate and two barrels: both fields, each as the primary's kind declares it.
	const std::vector<NodeAddress> weighed = {row_at(0), row_at(1), row_at(4)};
	const std::vector<InspectorSection> shared = plan_shared_inspector(pool, weighed, "");
	TEST_EXPECT(ids(shared) == std::vector<std::string>({"name", "weight"}));
	TEST_EXPECT(shared.front().fields[1].schema == &pool.fields(kPoolCrate)[1]);
	TEST_EXPECT(field_mixed(pool, weighed, "weight") && field_mixed(pool, weighed, "name"));
	TEST_EXPECT(ids(plan_shared_inspector(pool, weighed, "weigh")) == std::vector<std::string>({"weight"}));
	// With a note among them nothing is shared (the window shows the primary's own form instead).
	TEST_EXPECT(plan_shared_inspector(pool, {row_at(0), row_at(3)}, "").empty());
	TEST_EXPECT(selected_words(pool, {row_at(0), row_at(2)}) == "2 Crate records selected");
	TEST_EXPECT(selected_words(pool, weighed) == "3 records selected (1 Crate, 2 Barrel)");
	TEST_EXPECT(selected_words(pool, {row_at(4), row_at(3), row_at(0), row_at(1)}) ==
	            "4 records selected (2 Barrel, 1 Note, 1 Crate)");
	std::printf("test_kinds_alike passed\n");
	return 0;
}

// A source of names standing in for the graph: the fixture's catalog's three items (ADR 0046 S15).
class ItemNames : public NameSource {
public:
	ItemNames() {
		for (const auto &[id, name] : std::vector<std::pair<const char *, const char *>>{
		             {"106100", "Wire Test Pump"}, {"106101", "Wire Test Armory"}, {"106102", "Wire Test Rifleman"}}) {
			GraphSymbol symbol;
			symbol.kind = ReferenceKind::Item;
			symbol.name = symbol.display = id;
			symbol.record = name;
			symbol.file = "items.def";
			items_.push_back(symbol);
		}
	}
	const GraphSymbol *symbol(ReferenceKind kind, const std::string &name, const std::string &) const override {
		for (const GraphSymbol &symbol : items_)
			if (symbol.kind == kind && symbol.name == name) return &symbol;
		return nullptr;
	}
	const GraphSymbol *reached(const GraphEdge &) const override { return nullptr; }
	uint64_t generation() const override { return generation_; }
	uint64_t generation_ = 7;

private:
	std::vector<GraphSymbol> items_;
};

// ADR 0046 S15: the minted mission as a tree under the mission's headings (mission_row_headings): the
// mission row first under none, then each pool's heading with its count over its rows, the organics'
// two groups (their one team tells nothing apart) each a heading of its own; the rows titled with the
// names given (the lines made anew when the names' generation moves); a heading closed hides what
// stands under it and a click range leaves the headings out; a reveal of a row under a closed heading
// opens it; a filter by a record's words keeps it and the headings over it, held open.
int test_mission_headings() {
	const DocumentType *type = document_type_for(AssetKind::Mission);
	std::unique_ptr<Document> mission = type ? records_of(type->make()) : nullptr;
	Diagnostic error;
	const std::string repo = test_paths_repo_root(__FILE__);
	TEST_EXPECT(mission && mission->load_bytes(test_io::read_file(repo + "/fixtures/bms/synth_logic.bms"), "synth_logic.bms",
	                                           AssetKind::Mission, "jo", error));
	if (!mission) return 1;
	OutlineModel tree(OutlineMode::Tree, nullptr, nullptr, mission_row_headings);
	ItemNames names;
	const std::vector<OutlineLine> &lines = tree.lines(*mission, 0, &names);
	const auto line_named = [&](const std::string &text) {
		for (size_t i = 0; i < lines.size(); ++i)
			if (lines[i].text == text) return i;
		return SIZE_MAX;
	};
	TEST_EXPECT(!lines.empty() && !lines[0].heading && lines[0].address.kind == node_kind(MissionKind::Mission));
	const size_t items = line_named("Items (3)"), organics = line_named("Organics (2)");
	TEST_EXPECT(items == 1 && lines[items].heading && lines[items].depth == 0 && lines[items].open);
	TEST_EXPECT(line_named("Buildings (2)") != SIZE_MAX && line_named("Markers (5)") != SIZE_MAX &&
	            line_named("Area triggers (2)") != SIZE_MAX && line_named("Events (2)") != SIZE_MAX &&
	            line_named("Waypoint paths (128)") != SIZE_MAX);
	// The organics: one team, two groups, each its heading one deeper, its row under it.
	TEST_EXPECT(organics != SIZE_MAX && lines[organics + 1].heading && lines[organics + 1].text == "Group 1 (1)" &&
	            lines[organics + 1].depth == 1 && !lines[organics + 2].heading && lines[organics + 2].depth == 2);
	const NodeAddress walker = lines[organics + 2].address;
	const int32_t ssn = static_cast<const EntityRow *>(mission->row(walker.row))->native.id;
	TEST_EXPECT(lines[organics + 2].text == "Wire Test Rifleman #" + std::to_string(ssn) && lines[organics + 2].name == std::to_string(ssn));
	// The names' generation moved: the lines made anew; none given: the document's own words.
	const size_t made = tree.lines_made();
	names.generation_ = 8;
	tree.lines(*mission, 0, &names);
	TEST_EXPECT(tree.lines_made() == made + 1);
	TEST_EXPECT(tree.lines(*mission).size() == lines.size() &&
	            tree.lines(*mission)[organics + 2].text == "Organic #" + std::to_string(ssn));
	// Closed, the organics' heading hides their groups and rows; a click on a heading selects nothing.
	tree.lines(*mission, 0, &names);
	const size_t before = tree.lines(*mission, 0, &names).size();
	tree.set_open(tree.lines(*mission, 0, &names)[organics], false);
	const std::vector<OutlineLine> &closed = tree.lines(*mission, 0, &names);
	TEST_EXPECT(closed.size() == before - 4 && closed[organics].heading && !closed[organics].open);
	TEST_EXPECT(!tree.click(organics, NodeAddress(), false, false).record.row);
	// A reveal of the walker opens it.
	TEST_EXPECT(tree.reveal(*mission, {walker}, 0, &names) != SIZE_MAX && tree.lines(*mission, 0, &names).size() == before);
	// A filter by the walker's item's name keeps it and the headings over it, held open (and the event
	// whose trigger names it: its words name it too); no pool that holds none.
	tree.set_filter("rifleman");
	const std::vector<OutlineLine> &found = tree.lines(*mission, 0, &names);
	bool walker_kept = false, items_kept = false;
	for (const OutlineLine &line : found) {
		walker_kept = walker_kept || (!line.heading && line.address == walker);
		items_kept = items_kept || (line.heading && line.text.rfind("Items", 0) == 0);
	}
	TEST_EXPECT(walker_kept && !items_kept && !found.empty() && found[0].heading && found[0].forced &&
	            found[0].text == "Organics (2)");
	std::printf("test_mission_headings passed\n");
	return 0;
}

} // namespace

int main() {
	int failures = 0;
	failures += test_mission_headings();
	failures += test_kinds_and_listed_rows();
	failures += test_clicks();
	failures += test_kinds_alike();
	failures += test_list();
	failures += test_tree();
	failures += test_master_detail();
	failures += test_instances();
	failures += test_change_sets();
	failures += test_file_values();
	return failures ? 1 : 0;
}
