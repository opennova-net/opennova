// The mission document (editor/documents/mission_document.h, ADR 0046 S14) over the minted
// fixtures/bms/synth_logic.bms: the rows in bands with their names, the bytes written back as read;
// every reference a mission makes as a graph edge of its kind (a file's: the terrain, the environment,
// each entity's item, each loadout entry's weapon; a record of its own file by index: a stop's marker,
// an entity's group and path, an Event parameter's event; by id: an entity's SSN and an area's zone id,
// defined as symbols in the mission's own scope and named by a parameter; the text keys a record's
// number forms, in the mission's own table then medmssn.bin), the player's SSN naming none, a
// parameter's label and own choices by its record's type; the edits: an entity added with its item
// into its band with the next SSN, an area with the lowest free zone id, an event empty, a path or the
// mission row never added, moved or removed, a duplicate given a fresh id, a marker removed only with
// the stops that visit it (removal_edits), an event named by index removed only with its namer, a
// marker added before the markers named leaving every stop naming its marker, a stop edit refused on
// a path counted past its slots (D-MIS-6); what the parse makes of a file laid out otherwise than its
// events stand (event_order), of one whose runs share a record (invalid_input, blocked) and of one
// holding bytes the writer writes otherwise (rewrite_differs), each a finding of the type's; what the
// picture reads (mission_reads.h); a second record of an SSN or a zone id inert; and an item's TYPE on
// its symbol (the def catalog's refine_symbol).
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <base/io/strutil.h>
#include <base/vfs/vfs.h>
#include <base/vfs/vfs_decode.h>
#include <editor/documents/def_catalog_document.h>
#include <editor/documents/document_types.h>
#include <editor/documents/mission_document.h>
#include <editor/documents/mission_reads.h>
#include <editor/documents/mission_validation.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/reference_kinds.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/mission/mission.h>

#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;
namespace bms = opennova::bms;
namespace mission = opennova::mission;

namespace {

constexpr NodeKind k(MissionKind kind) { return node_kind(kind); }

std::vector<uint8_t> fixture_bytes() {
	return test_io::read_file(std::string(test_paths_repo_root(__FILE__)) + "/fixtures/bms/synth_logic.bms");
}

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

std::string bytes_of(const Document &document) { return document.serialize().text; }

Edit edit_of(EditOperation operation, const NodeAddress &address, const std::string &field = std::string(),
             Value value = int64_t(0)) {
	Edit edit;
	edit.operation = operation;
	edit.address = address;
	edit.field = field;
	edit.value = std::move(value);
	return edit;
}

size_t count_edges(const Extracted &extracted, ReferenceKind kind) {
	size_t n = 0;
	for (const GraphEdge &edge : extracted.edges) n += edge.kind == kind ? 1 : 0;
	return n;
}
const GraphEdge *edge(const Extracted &extracted, ReferenceKind kind, const char *value) {
	for (const GraphEdge &edge : extracted.edges)
		if (edge.kind == kind && edge.value == value) return &edge;
	return nullptr;
}
size_t count_symbols(const Extracted &extracted, ReferenceKind kind) {
	size_t n = 0;
	for (const GraphSymbol &symbol : extracted.symbols) n += symbol.kind == kind ? 1 : 0;
	return n;
}

// The first child of `kind` the row holds.
NodeAddress first_child(const Document &document, const NodeAddress &row, MissionKind kind) {
	for (const Document::Collection &collection : document.collections_of(row))
		if (collection.spec.kind == k(kind) && !collection.ids.empty()) return {row.row, k(kind), collection.ids[0]};
	return NodeAddress();
}

int test_rows() {
	const std::vector<uint8_t> bytes = fixture_bytes();
	std::unique_ptr<Document> document = open(bytes);
	TEST_EXPECT(document && !document->blocked() && document->issues().empty());
	const MissionDocument &m = as_mission(*document);
	// One mission row, 12 entities in their pools, the 128 paths, two areas, two events: in bands.
	TEST_EXPECT(document->rows().size() == 1 + 12 + 128 + 2 + 2);
	TEST_EXPECT(m.rows_of(MissionKind::Item).size() == 3 && m.rows_of(MissionKind::Building).size() == 2 &&
	            m.rows_of(MissionKind::Marker).size() == 5 && m.rows_of(MissionKind::Organic).size() == 2 &&
	            m.rows_of(MissionKind::WaypointPath).size() == 128 && m.rows_of(MissionKind::Area).size() == 2 &&
	            m.rows_of(MissionKind::Event).size() == 2);
	int band = 0;
	for (const auto &row : document->rows()) {
		TEST_EXPECT(mission_band(row->kind) >= band);
		band = mission_band(row->kind);
	}
	TEST_EXPECT(m.mission_row() && m.mission_row()->name() == "Synthetic Logic");
	TEST_EXPECT(document->record_name(row_at(*document, MissionKind::Area, 0)) == "Zone 20");
	TEST_EXPECT(document->record_name(row_at(*document, MissionKind::WaypointPath, 0)) == "None" &&
	            document->record_name(row_at(*document, MissionKind::WaypointPath, 1)) == "1" &&
	            document->record_name(row_at(*document, MissionKind::WaypointPath, 123)) == "Goto SSN (not driver, gunner)");
	TEST_EXPECT(document->record_name(row_at(*document, MissionKind::Event, 1)) == "Event 2");
	// An event holds its triggers and its actions; the bytes are written back as read.
	const NodeAddress event = row_at(*document, MissionKind::Event, 0);
	TEST_EXPECT(first_child(*document, event, MissionKind::Trigger).child && first_child(*document, event, MissionKind::Action).child);
	TEST_EXPECT(bytes_of(*document) == std::string(bytes.begin(), bytes.end()));
	TEST_EXPECT(document->rewrite_need() == DocumentBase::RewriteNeed::None);
	bms::File composed;
	TEST_EXPECT(m.compose(composed) && composed.triggers.size() == 2 && composed.events[1].trigger_index == 1);
	std::printf("rows: %zu in bands, the bytes back as read\n", document->rows().size());
	return 0;
}

int test_references() {
	std::unique_ptr<Document> document = open(fixture_bytes());
	TEST_EXPECT(document);
	Extracted extracted;
	extract_from_document(*document, extracted);
	// The files: the terrain, the environment, each entity's item, each loadout entry's weapon.
	const GraphEdge *terrain = edge(extracted, ReferenceKind::Terrain, "Tmap");
	TEST_EXPECT(terrain && terrain->rewritable && terrain->field == "terrain" && edge(extracted, ReferenceKind::Environment, "synth_full"));
	TEST_EXPECT(count_edges(extracted, ReferenceKind::Item) == 12 && edge(extracted, ReferenceKind::Item, "106101") &&
	            edge(extracted, ReferenceKind::Item, "102044"));
	TEST_EXPECT(count_edges(extracted, ReferenceKind::Weapon) == 4 && edge(extracted, ReferenceKind::Weapon, "WPN_KNIFE"));
	// By index: the stops' markers, the organics' group and path (0 names none), the KillGroup's group,
	// the ResetEvent's and the Event trigger's event.
	TEST_EXPECT(count_edges(extracted, ReferenceKind::MissionMarker) == 4 && edge(extracted, ReferenceKind::MissionMarker, "3"));
	TEST_EXPECT(count_edges(extracted, ReferenceKind::MissionGroup) == 3 && edge(extracted, ReferenceKind::MissionGroup, "1") &&
	            count_edges(extracted, ReferenceKind::MissionPath) == 1 && edge(extracted, ReferenceKind::MissionPath, "1"));
	TEST_EXPECT(count_edges(extracted, ReferenceKind::MissionEvent) == 2 && edge(extracted, ReferenceKind::MissionEvent, "0") &&
	            edge(extracted, ReferenceKind::MissionEvent, "1"));
	// By id, in the mission's own scope: the trigger's SSN and zone; the record sets and the symbols.
	const int walker_ssn = static_cast<const EntityRow *>(as_mission(*document).rows_of(MissionKind::Organic)[0])->native.id;
	const GraphEdge *ssn = edge(extracted, ReferenceKind::MissionEntity, std::to_string(walker_ssn).c_str());
	TEST_EXPECT(ssn && ssn->scope == "SYNTH_LOGIC.BMS" && ssn->rewritable && ssn->field == "param1");
	const GraphEdge *zone = edge(extracted, ReferenceKind::MissionZone, "20");
	TEST_EXPECT(zone && zone->scope == "SYNTH_LOGIC.BMS" && zone->field == "param2");
	TEST_EXPECT(count_symbols(extracted, ReferenceKind::MissionEntity) == 12 && count_symbols(extracted, ReferenceKind::MissionZone) == 2 &&
	            count_symbols(extracted, ReferenceKind::MissionMarker) == 5 && count_symbols(extracted, ReferenceKind::MissionEvent) == 2 &&
	            count_symbols(extracted, ReferenceKind::MissionGroup) == 64 && count_symbols(extracted, ReferenceKind::MissionPath) == 128);
	for (const GraphSymbol &symbol : extracted.symbols)
		if (symbol.kind == ReferenceKind::MissionEntity || symbol.kind == ReferenceKind::MissionZone)
			TEST_EXPECT(symbol.scope == "SYNTH_LOGIC.BMS" && !symbol.inert);
	// The text keys the records' numbers form: the location marker's, the walker's name, the win and
	// lose directives, in the mission's own table then medmssn.bin, none rewritable.
	const GraphEdge *location = edge(extracted, ReferenceKind::TextId, "LOCATION001");
	TEST_EXPECT(location && location->scope == "SYNTH_LOGIC.BIN/Locations" && !location->rewritable &&
	            location->scopes_after == std::vector<std::string>{"MEDMSSN.BIN/Locations"} &&
	            location->address == row_at(*document, MissionKind::Marker, 4));
	const GraphEdge *name = edge(extracted, ReferenceKind::TextId, "STRNAME001");
	TEST_EXPECT(name && name->scope == "SYNTH_LOGIC.BIN/PeopleNames" && name->field == "name_index" &&
	            name->address == row_at(*document, MissionKind::Organic, 0));
	const GraphEdge *win = edge(extracted, ReferenceKind::TextId, "STRWINDIRECTIVE001");
	TEST_EXPECT(win && win->scope == "SYNTH_LOGIC.BIN/WinConditions" && win->field == "win_conditions[0]" &&
	            win->address == row_at(*document, MissionKind::Mission, 0) && edge(extracted, ReferenceKind::TextId, "STRWINCOND001") &&
	            edge(extracted, ReferenceKind::TextId, "STRLOSEDIRECTIVE001") && count_edges(extracted, ReferenceKind::TextId) == 5);
	// What a parameter is called and whether the game reads it, by its record's type; its own choices.
	const NodeAddress trigger = first_child(*document, row_at(*document, MissionKind::Event, 0), MissionKind::Trigger);
	const NodeAddress reset = first_child(*document, row_at(*document, MissionKind::Event, 0), MissionKind::Action);
	const auto use = [&](const NodeAddress &record, const char *id) {
		for (const FieldSchema &schema : document->fields(record.kind))
			if (schema.id == id) return document->field_on(record, schema);
		return FieldUse();
	};
	TEST_EXPECT(use(trigger, "param2").reference == ReferenceKind::MissionZone && use(trigger, "param2").label &&
	            use(trigger, "param2").scope == "SYNTH_LOGIC.BMS" && use(trigger, "param3").applies == Applicability::Ignored);
	TEST_EXPECT(use(reset, "param1").reference == ReferenceKind::MissionEvent && use(reset, "param2").applies == Applicability::Ignored);
	std::vector<FieldChoice> own;
	const FieldUse sub = use(trigger, "sub_type");
	TEST_EXPECT(sub.own_choices && document->record_choices(trigger, sub, own) && !own.empty() &&
	            std::any_of(own.begin(), own.end(), [](const FieldChoice &c) { return c.value == 10; }));
	// The player's SSN names no record: a parameter set to it makes no edge.
	Diagnostic error;
	TEST_EXPECT(document->apply(edit_of(EditOperation::Set, trigger, "param1", int64_t(10000)), error));
	Extracted again;
	extract_from_document(*document, again);
	TEST_EXPECT(count_edges(again, ReferenceKind::MissionEntity) == 0 && use(trigger, "param1").reference == ReferenceKind::None);
	document->undo();
	std::printf("references: %zu edges, %zu symbols\n", extracted.edges.size(), extracted.symbols.size());
	return 0;
}

int test_edits() {
	std::unique_ptr<Document> document = open(fixture_bytes());
	TEST_EXPECT(document);
	const std::string original = bytes_of(*document);
	Diagnostic error;
	// An item added with its item, into its band (after the items, before the buildings), with the
	// next SSN; a later edit of the batch names it (batch_made).
	Edit add = edit_of(EditOperation::Add, {0, k(MissionKind::Item), 0}, "item", int64_t(106100));
	Edit place = edit_of(EditOperation::Set, {batch_made(0), k(MissionKind::Item), 0}, "x", 12.5);
	TEST_EXPECT(document->apply({add, place}, error));
	// The mission row, three items, then the new one, then the buildings.
	TEST_EXPECT(document->rows()[4]->id == document->last_added() && document->rows()[5]->kind == k(MissionKind::Building));
	const EntityRow &made = static_cast<const EntityRow &>(*document->rows()[4]);
	TEST_EXPECT(mission::entity_item_id(made.native) == 106100 && made.native.id == 13 && made.native.x == bms::to_fixed_16_16(12.5));
	// An area with the lowest free zone id; an event empty; a path and the mission row never added.
	TEST_EXPECT(document->apply(edit_of(EditOperation::Add, {0, k(MissionKind::Area), 0}), error) &&
	            static_cast<const AreaRow *>(document->row(document->last_added()))->native.id == 1);
	TEST_EXPECT(document->apply(edit_of(EditOperation::Add, {0, k(MissionKind::Event), 0}), error) &&
	            document->rows().back()->id == document->last_added() &&
	            static_cast<const EventRow *>(document->row(document->last_added()))->native.triggers.empty());
	TEST_EXPECT(!document->apply(edit_of(EditOperation::Add, {0, k(MissionKind::WaypointPath), 0}), error) && error.code() == "document.kind");
	TEST_EXPECT(!document->apply(edit_of(EditOperation::Add, {0, k(MissionKind::Mission), 0}), error));
	// The mission row and a path never move or go: a Move of the mission row stays in its band
	// (nothing changes), a path moved among the paths or removed is refused.
	Edit move = edit_of(EditOperation::Move, row_at(*document, MissionKind::Mission, 0));
	move.position = 5;
	const uint64_t revision = document->revision();
	TEST_EXPECT(document->apply(move, error) && document->revision() == revision &&
	            document->rows()[0]->kind == k(MissionKind::Mission));
	move = edit_of(EditOperation::Move, row_at(*document, MissionKind::WaypointPath, 7));
	move.position = 0;
	TEST_EXPECT(!document->apply(move, error) && error.code() == "document.structure");
	TEST_EXPECT(!document->apply(edit_of(EditOperation::Remove, row_at(*document, MissionKind::WaypointPath, 7)), error) &&
	            error.code() == "document.structure");
	// A duplicate takes a fresh SSN, a fresh zone id.
	TEST_EXPECT(document->apply(edit_of(EditOperation::Duplicate, row_at(*document, MissionKind::Organic, 0)), error) &&
	            static_cast<const EntityRow *>(document->row(document->last_added()))->native.id == 14);
	TEST_EXPECT(document->apply(edit_of(EditOperation::Duplicate, row_at(*document, MissionKind::Area, 0)), error) &&
	            static_cast<const AreaRow *>(document->row(document->last_added()))->native.id == 2);
	while (document->can_undo()) document->undo();
	TEST_EXPECT(bytes_of(*document) == original);

	// A marker a stop visits: a bare Remove is refused with the site; removal_edits takes the stops
	// first, and the later markers' stops are renumbered.
	const NodeAddress marker0 = row_at(*document, MissionKind::Marker, 0);
	TEST_EXPECT(!document->apply(edit_of(EditOperation::Remove, marker0), error) && error.code() == "document.collection" &&
	            error.message.find("stop 1") != std::string::npos);
	std::vector<Edit> removal;
	std::string why;
	TEST_EXPECT(document->removal_edits({marker0}, removal, why) && removal.size() == 2 &&
	            removal[0].address.kind == k(MissionKind::Stop) && removal[1].address == marker0);
	TEST_EXPECT(document->apply(removal, error));
	{
		const PathRow &path = *static_cast<const PathRow *>(document->row(row_at(*document, MissionKind::WaypointPath, 1).row));
		TEST_EXPECT(path.native.record.waypoint_numbers == std::vector<uint32_t>({0, 1, 2}) && path.native.record.marker_count == 3);
		TEST_EXPECT(as_mission(*document).rows_of(MissionKind::Marker).size() == 4);
	}
	document->undo();
	TEST_EXPECT(bytes_of(*document) == original);
	// A marker added before the markers named: every stop names the marker it named.
	Edit before = edit_of(EditOperation::Add, {0, k(MissionKind::Marker), 0}, "item", int64_t(100001));
	before.position = 6; // the first marker row's index: the mission row, three items, two buildings
	TEST_EXPECT(document->apply(before, error) && document->rows()[6]->id == document->last_added());
	{
		const PathRow &path = *static_cast<const PathRow *>(document->row(row_at(*document, MissionKind::WaypointPath, 1).row));
		TEST_EXPECT(path.native.record.waypoint_numbers == std::vector<uint32_t>({1, 2, 3, 4}));
	}
	document->undo();
	TEST_EXPECT(bytes_of(*document) == original);

	// An event another's trigger names: a bare Remove is refused, removal_edits of it alone refused
	// with the site, of both taken (the namer going first).
	const NodeAddress event0 = row_at(*document, MissionKind::Event, 0), event1 = row_at(*document, MissionKind::Event, 1);
	TEST_EXPECT(!document->apply(edit_of(EditOperation::Remove, event0), error) && error.code() == "document.collection");
	removal.clear();
	TEST_EXPECT(!document->removal_edits({event0}, removal, why) && why.find("Event 2's trigger 1") != std::string::npos);
	removal.clear();
	TEST_EXPECT(document->removal_edits({event0, event1}, removal, why) && removal.size() == 4 &&
	            removal[0].address.child != 0 && removal[1].address.child != 0);
	TEST_EXPECT(document->apply(removal, error) && as_mission(*document).rows_of(MissionKind::Event).empty());
	bms::File composed;
	TEST_EXPECT(as_mission(*document).compose(composed) && composed.events.empty() && composed.triggers.empty());
	document->undo();
	TEST_EXPECT(bytes_of(*document) == original);
	// A trigger added to an event and the event's first moved after it (a chain edit: the writer lays
	// the run out again); a 21st refused. (A Move into another event's chain is a Move across rows,
	// which the core applies inside one row: the clipboard carries a trigger between events.)
	const NodeAddress trigger = first_child(*document, event0, MissionKind::Trigger);
	TEST_EXPECT(document->apply(edit_of(EditOperation::Add, {event0.row, k(MissionKind::Trigger), 0}), error));
	Edit later = edit_of(EditOperation::Move, trigger);
	later.position = SIZE_MAX;
	TEST_EXPECT(document->apply(later, error));
	TEST_EXPECT(as_mission(*document).compose(composed) && composed.events[0].trigger_count == 2 &&
	            composed.triggers[1].main_type == bms::TriggerMainType::Single && composed.events[1].trigger_index == 2);
	document->undo();
	document->undo();
	for (int i = 0; i < 19; ++i) TEST_EXPECT(document->apply(edit_of(EditOperation::Add, {event0.row, k(MissionKind::Trigger), 0}), error));
	TEST_EXPECT(!document->apply(edit_of(EditOperation::Add, {event0.row, k(MissionKind::Trigger), 0}), error) &&
	            error.code() == "document.collection");
	while (document->can_undo()) document->undo();
	TEST_EXPECT(bytes_of(*document) == original);

	// A stop edit on a path counted past its 32 slots is refused (D-MIS-6).
	{
		bms::File file;
		std::string message;
		const std::vector<uint8_t> bytes = fixture_bytes();
		TEST_EXPECT(bms::parse(bytes.data(), bytes.size(), file, message));
		file.waypoint_records[1].marker_count = 39;
		std::vector<uint8_t> over;
		TEST_EXPECT(bms::write(file, over, message));
		std::unique_ptr<Document> counted = open(over, "over.bms");
		TEST_EXPECT(counted && !counted->blocked());
		const NodeAddress path = row_at(*counted, MissionKind::WaypointPath, 1);
		TEST_EXPECT(!counted->apply(edit_of(EditOperation::Add, {path.row, k(MissionKind::Stop), 0}), error) &&
		            error.message.find("32 slots") != std::string::npos);
		TEST_EXPECT(!counted->apply(edit_of(EditOperation::Remove, first_child(*counted, path, MissionKind::Stop)), error));
	}
	std::printf("edits: bands, ids, the cascade of a marker's stops, an event's namers, a stop on an over-counted path\n");
	return 0;
}

int test_parse_findings() {
	const DocumentType &type = *document_type_for(AssetKind::Mission);
	const std::vector<uint8_t> bytes = fixture_bytes();
	bms::File file;
	std::string message;
	TEST_EXPECT(bms::parse(bytes.data(), bytes.size(), file, message));
	// The runs laid out in the other order: noted, and Save lays them out as the events stand.
	{
		bms::File reordered = file;
		std::swap(reordered.triggers[0], reordered.triggers[1]);
		reordered.events[0].trigger_index = 1;
		reordered.events[1].trigger_index = 0;
		std::vector<uint8_t> out;
		TEST_EXPECT(bms::write(reordered, out, message) && out != bytes);
		std::unique_ptr<Document> document = open(out, "reordered.bms");
		TEST_EXPECT(document && !document->blocked() && document->issues().size() == 1 && !document->issues()[0].blocks);
		TEST_EXPECT(as_mission(*document).issue_codes() == std::vector<MissionFinding>{MissionFinding::EventOrder});
		const std::vector<Diagnostic> findings = type.validate_file(*document);
		TEST_EXPECT(findings.size() == 1 && findings[0].code() == "mission.event_order" &&
		            findings[0].severity == DiagnosticSeverity::Info && findings[0].row()->fixes == FindingFix::Rewrite);
		TEST_EXPECT(bytes_of(*document) == std::string(bytes.begin(), bytes.end()) &&
		            document->rewrite_need() == DocumentBase::RewriteNeed::Rewrite);
	}
	// A record in two events' runs: the document opens blocked, the finding on the event.
	{
		bms::File shared = file;
		shared.events[0].trigger_count = 2;
		std::vector<uint8_t> out;
		TEST_EXPECT(bms::write(shared, out, message));
		std::unique_ptr<Document> document = open(out, "shared.bms");
		TEST_EXPECT(document && document->blocked() && document->issues().size() == 1 && document->issues()[0].blocks);
		const std::vector<Diagnostic> findings = type.validate_file(*document);
		TEST_EXPECT(findings.size() == 1 && findings[0].code() == "mission.invalid_input" &&
		            findings[0].severity == DiagnosticSeverity::Error && findings[0].row()->blocks_save);
		TEST_EXPECT(findings[0].row_id != 0 && findings[0].record_kind == k(MissionKind::Event) &&
		            document->row(findings[0].row_id) && findings[0].record == "Event 2");
		TEST_EXPECT(!document->serialize().ok());
	}
	// Bytes the writer writes otherwise (two zero bytes inside the loadout chunk's length, as the
	// shipped missions with a damaged chunk hold): noted by the section, Save writes the chunk as the
	// game reads it.
	{
		std::vector<uint8_t> padded = bytes;
		const size_t chunk_end = bms::kHeaderSize + file.header.weapon_loadout_chunk_len;
		padded.insert(padded.begin() + std::ptrdiff_t(chunk_end), 2, 0);
		const uint16_t length = static_cast<uint16_t>(file.header.weapon_loadout_chunk_len + 2);
		padded[578] = static_cast<uint8_t>(length & 0xFF);
		padded[579] = static_cast<uint8_t>(length >> 8);
		std::unique_ptr<Document> document = open(padded, "padded.bms");
		TEST_EXPECT(document && !document->blocked() && document->issues().size() == 1);
		TEST_EXPECT(as_mission(*document).issue_codes() == std::vector<MissionFinding>{MissionFinding::RewriteDiffers} &&
		            document->issues()[0].message.find("loadout chunk") != std::string::npos);
		const std::vector<Diagnostic> findings = type.validate_file(*document);
		TEST_EXPECT(findings.size() == 1 && findings[0].code() == "mission.rewrite_differs" && findings[0].severity == DiagnosticSeverity::Info);
		TEST_EXPECT(bytes_of(*document) == std::string(bytes.begin(), bytes.end()));
	}
	std::printf("parse: the runs' order, a shared run, a chunk the writer writes otherwise\n");
	return 0;
}

int test_reads_and_symbols() {
	std::unique_ptr<Document> document = open(fixture_bytes());
	TEST_EXPECT(document);
	const MissionDocument &m = as_mission(*document);
	const std::vector<MissionEntityRead> entities = mission_entities(m);
	TEST_EXPECT(entities.size() == 12 && entities[0].pool == MissionKind::Item && entities[0].item_id == 106100 &&
	            entities[0].x == -100.0 && entities[0].y == -100.0 && entities[1].yaw == 90 && entities[10].pool == MissionKind::Organic &&
	            entities[10].group == 1 && entities[10].waypoint_id == 1 && entities[10].ssn == 11);
	MissionEntityRead one;
	TEST_EXPECT(mission_entity(m, row_at(*document, MissionKind::Building, 1), one) && one.item_id == 106101 && one.yaw == 270);
	TEST_EXPECT(!mission_entity(m, row_at(*document, MissionKind::Area, 0), one));
	const std::vector<MissionAreaRead> areas = mission_areas(m);
	TEST_EXPECT(areas.size() == 2 && areas[0].id == 20 && areas[0].x_min == -150.0 && areas[0].y_max == 150.0 && !areas[0].mission_area &&
	            areas[1].mission_area && !areas[1].constrains_z);
	const std::vector<MissionPathRead> paths = mission_paths(m);
	TEST_EXPECT(paths.size() == 1 && paths[0].number == 1 && paths[0].stops.size() == 4 && paths[0].markers.size() == 4 &&
	            paths[0].markers[3] == row_at(*document, MissionKind::Marker, 3) && paths[0].stops[0].kind == k(MissionKind::Stop));
	// A second record of an SSN is inert: the one the lookups find first in pool order (an organic
	// before an item, whatever the file's order), and of two of a pool the earlier.
	Diagnostic error;
	const NodeAddress item2 = row_at(*document, MissionKind::Item, 2), organic1 = row_at(*document, MissionKind::Organic, 1);
	TEST_EXPECT(document->apply(edit_of(EditOperation::Set, item2, "id", int64_t(entities[10].ssn)), error));
	TEST_EXPECT(document->apply(edit_of(EditOperation::Set, organic1, "id", int64_t(entities[10].ssn)), error));
	Extracted extracted;
	extract_from_document(*document, extracted);
	size_t inert = 0;
	for (const GraphSymbol &symbol : extracted.symbols) {
		if (symbol.kind != ReferenceKind::MissionEntity || symbol.display != std::to_string(entities[10].ssn)) continue;
		if (symbol.address == row_at(*document, MissionKind::Organic, 0)) TEST_EXPECT(!symbol.inert);
		else {
			TEST_EXPECT(symbol.inert && symbol.inert_reason.find("pool order") != std::string::npos);
			++inert;
		}
	}
	TEST_EXPECT(inert == 2);
	TEST_EXPECT(document->apply(edit_of(EditOperation::Set, row_at(*document, MissionKind::Area, 1), "id", int64_t(20)), error));
	Extracted zones;
	extract_from_document(*document, zones);
	for (const GraphSymbol &symbol : zones.symbols)
		if (symbol.kind == ReferenceKind::MissionZone)
			TEST_EXPECT(symbol.inert == (symbol.address == row_at(*document, MissionKind::Area, 1)));
	while (document->can_undo()) document->undo();
	// A snapshot serializes the document's bytes and refuses an edit.
	const std::unique_ptr<DocumentBase> snapshot = document->snapshot();
	TEST_EXPECT(snapshot && snapshot->serialize().text == bytes_of(*document) && !snapshot->apply(edit_of(EditOperation::Set, item2, "id", int64_t(5)), error));
	std::printf("reads: %zu entities, %zu areas, %zu paths; a second SSN and zone id inert\n", entities.size(), areas.size(), paths.size());
	return 0;
}

// An item's TYPE rides its symbol (DefCatalogDocument::refine_symbol): the pool a mission's drop
// reads from the graph.
int test_item_type_on_symbol() {
	const DocumentType *type = document_type_for(AssetKind::ItemDefs);
	TEST_EXPECT(type);
	std::unique_ptr<Document> items = records_of(type->make());
	Diagnostic error;
	TEST_EXPECT(items && items->load_bytes(test_io::read_file(std::string(test_paths_repo_root(__FILE__)) + "/fixtures/def/items.def"),
	                                       "items.def", AssetKind::ItemDefs, "jo", error));
	Extracted extracted;
	extract_from_document(*items, extracted);
	bool building = false, person = false;
	for (const GraphSymbol &symbol : extracted.symbols) {
		if (symbol.kind != ReferenceKind::Item) continue;
		if (symbol.display == "106101") building = symbol.value == "5";
		if (symbol.display == "106102") person = symbol.value == "3";
	}
	TEST_EXPECT(building && person);
	return 0;
}

// The validator's record findings (mission_validation.h), each made by one edit of the minted
// mission: the code on the record it concerns, at its severity; the minted mission itself makes
// none. The pool limit is left to the retail leg (no shipped mission crosses it either).
int test_validation() {
	const DocumentType &type = *document_type_for(AssetKind::Mission);
	std::unique_ptr<Document> document = open(fixture_bytes());
	TEST_EXPECT(document && type.validate_file(*document).empty());
	Diagnostic error;
	const auto one = [&](const Edit &edit, const char *code, DiagnosticSeverity severity, const NodeAddress &on,
	                     const char *field) {
		if (!document->apply(edit, error)) {
			std::fprintf(stderr, "%s: the edit is refused: %s\n", code, error.message.c_str());
			return false;
		}
		const std::vector<Diagnostic> findings = type.validate_file(*document);
		document->undo();
		if (findings.size() != 1 || findings[0].code() != code || findings[0].severity != severity ||
		    findings[0].row_id != on.row || findings[0].child_id != on.child || findings[0].field != field) {
			std::fprintf(stderr, "%s: %zu finding(s)%s%s\n", code, findings.size(), findings.empty() ? "" : ", the first ",
			             findings.empty() ? "" : findings[0].code().c_str());
			return false;
		}
		return true;
	};
	const NodeAddress item0 = row_at(*document, MissionKind::Item, 0), organic0 = row_at(*document, MissionKind::Organic, 0);
	const NodeAddress area0 = row_at(*document, MissionKind::Area, 0), area1 = row_at(*document, MissionKind::Area, 1);
	const NodeAddress event0 = row_at(*document, MissionKind::Event, 0), event1 = row_at(*document, MissionKind::Event, 1);
	const NodeAddress trigger0 = first_child(*document, event0, MissionKind::Trigger);
	const NodeAddress action0 = first_child(*document, event0, MissionKind::Action);
	const NodeAddress action1 = first_child(*document, event1, MissionKind::Action);
	const NodeAddress mission = row_at(*document, MissionKind::Mission, 0);
	const NodeAddress path1 = row_at(*document, MissionKind::WaypointPath, 1);
	const NodeAddress stop0 = first_child(*document, path1, MissionKind::Stop);
	const int walker_ssn = static_cast<const EntityRow *>(document->row(organic0.row))->native.id;
	// An SSN a second record carries: the one the lookups find (the organic) stands, the item is noted.
	TEST_EXPECT(one(edit_of(EditOperation::Set, item0, "id", int64_t(walker_ssn)), "mission.ssn_duplicate",
	                DiagnosticSeverity::Warning, item0, "id"));
	TEST_EXPECT(one(edit_of(EditOperation::Set, area1, "id", int64_t(20)), "mission.zone_duplicate", DiagnosticSeverity::Warning,
	                area1, "id"));
	TEST_EXPECT(one(edit_of(EditOperation::Set, area0, "x_max", -150.0), "mission.zone_degenerate", DiagnosticSeverity::Warning,
	                area0, ""));
	TEST_EXPECT(one(edit_of(EditOperation::Set, area1, "id", int64_t(100)), "mission.zone_id", DiagnosticSeverity::Info, area1, "id"));
	TEST_EXPECT(one(edit_of(EditOperation::Set, action0, "param1", int64_t(5)), "mission.event_missing", DiagnosticSeverity::Error,
	                action0, "param1"));
	TEST_EXPECT(one(edit_of(EditOperation::Set, stop0, "marker", int64_t(9)), "mission.marker_missing", DiagnosticSeverity::Warning,
	                stop0, "marker"));
	TEST_EXPECT(one(edit_of(EditOperation::Set, organic0, "waypoint_id", int64_t(2)), "mission.path_empty", DiagnosticSeverity::Info,
	                organic0, "waypoint_id"));
	TEST_EXPECT(one(edit_of(EditOperation::Set, organic0, "wp_number", int64_t(4)), "mission.path_start", DiagnosticSeverity::Warning,
	                organic0, "wp_number"));
	TEST_EXPECT(one(edit_of(EditOperation::Set, organic0, "group", int64_t(70)), "mission.group_range", DiagnosticSeverity::Error,
	                organic0, "group"));
	TEST_EXPECT(one(edit_of(EditOperation::Set, action1, "param1", int64_t(64)), "mission.group_range", DiagnosticSeverity::Error,
	                action1, "param1"));
	TEST_EXPECT(one(edit_of(EditOperation::Set, mission, "attrib_flags",
	                        int64_t(uint32_t(bms::AttribFlags::Coop) | uint32_t(bms::AttribFlags::Deathmatch))),
	                "mission.game_mode", DiagnosticSeverity::Warning, mission, "attrib_flags"));
	TEST_EXPECT(one(edit_of(EditOperation::Set, trigger0, "main_type", int64_t(9)), "mission.trigger_type", DiagnosticSeverity::Warning,
	                trigger0, "main_type"));
	TEST_EXPECT(one(edit_of(EditOperation::Set, trigger0, "sub_type", int64_t(99)), "mission.trigger_type", DiagnosticSeverity::Warning,
	                trigger0, "sub_type"));
	// A path of one stop; a path counted past its slots; a bounding box with a corner past the other:
	// through the file's bytes (a count is never set, a box never made).
	{
		const std::vector<uint8_t> bytes = fixture_bytes();
		bms::File file;
		std::string message;
		TEST_EXPECT(bms::parse(bytes.data(), bytes.size(), file, message));
		// A count past the slots stands only over full slots (mission_detail's resize keeps a shipped
		// over-count that way, as CP19.bms ships one): path 1 filled, then counted 39.
		file.waypoint_records[1].waypoint_numbers.assign(mission::kMaxWaypointPathMarkers, 0);
		file.waypoint_records[1].marker_count = 39;
		file.waypoint_records[2].waypoint_numbers = {0};
		file.waypoint_records[2].marker_count = 1;
		file.bounding_boxes.push_back(bms::BoundingBox{bms::to_fixed_16_16(10.0), 0, 0, 0, 0, 0, 1, -1, 0});
		mission::sync_counts(file);
		std::vector<uint8_t> out;
		TEST_EXPECT(bms::write(file, out, message));
		std::unique_ptr<Document> flawed = open(out, "flawed.bms");
		TEST_EXPECT(flawed);
		std::map<std::string, size_t> codes;
		for (const Diagnostic &d : type.validate_file(*flawed)) ++codes[d.code()];
		TEST_EXPECT(codes.size() == 3 && codes["mission.path_count"] == 1 && codes["mission.path_one_shot"] == 1 &&
		            codes["mission.bounding_box"] == 1);
	}
	std::printf("validation: each record finding on its record\n");
	return 0;
}

// The mission's use check (graph/use_checks.cpp, mission.pool): in a project holding the catalog and
// the minted mission, an entity in another pool than its item's TYPE places it in is a warning on the
// record, read through the graph (the item symbol's value, its TYPE).
int test_pool_check() {
	editor_test::TempProjectDir dir("opennova_mission_pool_check");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Pools"));
	editor_test::create_missing_files(session);
	const std::string root = session.view().project.root;
	const std::string repo = test_paths_repo_root(__FILE__);
	TEST_EXPECT(editor_test::write_bytes(root + "/defs/items.def", test_io::read_file(repo + "/fixtures/def/items.def")));
	// The minted mission with the armory (a building) placed among the items.
	{
		const std::vector<uint8_t> bytes = fixture_bytes();
		bms::File file;
		std::string message;
		TEST_EXPECT(bms::parse(bytes.data(), bytes.size(), file, message));
		file.items[0].type_id = 106101 - mission::kItemIdOffset;
		std::vector<uint8_t> out;
		TEST_EXPECT(bms::write(file, out, message));
		TEST_EXPECT(editor_test::write_bytes(root + "/missions/pools.bms", out));
	}
	editor_test::handle_to_end(session, request::rescan());
	size_t pool = 0;
	for (const Diagnostic &d : session.view().findings.diagnostics) {
		if (d.code() != "mission.pool") continue;
		++pool;
		TEST_EXPECT(d.asset == "missions/pools.bms" && d.severity == DiagnosticSeverity::Warning && d.field == "item" &&
		            d.record_kind == k(MissionKind::Item) && d.message.find("building") != std::string::npos);
	}
	TEST_EXPECT(pool == 1);
	return 0;
}

// What the shipped missions reference (OPENNOVA_JO_DIR, base and each expansion through the VFS): each
// opened as a document, unblocked, its runs canonical (the five with a damaged loadout chunk noted,
// mission_corpus's); the references counted: the entity parameters and those naming no SSN of their
// mission, the zone parameters and those naming no zone, the event references and those past the
// table, the stops and those past the markers, the text keys; and the findings the validator makes
// over the install, by code.
int test_retail() {
	const std::string root = retail::install();
	if (root.empty()) return retail::skip_leg("OPENNOVA_JO_DIR (every shipped mission as a document)");
	std::vector<std::string> expansions = opennova::vfs_list_expansions(root);
	expansions.insert(expansions.begin(), std::string());
	std::set<std::string> seen;
	size_t missions = 0, differing = 0;
	size_t entity_refs = 0, entity_missing = 0, zone_refs = 0, zone_missing = 0, event_refs = 0, event_past = 0,
	       stops = 0, stops_past = 0, text_refs = 0;
	std::map<std::string, size_t> findings_by_code;
	const DocumentType &type = *document_type_for(AssetKind::Mission);
	for (const std::string &expansion : expansions) {
		opennova::Vfs game;
		game.set_scr_policy(opennova::VFS_SCR_FORCE_JO_DFX2);
		TEST_EXPECT(game.mount_game(root, expansion, opennova::VfsMountMode::Packed));
		for (const opennova::VfsFileLocation &file : game.list_files()) {
			if (retail::lower_ascii(std::filesystem::path(file.logical_name).extension().string()) != ".bms") continue;
			if (!seen.insert(file.source_path + "|" + retail::lower_ascii(file.logical_name)).second) continue;
			std::vector<uint8_t> bytes;
			TEST_EXPECT(game.read_file(file.logical_name, bytes));
			std::unique_ptr<Document> document = open(bytes, file.logical_name.c_str());
			TEST_EXPECT(document && !document->blocked());
			if (!document) continue;
			++missions;
			const MissionDocument &m = as_mission(*document);
			for (const MissionFinding code : m.issue_codes()) {
				TEST_EXPECT(code == MissionFinding::RewriteDiffers); // every shipped mission's runs canonical
				++differing;
			}
			std::set<int32_t> ssns, zones;
			for (const MissionEntityRead &entity : mission_entities(m)) ssns.insert(entity.ssn);
			for (const MissionAreaRead &area : mission_areas(m)) zones.insert(area.id);
			const size_t events = m.rows_of(MissionKind::Event).size(), markers = m.rows_of(MissionKind::Marker).size();
			Extracted extracted;
			extract_from_document(*document, extracted);
			for (const GraphEdge &edge : extracted.edges) {
				const std::optional<int> number = opennova::strutil::parse_int(edge.value);
				switch (edge.kind) {
				case ReferenceKind::MissionEntity:
					++entity_refs;
					entity_missing += number && !ssns.count(*number);
					break;
				case ReferenceKind::MissionZone:
					++zone_refs;
					zone_missing += number && !zones.count(*number);
					break;
				case ReferenceKind::MissionEvent:
					++event_refs;
					event_past += number && size_t(*number) >= events;
					break;
				case ReferenceKind::MissionMarker:
					++stops;
					stops_past += number && size_t(*number) >= markers;
					break;
				case ReferenceKind::TextId: ++text_refs; break;
				default: break;
				}
			}
			for (const Diagnostic &d : type.validate_file(*document)) ++findings_by_code[d.code()];
		}
	}
	if (missions == 0) return retail::skip_leg("OPENNOVA_JO_DIR with the game's missions in its archives");
	std::printf("retail: %zu missions opened, %zu with a chunk the writer writes otherwise; %zu entity parameters (%zu naming "
	            "no SSN), %zu zone parameters (%zu naming no zone), %zu event references (%zu past the table), %zu stops (%zu "
	            "past the markers), %zu text keys\n",
	            missions, differing, entity_refs, entity_missing, zone_refs, zone_missing, event_refs, event_past, stops,
	            stops_past, text_refs);
	for (const auto &[code, count] : findings_by_code) std::printf("  %s: %zu\n", code.c_str(), count);
	// What the install holds, measured by this document (the design's emulation counted 3,272 entity
	// parameters with 140 missing, before the waypoint riders and the Redirect actions' entity slot
	// were parameters of that kind; the rest as it counted): no event reference past its table, no
	// stop past the markers, and the findings the validator makes over the shipped missions, one
	// path counted past its slots (CP19), 36 SSNs carried twice, every mission with an entity on an
	// empty path.
	TEST_EXPECT(missions == 115 && differing == 5);
	TEST_EXPECT(entity_refs == 4561 && entity_missing == 162 && zone_refs == 879 && zone_missing == 53);
	TEST_EXPECT(event_refs == 841 && event_past == 0 && stops == 11235 && stops_past == 0 && text_refs == 1428);
	const std::map<std::string, size_t> expected = {{"mission.path_count", 1},  {"mission.path_empty", 115},
	                                                {"mission.path_one_shot", 72}, {"mission.path_start", 95},
	                                                {"mission.rewrite_differs", 5}, {"mission.ssn_duplicate", 36}};
	TEST_EXPECT(findings_by_code == expected);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	if (test_rows() != 0) return 1;
	if (test_references() != 0) return 1;
	if (test_edits() != 0) return 1;
	if (test_parse_findings() != 0) return 1;
	if (test_reads_and_symbols() != 0) return 1;
	if (test_item_type_on_symbol() != 0) return 1;
	if (test_validation() != 0) return 1;
	if (test_pool_check() != 0) return 1;
	return test_retail();
}
