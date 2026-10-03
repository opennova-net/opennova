// The display-name service (ADR 0046 S15, Names) over a mission: documents/mission_labels.h (the
// mission's words for its records and values), graph/display_names.h (the dispatch, the graph as the
// names, the pickers' words, the cache). Over the minted fixtures/bms/synth_logic.bms: the document's
// own words without names (an entity by its pool and SSN, a path by its stops, an area by its zone, an
// event by its first trigger, a stop by its marker); with a table of names (an entity by its item's
// name and the name the game shows for it, an item id by its catalog's name, an SSN, a zone, an event,
// a group, a path, a stop, a text key by its string; every value naming nothing said in words); through
// a session over a project holding the catalog and the mission's table (the graph's names: the
// outline's titles, the picker's choices worded, the wire's `display`, Problems' places, the status
// line). Its retail leg words every record and every reference of the install's 115 missions with the
// install's catalog and tables, and times it.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <base/io/json.h>
#include <base/io/strutil.h>
#include <base/vfs/vfs.h>
#include <base/vfs/vfs_decode.h>
#include <editor/assets/asset_kinds.h>
#include <editor/documents/document_types.h>
#include <editor/documents/mission_document.h>
#include <editor/documents/mission_labels.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/display_names.h>
#include <editor/graph/reference_queries.h>
#include <editor/model/field_text.h>
#include <editor/session/preferences_store.h>
#include <editor/session/problem_query.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/session_json.h>
#include <editor/session/view/session_view.h>
#include <formats/mission/bms.h>

#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;
namespace bms = opennova::bms;

namespace {

constexpr NodeKind k(MissionKind kind) { return node_kind(kind); }

std::string repo() { return test_paths_repo_root(__FILE__); }

std::unique_ptr<Document> open(const std::vector<uint8_t> &bytes, const char *name = "synth_logic.bms") {
	const DocumentType *type = document_type_for(AssetKind::Mission);
	if (!type) return nullptr;
	std::unique_ptr<Document> document = records_of(type->make());
	Diagnostic error;
	if (!document || !document->load_bytes(bytes, name, AssetKind::Mission, "jo", error)) {
		std::fprintf(stderr, "%s does not load: %s\n", name, error.message.c_str());
		return nullptr;
	}
	return document;
}

const MissionDocument &as_mission(const Document &document) { return dynamic_cast<const MissionDocument &>(document); }

NodeAddress row_at(const Document &document, MissionKind kind, size_t index) {
	const std::vector<const Node *> rows = as_mission(document).rows_of(kind);
	return index < rows.size() ? NodeAddress{rows[index]->id, rows[index]->kind, 0} : NodeAddress();
}

// The first record of `kind` a row holds.
NodeAddress first_child(const Document &document, const NodeAddress &row, MissionKind kind) {
	for (const Document::Collection &collection : document.collections_of(row))
		if (collection.spec.kind == k(kind) && !collection.ids.empty()) return {row.row, k(kind), collection.ids[0]};
	return NodeAddress();
}

// The words against those wanted, the two printed where they differ.
bool same(const std::string &got, const std::string &want) {
	if (got != want) std::printf("  got:  %s\n  want: %s\n", got.c_str(), want.c_str());
	return got == want;
}

int32_t ssn_of(const Document &document, const NodeAddress &row) {
	return static_cast<const EntityRow *>(document.row(row.row))->native.id;
}

DisplayName shown(const Document &document, const NodeAddress &address, const std::string &id, const NameSource *names) {
	for (const FieldSchema &schema : document.fields(address.kind)) {
		if (schema.id != id) continue;
		Value value;
		if (!document.get(address, id, value)) break;
		return value_display(document, address, document.field_on(address, schema), value, names);
	}
	return DisplayName();
}

// A table of names standing in for the graph: definitions by kind and name, a text key found in its
// edge's scope's table, else its alternate's (as the graph's lookup tries them).
class TableNames : public NameSource {
public:
	void define(ReferenceKind kind, const std::string &name, const std::string &record, const std::string &value,
	            const std::string &scope = std::string(), const std::string &file = "defs/items.def") {
		GraphSymbol symbol;
		symbol.kind = kind;
		symbol.name = symbol.display = name;
		symbol.record = record;
		symbol.value = value;
		symbol.scope = scope;
		symbol.file = file;
		symbols_.push_back(symbol);
	}
	const GraphSymbol *symbol(ReferenceKind kind, const std::string &name, const std::string &scope) const override {
		for (const GraphSymbol &symbol : symbols_)
			if (symbol.kind == kind && opennova::strutil::iequals(symbol.name, name) &&
			    (scope.empty() || opennova::strutil::iequals(symbol.scope, scope)))
				return &symbol;
		return nullptr;
	}
	const GraphSymbol *reached(const GraphEdge &edge) const override {
		if (const GraphSymbol *found = symbol(edge.kind, edge.value, edge.scope)) return found;
		const size_t slash = edge.scope.find('/');
		if (edge.scope_alternate.empty()) return nullptr;
		return symbol(edge.kind, edge.value, edge.scope_alternate + (slash == std::string::npos ? "" : edge.scope.substr(slash)));
	}
	uint64_t generation() const override { return generation_; }
	uint64_t generation_ = 1;

private:
	std::vector<GraphSymbol> symbols_;
};

// The fixture's catalog and table as names: the four items it places (the location marker's 102044
// none), the walker's name, the location's, the objectives row.
TableNames fixture_names() {
	TableNames names;
	names.define(ReferenceKind::Item, "106100", "Wire Test Pump", "13");
	names.define(ReferenceKind::Item, "106101", "Wire Test Armory", "2");
	names.define(ReferenceKind::Item, "106102", "Wire Test Rifleman", "6");
	names.define(ReferenceKind::Item, "100001", "Marker Alpha", "4");
	names.define(ReferenceKind::TextId, "STRNAME001", "PeopleNames/STRNAME001", "Sgt. Walker", "SYNTH_LOGIC.BIN/PeopleNames",
	             "missions/synth_logic.bin");
	names.define(ReferenceKind::TextId, "LOCATION001", "Locations/LOCATION001", "Pump House", "SYNTH_LOGIC.BIN/Locations",
	             "missions/synth_logic.bin");
	names.define(ReferenceKind::TextId, "STRWINCOND001", "WinConditions/STRWINCOND001", "Pump house reached",
	             "SYNTH_LOGIC.BIN/WinConditions", "missions/synth_logic.bin");
	return names;
}

// The document's own words: no source of names.
int test_document_words() {
	std::unique_ptr<Document> document =
	        open(test_io::read_file(repo() + "/fixtures/bms/synth_logic.bms"));
	TEST_EXPECT(document);
	if (!document) return 1;
	const MissionDocument &m = as_mission(*document);
	const NodeAddress walker = row_at(*document, MissionKind::Organic, 0);
	const std::string walker_ssn = std::to_string(ssn_of(*document, walker));
	// An entity by its pool and its SSN; the title the windows read is the same (record_title), while the
	// graph's record name stays the SSN.
	TEST_EXPECT(record_display(*document, walker, nullptr) == "Organic #" + walker_ssn);
	TEST_EXPECT(document->record_title(walker) == "Organic #" + walker_ssn && document->record_name(walker) == walker_ssn);
	// A path by its stops, one with none and the commands; an area by its zone, the boundary said.
	TEST_EXPECT(record_display(*document, row_at(*document, MissionKind::WaypointPath, 1), nullptr) == "Path 1 (4 stops)");
	TEST_EXPECT(record_display(*document, row_at(*document, MissionKind::WaypointPath, 2), nullptr) == "Path 2 (no stops)");
	TEST_EXPECT(record_display(*document, row_at(*document, MissionKind::WaypointPath, 0), nullptr) == "No path");
	TEST_EXPECT(record_display(*document, row_at(*document, MissionKind::WaypointPath, 127), nullptr) == "Goto player");
	TEST_EXPECT(record_display(*document, row_at(*document, MissionKind::Area, 0), nullptr) == "Zone 20");
	TEST_EXPECT(record_display(*document, row_at(*document, MissionKind::Area, 1), nullptr) == "Zone 30 (mission area)");
	// An event as its sentence (the events lane's, mission_sentence.h) over these words (an entity
	// parameter by its title, a zone by its area's); its trigger and its action in words; a stop by the
	// marker it visits.
	const NodeAddress first = row_at(*document, MissionKind::Event, 0);
	const std::string event_one = record_display(*document, first, nullptr);
	TEST_EXPECT(event_one == "When Organic #" + walker_ssn + " is in Zone 20, then re-arm event 2.");
	TEST_EXPECT(record_display(*document, first_child(*document, first, MissionKind::Trigger), nullptr) ==
	            "Organic #" + walker_ssn + " is in Zone 20");
	TEST_EXPECT(record_display(*document, first_child(*document, first, MissionKind::Action), nullptr) == "re-arm event 2");
	// Briefly, for a narrow column: the event by its first trigger's subject and verb, an entity in it by
	// its SSN; the entity by its SSN first; a path or an area has no brief words (its title is short).
	TEST_EXPECT(same(record_brief(*document, first, nullptr), "#" + walker_ssn + " is in zone 20"));
	TEST_EXPECT(same(record_brief(*document, first_child(*document, first, MissionKind::Action), nullptr), "re-arm event 2"));
	TEST_EXPECT(same(record_brief(*document, walker, nullptr), "#" + walker_ssn + " Organic"));
	TEST_EXPECT(record_brief(*document, row_at(*document, MissionKind::Area, 0), nullptr).empty());
	const NodeAddress stop = first_child(*document, row_at(*document, MissionKind::WaypointPath, 1), MissionKind::Stop);
	const std::string marker_ssn = std::to_string(ssn_of(*document, row_at(*document, MissionKind::Marker, 0)));
	TEST_EXPECT(record_display(*document, stop, nullptr) == "Stop 1: Marker #" + marker_ssn);
	// A value: the walker's group, path and start stop; an Event trigger's event; a stop's marker.
	DisplayName group = shown(*document, walker, "group", nullptr);
	TEST_EXPECT(group.text == "Group 1 (1 entity)" && group.raw == "1" && !group.dangling);
	TEST_EXPECT(shown(*document, walker, "waypoint_id", nullptr).text == "Path 1 (4 stops)");
	TEST_EXPECT(shown(*document, walker, "wp_number", nullptr).text == "Stop 1 of 4: Marker #" + marker_ssn);
	const DisplayName event = shown(*document, first_child(*document, row_at(*document, MissionKind::Event, 1), MissionKind::Trigger),
	                                "param1", nullptr);
	TEST_EXPECT(event.text == "Event 1: " + event_one && event.raw == "0");
	TEST_EXPECT(shown(*document, stop, "marker", nullptr).text == "Marker #" + marker_ssn);
	// Without names an item is its id alone (no words), and a name index forms its key.
	TEST_EXPECT(shown(*document, walker, "item", nullptr).text.empty());
	TEST_EXPECT(shown(*document, walker, "name_index", nullptr).text == "STRNAME001");
	// What names nothing says so: an SSN, a zone, an event, a group, a marker the mission lacks; the
	// player's SSN is the player.
	TEST_EXPECT(mission_ssn_display(m, 55555, nullptr).text == "No entity has SSN 55555" &&
	            mission_ssn_display(m, 55555, nullptr).dangling);
	TEST_EXPECT(mission_ssn_display(m, 10000, nullptr).text == "The player" && !mission_ssn_display(m, 10000, nullptr).dangling);
	TEST_EXPECT(mission_zone_display(m, 7).text == "No area has zone 7" && mission_zone_display(m, 7).dangling);
	TEST_EXPECT(mission_event_display(m, 8, nullptr).text == "No event 9 (the mission has 2 events)");
	TEST_EXPECT(mission_group_display(m, 70).text == "No group 70 (the mission has 64)" && mission_group_display(m, 0).text == "No group");
	TEST_EXPECT(mission_marker_display(m, 40, nullptr).text == "No marker 40 (the mission has 5 markers)");
	std::printf("document words: entities, paths, areas, events, stops, values, dangling\n");
	return 0;
}

// The words with names: an entity by its item's name and the name the game shows for it, an item id by
// its catalog's name (one the catalog lacks said so), a text key by its string; the picker's choices
// worded; the cache keeps a title until the document or the names move.
int test_named_words() {
	std::unique_ptr<Document> document = open(test_io::read_file(repo() + "/fixtures/bms/synth_logic.bms"));
	TEST_EXPECT(document);
	if (!document) return 1;
	const TableNames names = fixture_names();
	const NodeAddress walker = row_at(*document, MissionKind::Organic, 0);
	const std::string walker_ssn = std::to_string(ssn_of(*document, walker));
	TEST_EXPECT(record_display(*document, walker, &names) == "Wire Test Rifleman #" + walker_ssn + " (Sgt. Walker)");
	// Briefly: the SSN, then the name the game shows for him before his item's.
	TEST_EXPECT(same(record_brief(*document, walker, &names), "#" + walker_ssn + " Sgt. Walker"));
	const NodeAddress location = row_at(*document, MissionKind::Marker, 4);
	TEST_EXPECT(record_display(*document, location, &names) == "Marker #" + std::to_string(ssn_of(*document, location)) +
	                                                                 " (Pump House)");
	const DisplayName item = shown(*document, walker, "item", &names);
	TEST_EXPECT(item.text == "Wire Test Rifleman" && item.raw == "106102" && item.source == "defs/items.def");
	const DisplayName missing = shown(*document, location, "item", &names);
	TEST_EXPECT(missing.dangling && missing.text == "No item 102044 in the project");
	const DisplayName name = shown(*document, walker, "name_index", &names);
	TEST_EXPECT(name.text == "\"Sgt. Walker\"" && name.raw == "1" && name.source == "SYNTH_LOGIC.BIN/PeopleNames");
	// The objectives panel's row: its STRWINCOND.
	const NodeAddress mission{as_mission(*document).mission_row()->id, k(MissionKind::Mission), 0};
	TEST_EXPECT(shown(*document, mission, "win_conditions[0]", &names).text == "\"Pump house reached\"");
	// The trigger's SSN by the walker's title.
	const NodeAddress trigger = first_child(*document, row_at(*document, MissionKind::Event, 0), MissionKind::Trigger);
	TEST_EXPECT(shown(*document, trigger, "param1", &names).text == "Wire Test Rifleman #" + walker_ssn + " (Sgt. Walker)");
	TEST_EXPECT(shown(*document, trigger, "param2", &names).text == "Zone 20");
	// The event's sentence with the names: the walker by his item's name and his own.
	TEST_EXPECT(record_display(*document, row_at(*document, MissionKind::Event, 0), &names) ==
	            "When Wire Test Rifleman #" + walker_ssn + " (Sgt. Walker) is in Zone 20, then re-arm event 2.");
	// The picker's choices worded: an SSN by its entity's title, an id muted beside it.
	const FieldSchema *param1 = nullptr;
	for (const FieldSchema &schema : document->fields(k(MissionKind::Trigger)))
		if (schema.id == "param1") param1 = &schema;
	TEST_EXPECT(param1);
	if (!param1) return 1;
	std::vector<ReferenceChoice> choices(2);
	choices[0].name = walker_ssn;
	choices[1].name = "777";
	word_choices(*document, trigger, document->field_on(trigger, *param1), &names, choices);
	TEST_EXPECT(choices[0].label == "Wire Test Rifleman #" + walker_ssn + " (Sgt. Walker)" &&
	            choices[1].label == "No entity has SSN 777");
	// The cache: a title worded once while the document and the names stand.
	DisplayNameCache cache;
	for (int i = 0; i < 3; ++i) TEST_EXPECT(cache.record(*document, walker, &names) == record_display(*document, walker, &names));
	TEST_EXPECT(cache.made() == 1);
	TableNames moved = fixture_names();
	moved.generation_ = 2;
	cache.record(*document, walker, &moved);
	TEST_EXPECT(cache.made() == 2);
	std::printf("named words: items, shown names, text keys, choices, the cache\n");
	return 0;
}

// Through a session: a project holding the fixture's catalog, the mission and its table; the graph's
// names (GraphNameSource): a title, a value, the picker's choices (reference_choices then
// word_choices), the record query's `display` and the document query's `title`.
int test_session_words() {
	editor_test::TempProjectDir dir("opennova_mission_labels");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Labels"));
	editor_test::create_missing_files(session);
	const std::string root = session.view().project.root;
	TEST_EXPECT(editor_test::write_bytes(root + "/defs/items.def", test_io::read_file(repo() + "/fixtures/def/items.def")));
	TEST_EXPECT(editor_test::write_bytes(root + "/missions/synth_logic.bms", test_io::read_file(repo() + "/fixtures/bms/synth_logic.bms")));
	TEST_EXPECT(editor_test::write_bytes(root + "/missions/synth_logic.bin", test_io::read_file(repo() + "/fixtures/bms/synth_logic.bin")));
	editor_test::handle_to_end(session, request::rescan());
	editor_test::handle_to_end(session, request::open_document("missions/synth_logic.bms"));
	const Document *document = session.document_for("missions/synth_logic.bms");
	const SessionView &view = session.view();
	TEST_EXPECT(document && view.findings.graph);
	if (!document || !view.findings.graph) return 1;
	const GraphNameSource names(*view.findings.graph);
	const NodeAddress walker = row_at(*document, MissionKind::Organic, 0);
	const std::string walker_ssn = std::to_string(ssn_of(*document, walker));
	TEST_EXPECT(record_display(*document, walker, &names) == "Wire Test Rifleman #" + walker_ssn + " (Sgt. Walker)");
	TEST_EXPECT(shown(*document, walker, "item", &names).text == "Wire Test Rifleman");
	// The item field's picker: every item of the catalog, each worded by its name.
	FieldUse item_field;
	for (const FieldSchema &schema : document->fields(walker.kind))
		if (schema.id == "item") item_field = document->field_on(walker, schema);
	std::vector<ReferenceChoice> items = reference_choices(*view.findings.graph, item_field);
	word_choices(*document, walker, item_field, &names, items);
	const auto rifleman = std::find_if(items.begin(), items.end(), [](const ReferenceChoice &c) { return c.name == "106102"; });
	TEST_EXPECT(rifleman != items.end() && rifleman->label == "Wire Test Rifleman");
	TEST_EXPECT(symbol_preview(*view.findings.graph, ReferenceKind::Item, "106102", std::string()).find("Model: shed") !=
	            std::string::npos);
	// The wire: the record's field carries its words beside its value; a row its title.
	const opennova::io::JsonValue record = record_to_json(*document, walker, view);
	bool worded = false;
	if (const opennova::io::JsonValue *fields = record.get("fields"))
		for (const opennova::io::JsonValue &field : fields->array)
			if (field.get_string("id", "") == "item")
				worded = field.get_string("display", "") == "Wire Test Rifleman" && field.get_number("value", 0) == 106102.0;
	TEST_EXPECT(worded && record.get_string("title", "") == "Wire Test Rifleman #" + walker_ssn + " (Sgt. Walker)");
	// Problems: a finding's place in words while its file is open, the record by its title, the field
	// by what the record calls it (a trigger's parameter by what it reads); none for the raw ids.
	const NodeAddress trigger = first_child(*document, row_at(*document, MissionKind::Event, 0), MissionKind::Trigger);
	Diagnostic finding;
	finding.asset = document->path();
	finding.row_id = trigger.row;
	finding.record_kind = trigger.kind;
	finding.child_id = trigger.child;
	finding.field = "param2";
	std::string param2;
	for (const FieldSchema &schema : document->fields(trigger.kind))
		if (schema.id == "param2") param2 = field_title(document->field_on(trigger, schema));
	TEST_EXPECT(finding_record_title(finding, view) == record_display(*document, trigger, &names));
	TEST_EXPECT(!param2.empty() && param2 != "param2" && finding_field_title(finding, view) == param2);
	finding.field = "no_such_field";
	TEST_EXPECT(finding_field_title(finding, view).empty());
	// The status line: an edit in words, the field by its name, the record by its title as it read
	// before the edit (its item set renames it), the value by what it names.
	Edit item;
	item.address = walker;
	item.field = "item";
	item.value = int64_t(106100);
	editor_test::handle_to_end(session, request::edit_record("missions/synth_logic.bms", item));
	TEST_EXPECT(same(view.activity.status, "Set Item of Wire Test Rifleman #" + walker_ssn + " (Sgt. Walker) to Wire Test Pump."));
	Edit group;
	group.address = walker;
	group.field = "group";
	group.value = int64_t(5);
	editor_test::handle_to_end(session, request::edit_record("missions/synth_logic.bms", group));
	TEST_EXPECT(same(view.activity.status, "Set Group of Wire Test Pump #" + walker_ssn + " (Sgt. Walker) to Group 5 (1 entity)."));
	// A trigger's entity set to the player: no record named (no reference, no badge), yet still picked by
	// name as an entity (FieldUse::picks), the player in words.
	Edit player;
	player.address = trigger;
	player.field = "param1";
	player.value = int64_t(10000);
	editor_test::handle_to_end(session, request::edit_record("missions/synth_logic.bms", player));
	FieldUse entity;
	for (const FieldSchema &schema : document->fields(trigger.kind))
		if (schema.id == "param1") entity = document->field_on(trigger, schema);
	TEST_EXPECT(entity.schema && entity.reference == ReferenceKind::None && entity.picks == ReferenceKind::MissionEntity &&
	            !entity.scope.empty());
	TEST_EXPECT(same(shown(*document, trigger, "param1", &names).text, "The player"));
	std::printf("session words: the graph's names, the picker, the wire, Problems' places, the status line\n");
	return 0;
}

// The names an install's files define, keyed for lookups by kind, name and scope (the graph's keys,
// without case): the items of its catalog, the string ids of every table it ships; a text key found in
// its edge's own table where the install ships that table, else in the alternate (as the graph's lookup
// tries them, AssetGraph::lookup_scope).
class InstallNames : public NameSource {
public:
	void add(const Extracted &extracted) {
		for (const GraphSymbol &symbol : extracted.symbols) {
			symbols_.emplace(key(symbol.kind, symbol.display, symbol.scope), symbol);
			if (!symbol.scope.empty()) tables_.insert(opennova::strutil::to_upper(symbol.scope.substr(0, symbol.scope.find('/'))));
		}
	}
	const GraphSymbol *symbol(ReferenceKind kind, const std::string &name, const std::string &scope) const override {
		const auto found = symbols_.find(key(kind, name, scope));
		return found == symbols_.end() ? nullptr : &found->second;
	}
	const GraphSymbol *reached(const GraphEdge &edge) const override {
		const size_t slash = edge.scope.find('/');
		if (tables_.count(opennova::strutil::to_upper(edge.scope.substr(0, slash))) || edge.scope_alternate.empty())
			return symbol(edge.kind, edge.value, edge.scope);
		return symbol(edge.kind, edge.value, edge.scope_alternate + (slash == std::string::npos ? "" : edge.scope.substr(slash)));
	}
	uint64_t generation() const override { return 1; }

private:
	static std::string key(ReferenceKind kind, const std::string &name, const std::string &scope) {
		return std::to_string(int(kind)) + "|" + opennova::strutil::to_upper(name) + "|" + opennova::strutil::to_upper(scope);
	}
	std::unordered_map<std::string, GraphSymbol> symbols_;
	std::set<std::string> tables_;
};

// Every record and every value of the shipped missions in words (OPENNOVA_JO_DIR, base and each
// expansion through the VFS), with the install's item catalog and string tables as the names: every
// record titled, every reference value worded, what names nothing counted by what it is (the SSNs and
// zones mission_document's retail leg counts as naming nothing, the same here), and the time each
// mission's words took (a title or a value a row, per drawn row: held to a bound well under a frame).
int test_retail() {
	const std::string root = retail::install();
	if (root.empty()) return retail::skip_leg("OPENNOVA_JO_DIR (every shipped mission worded)");
	std::vector<std::string> expansions = opennova::vfs_list_expansions(root);
	expansions.insert(expansions.begin(), std::string());
	std::set<std::string> seen;
	size_t missions = 0, titles = 0, values = 0, items_named = 0, shown_names = 0;
	std::map<std::string, size_t> dangling;
	double words_ms = 0, slowest_ms = 0, slowest_per_row_us = 0;
	size_t slowest_rows = 0, all_rows = 0;
	for (const std::string &expansion : expansions) {
		opennova::Vfs game;
		game.set_scr_policy(opennova::VFS_SCR_FORCE_JO_DFX2);
		TEST_EXPECT(game.mount_game(root, expansion, opennova::VfsMountMode::Packed));
		// The names: the def catalogs (the items, the weapons a loadout names) and every string table the
		// mount holds.
		InstallNames names;
		std::vector<opennova::VfsFileLocation> bms;
		for (const opennova::VfsFileLocation &file : game.list_files()) {
			const std::string extension = retail::lower_ascii(std::filesystem::path(file.logical_name).extension().string());
			if (extension == ".bms") {
				bms.push_back(file);
				continue;
			}
			if (extension != ".def" && extension != ".bin") continue;
			// A .bin's kind needs its bytes (a string table or another table): read as one, skipped
			// where it does not load as one.
			const AssetKind kind = extension == ".bin" ? AssetKind::Strings : asset_kind_for_name(file.logical_name);
			std::vector<uint8_t> bytes;
			const DocumentType *type = document_type_for(kind);
			std::unique_ptr<Document> table = type ? records_of(type->make()) : nullptr;
			Diagnostic error;
			if (!table || !game.read_file(file.logical_name, bytes) ||
			    !table->load_bytes(bytes, file.logical_name, kind, "jo", error))
				continue;
			Extracted extracted;
			extract_from_document(*table, extracted);
			names.add(extracted);
		}
		for (const opennova::VfsFileLocation &file : bms) {
			if (!seen.insert(file.source_path + "|" + retail::lower_ascii(file.logical_name)).second) continue;
			std::vector<uint8_t> bytes;
			TEST_EXPECT(game.read_file(file.logical_name, bytes));
			std::unique_ptr<Document> document = open(bytes, file.logical_name.c_str());
			TEST_EXPECT(document && !document->blocked());
			if (!document) continue;
			++missions;
			const auto started = std::chrono::steady_clock::now();
			size_t rows = 0;
			for (const auto &row : document->rows()) {
				++rows;
				const NodeAddress address{row->id, row->kind, 0};
				std::vector<NodeAddress> records{address};
				document->walk_records(*row, [&](const NodeAddress &record, const Document::Placement &) {
					records.push_back(record);
					return true;
				});
				for (const NodeAddress &record : records) {
					const std::string title = record_display(*document, record, &names);
					TEST_EXPECT(!title.empty());
					++titles;
					if (is_entity_kind(record.kind) && !record.child) shown_names += title.find(" (") != std::string::npos;
					// Every field a record's type words or that names something.
					for (const FieldSchema &schema : document->fields(record.kind)) {
						const FieldUse field = document->field_on(record, schema);
						if (field.applies == Applicability::Ignored) continue;
						Value value;
						if (!document->get(record, schema.id, value)) continue;
						const DisplayName words = value_display(*document, record, field, value, &names);
						if (words.text.empty()) continue;
						++values;
						if (schema.id == "item" && !words.dangling) ++items_named;
						if (!words.dangling) continue;
						// What it is that names nothing: its first word ("No entity has SSN 5" an SSN).
						const std::string &text = words.text;
						const std::string what = text.find("SSN") != std::string::npos      ? "ssn"
						                         : text.find("zone") != std::string::npos   ? "zone"
						                         : text.find("item") != std::string::npos   ? "item"
						                         : text.find("text") != std::string::npos   ? "text"
						                         : text.find("event") != std::string::npos  ? "event"
						                         : text.find("marker") != std::string::npos ? "marker"
						                         : text.find("stop") != std::string::npos   ? "stop"
						                                                                    : "other: " + text;
						++dangling[what];
					}
				}
			}
			const double took = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
			words_ms += took;
			all_rows += rows;
			if (took > slowest_ms) {
				slowest_ms = took;
				slowest_rows = rows;
			}
			slowest_per_row_us = std::max(slowest_per_row_us, took * 1000.0 / double(std::max<size_t>(rows, 1)));
		}
	}
	if (missions == 0) return retail::skip_leg("OPENNOVA_JO_DIR with the game's missions in its archives");
	std::printf("retail: %zu missions, %zu records titled (%zu entities with a name the game shows), %zu values worded "
	            "(%zu items by their catalog's name)\n",
	            missions, titles, shown_names, values, items_named);
	for (const auto &[what, count] : dangling) std::printf("  naming nothing, %s: %zu\n", what.c_str(), count);
	std::printf("retail: the words took %.1f ms over the %zu missions, the slowest %.1f ms (%zu rows), at most %.1f us a row\n",
	            words_ms, missions, slowest_ms, slowest_rows, slowest_per_row_us);
	TEST_EXPECT(missions == 115);
	// The SSNs and zones naming nothing are the ones mission_document's retail leg counts (162 entity
	// parameters naming no SSN of their mission, 53 zone parameters naming no zone); no event past its
	// table, no stop past the markers.
	TEST_EXPECT(dangling["ssn"] == 162 && dangling["zone"] == 53 && !dangling.count("event") && !dangling.count("marker"));
	// Fast enough to word every row of a mission each time it is drawn whole: a few microseconds a row
	// over the 115 (the bound is the whole pass's mean, which a loaded machine's one slow mission does
	// not move; the slowest mission's is printed).
	const double mean_per_row_us = words_ms * 1000.0 / double(std::max<size_t>(all_rows, 1));
	std::printf("retail: %.1f us a row over the %zu rows\n", mean_per_row_us, all_rows);
	TEST_EXPECT(mean_per_row_us < 200.0);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	if (test_document_words() != 0) return 1;
	if (test_named_words() != 0) return 1;
	if (test_session_words() != 0) return 1;
	return test_retail();
}
