// S13 V3 (ADR 0046 S13): a document's outline model (ui/outline_model), with no ImGui, in its three
// modes. A list (a catalog): the rows, a row of another kind than the file's own after its kind's
// label (a weapon table's carry limit), filtered by name in any case, sorted by name as a display
// order that keeps each row's place in the file. A tree (a model): the rows first, a record's
// collections under it once it is open and a collection's records once that is, closed again with
// what they hold; a reveal opening what holds a nested record, which is then listed; a filter keeping
// the records that match with what holds them, open, and leaving what is open otherwise as it was.
// Master and detail (a string table): every row as a master; the master row's strings as the
// detail, its columns the kind's text fields; a filter over them; every row's matches with Every; a
// text over several lines counted (eight at most). The lines are made again only when what they
// read moves (lines_made): the document's revision, what is open, the filter, the order, Every, the
// master row; and the file-wide values come from the hook a type gives.
#include <cstdio>
#include <string>
#include <vector>

#include <editor/documents/def_catalog_document.h>
#include <editor/documents/model_document.h>
#include <editor/documents/strings_document.h>
#include <editor/ui/outline_model.h>
#include <formats/rtxt/rtxt.h>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"

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
	// An edit (the document's revision) makes them again: the renamed row matches no more.
	Edit rename;
	rename.address = {items.rows()[0]->id, items.rows()[0]->kind, 0};
	rename.field = "display_name";
	rename.value = std::string("Zulu Car");
	Diagnostic error;
	TEST_EXPECT(items.apply(rename, error));
	TEST_EXPECT(list.lines(items).empty() && list.lines_made() == 4);
	items.undo();
	TEST_EXPECT(list.lines(items).size() == 1 && list.lines_made() == 5);

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
	// listed; revealing it again opens nothing more.
	OutlineModel fresh(OutlineMode::Tree);
	const NodeAddress last{nested.address.row, nested.address.kind, held[at - 1].ids.back()};
	std::vector<NodeAddress> path = model.ancestors(last);
	path.push_back(last);
	TEST_EXPECT(fresh.lines(model).size() == first.size() && fresh.line_of(last) == SIZE_MAX);
	TEST_EXPECT(fresh.reveal(path));
	fresh.lines(model); // made again: line_of reads the lines as last made
	const size_t line = fresh.line_of(last);
	TEST_EXPECT(line != SIZE_MAX && fresh.lines(model)[line].depth == int(path.size()) * 2 - 2);
	TEST_EXPECT(!fresh.reveal(path));

	// A filter keeps a record whose title holds it and what holds it, open, whatever is open; the
	// filter gone, what was open is listed again as it was.
	const std::string title = nested.text;
	OutlineModel filtered(OutlineMode::Tree);
	filtered.set_filter(title);
	const std::vector<OutlineLine> found = filtered.lines(model);
	bool reached = false;
	for (const OutlineLine &kept : found) reached = reached || (!kept.collection && kept.address == nested.address);
	TEST_EXPECT(reached && found.front().open);
	for (const OutlineLine &kept : found)
		if (kept.collection || kept.address != nested.address)
			TEST_EXPECT(kept.open); // every other line holds a match
	filtered.set_filter("");
	TEST_EXPECT(filtered.lines(model).size() == first.size());
	filtered.set_filter("NO_SUCH_RECORD_NAME");
	TEST_EXPECT(filtered.lines(model).empty());
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
	const std::vector<OutlineLine> every = outline.lines(strings, menu);
	TEST_EXPECT(every.size() == 2 && every[0].row_name == "Menu" && every[1].row_name == "Help");
	// Every with no filter: the master row's alone.
	outline.set_filter("");
	TEST_EXPECT(outline.lines(strings, menu).size() == 3);
	const size_t made = outline.lines_made();
	outline.lines(strings, menu);
	TEST_EXPECT(outline.lines_made() == made);
	TEST_EXPECT(outline.line_of(lines[1].address) == 1);
	std::printf("test_master_detail passed\n");
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
	TEST_EXPECT(OutlineModel(OutlineMode::List, two_values).file_values(items, out) && out.values.size() == 2 &&
	            out.max == 3);
	std::printf("test_file_values passed\n");
	return 0;
}

} // namespace

int main() {
	int failures = 0;
	failures += test_list();
	failures += test_tree();
	failures += test_master_detail();
	failures += test_file_values();
	return failures ? 1 : 0;
}
