// The items.def ids the engine fixes, as the catalog keeps them (ADR 0046 S19, "Reserved item ids";
// formats/def/reserved_items.h; docs/world/itemdef-re.md, "The ids and rows the engine fixes"): a new
// item and a copy never take a reserved id; an edit moving a place or an objective off its id, or
// giving its id to another kind, is refused with catalog.reserved_refused, through the document and
// through the session's edit_record, while every other field stays free; the findings (what the engine
// keeps an id for, another kind on it, an item named as one under another id, the first row) and
// their fixes; the engine's rows added on their ids and of their kinds.
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include <editor/blank/blank_factory.h>
#include <editor/documents/catalog_validation.h>
#include <editor/documents/def_catalog_document.h>
#include <editor/graph/asset_graph.h>
#include <editor/session/preferences_store.h>
#include <editor/session/problem_fixes.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/def/reserved_items.h>
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;
using namespace opennova::def;

namespace {

using editor_test::NoProcess;

const DefItemDef &item_at(const Document &document, size_t index) {
	return static_cast<const CatalogRow &>(*document.rows()[index]).native.as<DefItemDef>();
}

NodeAddress row_address(const Document &document, size_t index) {
	return {document.rows()[index]->id, node_kind(DefRecordKind::Item), 0};
}

Edit set(NodeAddress address, const std::string &field, Value value) {
	Edit edit;
	edit.address = address;
	edit.field = field;
	edit.value = std::move(value);
	return edit;
}

std::string item(const char *name, int id, const char *type) {
	return std::string("begin \"") + name + "\"\nid " + std::to_string(id) + "\ntype " + type + "\nend\n";
}

bool load(DefCatalogDocument &document, editor_test::TempProjectDir &dir, const std::string &text) {
	Diagnostic error;
	return editor_test::write_text(dir.file("items.def"), text) &&
	       document.load(dir.file("items.def"), "items.def", AssetKind::ItemDefs, "jo", error);
}

const Diagnostic *finding(const std::vector<Diagnostic> &findings, const std::string &code, const std::string &record) {
	for (const Diagnostic &d : findings)
		if (d.code() == code && d.record == record) return &d;
	return nullptr;
}

size_t count_of(const std::vector<Diagnostic> &findings, const std::string &code) {
	return size_t(std::count_if(findings.begin(), findings.end(), [&](const Diagnostic &d) { return d.code() == code; }));
}

// A new item and a copy take the first free id the engine keeps for nothing: 100185, the parachute's,
// is passed over; so is every reserved id the picker meets.
int test_free_ids() {
	editor_test::TempProjectDir dir("opennova_reserved_free_ids");
	std::string text;
	for (int id = 100000; id < 100185; ++id) text += item(("R" + std::to_string(id)).c_str(), id, "marker");
	DefCatalogDocument document;
	TEST_EXPECT(load(document, dir, text));
	Diagnostic error;
	Edit add;
	add.operation = EditOperation::Add;
	add.address.kind = node_kind(DefRecordKind::Item);
	TEST_EXPECT(document.apply(add, error));
	TEST_EXPECT(item_at(document, 185).id == 100186);
	Edit duplicate;
	duplicate.operation = EditOperation::Duplicate;
	duplicate.address = row_address(document, 0);
	TEST_EXPECT(document.apply(duplicate, error));
	TEST_EXPECT(item_at(document, 1).id == 100187);
	// The picker over the whole table: no reserved id is ever handed out.
	int last = 0;
	for (int i = 0; i < 7000; ++i) {
		const int id = free_item_id([&](int candidate) { return candidate <= last; });
		TEST_EXPECT(id > last && !reserved_item_by_id(id));
		last = id;
	}
	TEST_EXPECT(last > 106099);
	// The blank items.def is the Null marker alone: the row retail's first is, and no finding of its own.
	std::vector<uint8_t> bytes;
	BlankRequest blank;
	blank.logical_name = "items.def";
	TEST_EXPECT(make_blank(blank, AssetKind::ItemDefs, bytes, error));
	TEST_EXPECT(editor_test::write_bytes(dir.file("items.def"), bytes));
	DefCatalogDocument made;
	TEST_EXPECT(made.load(dir.file("items.def"), "items.def", AssetKind::ItemDefs, "jo", error));
	TEST_EXPECT(made.rows().size() == 1 && item_at(made, 0).id == 100000 && item_at(made, 0).type == DEF_ITEM_TYPE_MARKER);
	TEST_EXPECT(validate_catalog_file(made).empty());
	return 0;
}

// The refusals through the document: off its id, another kind onto it, its kind changed; every other
// field free, an id the engine keeps for a model free too (a warning), and a mismatch the file already
// had left editable.
int test_refusals() {
	editor_test::TempProjectDir dir("opennova_reserved_refusals");
	DefCatalogDocument document;
	TEST_EXPECT(load(document, dir,
	                 item("Null", 100000, "marker") + item("Insertion point", 106094, "marker") +
	                         item("Crate", 100010, "decoration") + item("Med pack", 102044, "powerup") +
	                         item("Pilot", 100020, "person")));
	const NodeAddress insertion = row_address(document, 1), crate = row_address(document, 2), pack = row_address(document, 3),
	                  pilot = row_address(document, 4);
	Diagnostic error;
	// Off its id.
	TEST_EXPECT(!document.apply(set(insertion, "id", int64_t(100500)), error));
	TEST_EXPECT(error.code() == "catalog.reserved_refused" && error.field == "id" && error.row_id == insertion.row &&
	            error.record == "Insertion point" && error.severity == DiagnosticSeverity::Error);
	TEST_EXPECT(error.message.find("106094") != std::string::npos && error.message.find("Insertion point") != std::string::npos);
	TEST_EXPECT(item_at(document, 1).id == 106094 && !document.dirty());
	// Its kind changed.
	TEST_EXPECT(!document.apply(set(insertion, "type", int64_t(DEF_ITEM_TYPE_DECORATION)), error));
	TEST_EXPECT(error.code() == "catalog.reserved_refused" && error.field == "type");
	TEST_EXPECT(item_at(document, 1).type == DEF_ITEM_TYPE_MARKER);
	// Another kind onto a start's id.
	TEST_EXPECT(!document.apply(set(crate, "id", int64_t(106001)), error));
	TEST_EXPECT(error.code() == "catalog.reserved_refused" && error.field == "id" && error.row_id == crate.row);
	TEST_EXPECT(error.message.find("decoration") != std::string::npos);
	// In one batch, the id and the kind set together: the step as a whole is what is checked.
	TEST_EXPECT(document.apply(std::vector<Edit>{set(crate, "type", int64_t(DEF_ITEM_TYPE_MARKER)), set(crate, "id", int64_t(106001))},
	                           error));
	TEST_EXPECT(item_at(document, 2).id == 106001);
	document.undo();
	// Every other field of a reserved item is free.
	TEST_EXPECT(document.apply(set(insertion, "display_name", std::string("Start")), error));
	TEST_EXPECT(document.apply(set(insertion, "hp", int64_t(5)), error));
	// An id the engine keeps for a model or an actor is a warning, not a refusal.
	TEST_EXPECT(document.apply(set(crate, "id", int64_t(100185)), error) && item_at(document, 2).id == 100185);
	TEST_EXPECT(document.apply(set(pilot, "id", int64_t(101281)), error) && item_at(document, 4).id == 101281);
	// A mismatch the file had (retail's own 102044 powerup) stays editable, and may stay mismatched.
	TEST_EXPECT(document.apply(set(pack, "hp", int64_t(50)), error));
	TEST_EXPECT(document.apply(set(pack, "type", int64_t(DEF_ITEM_TYPE_PERSON)), error));
	// A copy of a reserved item takes an id of its own.
	Edit duplicate;
	duplicate.operation = EditOperation::Duplicate;
	duplicate.address = insertion;
	TEST_EXPECT(document.apply(duplicate, error));
	TEST_EXPECT(item_at(document, 2).id == 100001 && !reserved_item_by_id(item_at(document, 2).id));
	return 0;
}

// The engine's rows added on their ids and of their kinds (the outline's Add engine item...): one undo
// step each; what the engine keeps the id for then said on the row.
int test_add_engine_items() {
	editor_test::TempProjectDir dir("opennova_reserved_add");
	DefCatalogDocument document;
	TEST_EXPECT(load(document, dir, item("Null", 100000, "marker")));
	Diagnostic error;
	TEST_EXPECT(document.apply(reserved_item_add_edits(*reserved_item_by_type(6094)), error));
	TEST_EXPECT(document.rows().size() == 2 && item_at(document, 1).id == 106094 &&
	            item_at(document, 1).type == DEF_ITEM_TYPE_MARKER && document.rows()[1]->name() == "Insertion point");
	TEST_EXPECT(document.apply(reserved_item_add_edits(*reserved_item_by_type(4091)), error));
	TEST_EXPECT(item_at(document, 2).id == 104091 && item_at(document, 2).type == DEF_ITEM_TYPE_OBJECT);
	TEST_EXPECT(document.apply(reserved_item_add_edits(*reserved_item_by_type(5305)), error));
	TEST_EXPECT(item_at(document, 3).id == 105305 && item_at(document, 3).type == DEF_ITEM_TYPE_PERSON);
	document.undo();
	TEST_EXPECT(document.rows().size() == 3);
	const std::vector<Diagnostic> findings = validate_catalog_file(document);
	TEST_EXPECT(findings.size() == 2 && count_of(findings, "catalog.reserved_id") == 2);
	const Diagnostic *said = finding(findings, "catalog.reserved_id", "Insertion point");
	TEST_EXPECT(said && said->severity == DiagnosticSeverity::Info && said->field == "id" &&
	            said->message.find("spawns the player") != std::string::npos);
	return 0;
}

// The findings over one file: what the engine keeps an id for (Info), another kind on a place's id (an
// error) or on an actor's (a warning), an item named as a place under another id the file lacks, and
// the first row; a name only the editor's words for an unshipped row give, and one whose id the file
// has, are none.
int test_findings() {
	editor_test::TempProjectDir dir("opennova_reserved_findings");
	DefCatalogDocument document;
	TEST_EXPECT(load(document, dir,
	                 item("Crate", 100010, "decoration") + item("Insertion point", 106094, "marker") +
	                         item("Truck", 106001, "vehicle") + item("Medic", 104520, "vehicle") +
	                         item("Canopy", 100185, "object") + item("Teleport target", 100060, "marker") +
	                         item("Flag", 100070, "decoration") + item("start, primary, player", 100080, "marker")));
	const std::vector<Diagnostic> findings = validate_catalog_file(document);
	const Diagnostic *first = finding(findings, "catalog.first_row", "Crate");
	TEST_EXPECT(first && first->severity == DiagnosticSeverity::Warning && first->field == "type" &&
	            first->message.find("decoration") != std::string::npos);
	TEST_EXPECT(finding(findings, "catalog.reserved_id", "Insertion point") && finding(findings, "catalog.reserved_id", "Canopy"));
	const Diagnostic *truck = finding(findings, "catalog.reserved_kind", "Truck");
	TEST_EXPECT(truck && truck->severity == DiagnosticSeverity::Error && truck->field == "id" &&
	            truck->message.find("a vehicle") != std::string::npos);
	TEST_EXPECT(truck->row() && !truck->row()->gates_build && truck->row()->fixes == FindingFix::ItemId);
	const Diagnostic *medic = finding(findings, "catalog.reserved_kind", "Medic");
	TEST_EXPECT(medic && medic->severity == DiagnosticSeverity::Warning);
	const Diagnostic *named = finding(findings, "catalog.reserved_name", "Teleport target");
	TEST_EXPECT(named && named->severity == DiagnosticSeverity::Warning && named->message.find("106088") != std::string::npos);
	TEST_EXPECT(!finding(findings, "catalog.reserved_name", "Flag") &&
	            !finding(findings, "catalog.reserved_name", "start, primary, player"));
	TEST_EXPECT(findings.size() == 6);
	// A first row that is a marker the engine keeps (every id it finds no item of becomes one) is said
	// too; retail's "Null" marker first is not.
	DefCatalogDocument starts;
	TEST_EXPECT(load(starts, dir, item("Insertion point", 106094, "marker") + item("Null", 100000, "marker")));
	const std::vector<Diagnostic> of_starts = validate_catalog_file(starts);
	const Diagnostic *reserved_first = finding(of_starts, "catalog.first_row", "Insertion point");
	TEST_EXPECT(reserved_first && reserved_first->field == "id" &&
	            reserved_first->message.find("the Insertion point, a marker (id 106094)") != std::string::npos);
	DefCatalogDocument retail_like;
	TEST_EXPECT(load(retail_like, dir, item("Null", 100000, "marker") + item("start, player", 106001, "marker")));
	TEST_EXPECT(count_of(validate_catalog_file(retail_like), "catalog.first_row") == 0);
	return 0;
}

const Diagnostic *view_finding(const SessionView &view, const std::string &code, const std::string &record) {
	for (const Diagnostic &d : view.findings.diagnostics)
		if (d.code() == code && d.record == record) return &d;
	return nullptr;
}

// The session: a refusal through edit_record, said in its outcome; the fixes Problems offers (Use an id
// of its own, Use the engine's id, each a Rename everywhere carrying the mission's records with it; Add a
// Null marker first) and what each does.
int test_session() {
	editor_test::TempProjectDir dir("opennova_reserved_session");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Reserved"));
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	const AssetEntry *entry = view.project.scan->find("items.def");
	TEST_EXPECT(entry != nullptr);
	if (!entry) return 1;
	const std::string items = entry->relative_path, root = view.project.root;
	TEST_EXPECT(editor_test::write_text(root + "/" + items,
	                                    item("Null", 100000, "marker") + item("Truck", 106001, "vehicle") +
	                                            item("Teleport target", 100060, "marker") +
	                                            item("Insertion point", 106094, "marker")));
	{
		opennova::bms::File mission;
		opennova::mission::make_default(mission);
		opennova::mission::add_entity(mission, opennova::mission::EntityKind::Item, 106001, {});
		opennova::mission::add_entity(mission, opennova::mission::EntityKind::Marker, 100060, {});
		std::vector<uint8_t> bytes;
		std::string error;
		TEST_EXPECT(opennova::bms::write(mission, bytes, error));
		TEST_EXPECT(editor_test::write_bytes(root + "/missions/place.bms", bytes));
	}
	editor_test::handle_to_end(session, request::rescan());

	// The refusal through edit_record: the outcome says why, with the code and the field.
	editor_test::handle_to_end(session, request::open_document(items));
	Document *document = session.document_for(items);
	TEST_EXPECT(document && document->rows().size() == 4);
	if (!document || document->rows().size() != 4) return 1;
	const ActionOutcome refused =
	        editor_test::handle_to_end(session, request::edit_record(items, set(row_address(*document, 3), "id", int64_t(100700))));
	TEST_EXPECT(refused.refused && !refused.findings.empty() && refused.findings.front().code() == "catalog.reserved_refused" &&
	            refused.findings.front().field == "id");
	TEST_EXPECT(item_at(*document, 3).id == 106094 && !document->dirty());
	TEST_EXPECT(editor_test::handle_to_end(session, request::edit_record(items, set(row_address(*document, 3), "hp", int64_t(9)))).done());
	TEST_EXPECT(editor_test::handle_to_end(session, request::save(items)).done());
	editor_test::handle_to_end(session, request::close_document(items));

	// Use an id of its own: the truck off the start's id, the mission's entity with it.
	const Diagnostic *truck = view_finding(view, "catalog.reserved_kind", "Truck");
	TEST_EXPECT(truck != nullptr);
	if (!truck) return 1;
	std::vector<ProblemFix> fixes = fixes_for(*truck, view);
	TEST_EXPECT(fixes.size() == 1 && fixes[0].label == "Use id 100001" && !fixes[0].bulk &&
	            fixes[0].request.kind == EditorRequestKind::RenameSymbol && fixes[0].request.new_name == "100001" &&
	            fixes[0].detail.find("2 uses") != std::string::npos);
	if (fixes.size() != 1) return 1;
	TEST_EXPECT(editor_test::handle_to_end(session, fixes[0].request).done());
	const AssetGraph &graph = *view.findings.graph;
	TEST_EXPECT(graph.resolve_symbol(ReferenceKind::Item, "100001") && !graph.resolve_symbol(ReferenceKind::Item, "106001"));
	TEST_EXPECT(!view_finding(view, "catalog.reserved_kind", "Truck"));
	// Use the engine's id: the teleport target onto 106088, the mission's marker with it.
	const Diagnostic *named = view_finding(view, "catalog.reserved_name", "Teleport target");
	TEST_EXPECT(named != nullptr);
	if (!named) return 1;
	fixes = fixes_for(*named, view);
	TEST_EXPECT(fixes.size() == 1 && fixes[0].label == "Use id 106088" && fixes[0].request.new_name == "106088");
	if (fixes.size() != 1) return 1;
	TEST_EXPECT(editor_test::handle_to_end(session, fixes[0].request).done());
	bool placed = false;
	view.findings.graph->for_each_edge([&](const GraphEdge &edge) {
		placed = placed || (edge.kind == ReferenceKind::Item && edge.value == "106088" && edge.source == "missions/place.bms");
	});
	TEST_EXPECT(placed && view_finding(view, "catalog.reserved_id", "Teleport target"));
	TEST_EXPECT(!view_finding(view, "catalog.reserved_name", "Teleport target"));

	// The first row: a decoration first, a Null marker added before it.
	TEST_EXPECT(editor_test::write_text(root + "/" + items, item("Crate", 100010, "decoration") + item("Insertion point", 106094, "marker")));
	editor_test::handle_to_end(session, request::rescan());
	const Diagnostic *first = view_finding(view, "catalog.first_row", "Crate");
	TEST_EXPECT(first != nullptr);
	if (!first) return 1;
	fixes = fixes_for(*first, view);
	TEST_EXPECT(fixes.size() == 1 && fixes[0].label == "Add a Null marker first" &&
	            fixes[0].request.kind == EditorRequestKind::EditRecord);
	if (fixes.size() != 1) return 1;
	TEST_EXPECT(editor_test::handle_to_end(session, fixes[0].request).done());
	document = session.document_for(items);
	TEST_EXPECT(document && document->rows().size() == 3 && document->rows()[0]->name() == "Null" &&
	            item_at(*document, 0).type == DEF_ITEM_TYPE_MARKER && !reserved_item_by_id(item_at(*document, 0).id));
	return 0;
}

} // namespace

int main() {
	int failures = 0;
	failures += test_free_ids();
	failures += test_refusals();
	failures += test_add_engine_items();
	failures += test_findings();
	failures += test_session();
	if (failures == 0) std::printf("editor_reserved_items: ok\n");
	return failures == 0 ? 0 : 1;
}
