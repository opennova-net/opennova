// The mission document (editor/documents/mission_document.h, ADR 0046 S14) over the minted
// fixtures/bms/synth_logic.bms: the rows in bands with their names, the bytes written back as read;
// every reference a mission makes as a graph edge of its kind (a file's: the terrain, the environment,
// each entity's item, each loadout entry's weapon; a record of its own file by index: a stop's marker,
// an entity's group and path, an Event parameter's event; by id: an entity's SSN and an area's zone id,
// defined as symbols in the mission's own scope and named by a parameter, a zone of 0 too; the text
// keys a record's number forms, in the mission's own table else medmssn.bin: the panel's win rows to
// the first empty slot, an action's chat line, directive or triggered text), the player's SSN naming none, a
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
#include <chrono>
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
#include <editor/documents/mission_file_set.h>
#include <editor/documents/mission_reads.h>
#include <editor/documents/mission_validation.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/reference_kinds.h>
#include <editor/graph/rename_transaction.h>
#include <editor/import/importer.h>
#include <editor/import/sidecar.h>
#include <editor/model/text_document.h>
#include <editor/session/play_controller.h>
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
#include "common/png_test_support.h"
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
// The first edge of `kind` a file of the graph makes; null for none.
const GraphEdge *edge_to_file(const AssetGraph &graph, const std::string &file, ReferenceKind kind) {
	for (const GraphEdge *edge : graph.references_of(file))
		if (edge->kind == kind) return edge;
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
	// By index: the stops' markers, the organics' group and path (0 names none) and the waypoint markers'
	// path (the four on path 1), the KillGroup's group, the ResetEvent's and the Event trigger's event.
	TEST_EXPECT(count_edges(extracted, ReferenceKind::MissionMarker) == 4 && edge(extracted, ReferenceKind::MissionMarker, "3"));
	TEST_EXPECT(count_edges(extracted, ReferenceKind::MissionGroup) == 3 && edge(extracted, ReferenceKind::MissionGroup, "1") &&
	            count_edges(extracted, ReferenceKind::MissionPath) == 5 && edge(extracted, ReferenceKind::MissionPath, "1"));
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
	// The text keys the records' numbers form: the location marker's, the walker's name, the objectives
	// panel's win row, in the mission's own table, else medmssn.bin (never both), none rewritable.
	const GraphEdge *location = edge(extracted, ReferenceKind::TextId, "LOCATION001");
	TEST_EXPECT(location && location->scope == "SYNTH_LOGIC.BIN/Locations" && !location->rewritable &&
	            location->scope_alternate == "MEDMSSN.BIN" && location->scopes_after.empty() &&
	            location->address == row_at(*document, MissionKind::Marker, 4));
	const GraphEdge *name = edge(extracted, ReferenceKind::TextId, "STRNAME001");
	TEST_EXPECT(name && name->scope == "SYNTH_LOGIC.BIN/PeopleNames" && name->field == "name_index" &&
	            name->address == row_at(*document, MissionKind::Organic, 0));
	const NodeAddress header = row_at(*document, MissionKind::Mission, 0);
	const GraphEdge *win = edge(extracted, ReferenceKind::TextId, "STRWINCOND001");
	TEST_EXPECT(win && win->scope == "SYNTH_LOGIC.BIN/WinConditions" && win->field == "win_conditions[0]" &&
	            win->address == header && count_edges(extracted, ReferenceKind::TextId) == 7 &&
	            edge(extracted, ReferenceKind::TextId, "STRWPNAME000")); // and the four waypoint markers' names
	// The panel's rows stop at the first slot of id 0 or 255; an action reads its slot's id: a won or
	// lost subgoal's chat line, a shown subgoal's directive (none when it hides one), a text's line.
	{
		Diagnostic error;
		TEST_EXPECT(document->apply(edit_of(EditOperation::Set, header, "win_conditions[1]", int64_t(0)), error) &&
		            document->apply(edit_of(EditOperation::Set, header, "win_conditions[2]", int64_t(7)), error) &&
		            document->apply(edit_of(EditOperation::Set, header, "lose_conditions[1]", int64_t(4)), error));
		const NodeAddress action = first_child(*document, row_at(*document, MissionKind::Event, 1), MissionKind::Action);
		using Keys = std::vector<std::string>;
		// The keys the action reads, each "scope key"; one malformed edge, or the panel's rows other
		// than slot 1's, answers "bad".
		const auto keys_as = [&](bms::ActionType type, int64_t param1, int64_t param2) {
			if (!document->apply(edit_of(EditOperation::Set, action, "action_type", int64_t(type)), error) ||
			    !document->apply(edit_of(EditOperation::Set, action, "param1", param1), error) ||
			    !document->apply(edit_of(EditOperation::Set, action, "param2", param2), error))
				return Keys{"bad"};
			Extracted read;
			extract_from_document(*document, read);
			Keys keys;
			for (const GraphEdge &e : read.edges)
				if (e.kind == ReferenceKind::TextId && e.address == action) {
					if (e.field != "param1" || e.scope_alternate != "MEDMSSN.BIN" || e.rewritable) return Keys{"bad"};
					keys.push_back(e.scope + " " + e.value);
				}
			// The location's, the person's and the win row's names, and the four waypoint markers'.
			if (count_edges(read, ReferenceKind::TextId) != 7 + keys.size() || edge(read, ReferenceKind::TextId, "STRWINCOND007"))
				return Keys{"bad"};
			return keys;
		};
		TEST_EXPECT(keys_as(bms::ActionType::SubGoalWon, 1, 0) == Keys{"SYNTH_LOGIC.BIN/WinConditions STRWINMSG001"});
		TEST_EXPECT(keys_as(bms::ActionType::SubGoalLost, 2, 0) == Keys{"SYNTH_LOGIC.BIN/LoseConditions STRLOSEMSG004"});
		TEST_EXPECT(keys_as(bms::ActionType::ShowWinSubgoal, 3, 1) == Keys{"SYNTH_LOGIC.BIN/WinConditions STRWINDIRECTIVE007"});
		TEST_EXPECT(keys_as(bms::ActionType::ShowWinSubgoal, 3, 0).empty());
		TEST_EXPECT(keys_as(bms::ActionType::ShowLoseSubgoal, 1, 1) == Keys{"SYNTH_LOGIC.BIN/LoseConditions STRLOSEDIRECTIVE001"});
		TEST_EXPECT(keys_as(bms::ActionType::SubGoalWon, 9, 0).empty());
		TEST_EXPECT(keys_as(bms::ActionType::OutputText, 12, 0) == Keys{"SYNTH_LOGIC.BIN/Triggered Text ID012"});
		while (document->can_undo()) document->undo();
		TEST_EXPECT(bytes_of(*document) == bytes_of(*open(fixture_bytes())));
	}
	// The files the game finds by the mission's name, one edge each from the file itself: the name
	// the reader builds, its alternate or its fallback next; the ones the game runs without optional.
	const auto sidecar = [&](ReferenceKind kind, const char *value, const char *fallback, bool optional, const char *field) {
		const GraphEdge *found = edge(extracted, kind, value);
		return found && found->fallback == fallback && found->optional == optional && found->field == field &&
		       found->record.empty() && !found->rewritable;
	};
	TEST_EXPECT(sidecar(ReferenceKind::MissionStrings, "synth_logic.bin", "medmssn.bin", false, "text"));
	TEST_EXPECT(sidecar(ReferenceKind::Script, "synth_logic.wac", "", true, "script"));
	TEST_EXPECT(sidecar(ReferenceKind::LoadingImage, "synth_logic.pcx", "loadscrn.pcx", true, "loading_image"));
	TEST_EXPECT(sidecar(ReferenceKind::TilePlacement, "synth_logic.til", "", false, "tiles"));
	TEST_EXPECT(sidecar(ReferenceKind::DialogBank, "synth_logic.dbf", "", true, "dialog"));
	TEST_EXPECT(sidecar(ReferenceKind::SoundBank, "synth_logic.lwf", "synth_logic.pwf", false, "dialog_sounds"));
	// The dialog's sounds read only beside its .dbf (review F5); no other row needs one.
	for (const GraphEdge &e : extracted.edges)
		TEST_EXPECT(e.needs == (e.field == "dialog_sounds" && e.record.empty() ? "synth_logic.dbf" : ""));
	TEST_EXPECT(count_edges(extracted, ReferenceKind::DialogBank) == 1);
	// A record that plays a dialog names the dialog its number forms, dlg%03i, in the mission's dialog bank (DI-32),
	// rewritable as that number [orig: Dialog_PlayByIndex @ 0x527ae0]; a dialog of 0 names none.
	{
		Diagnostic error;
		const NodeAddress action = first_child(*document, row_at(*document, MissionKind::Event, 1), MissionKind::Action);
		TEST_EXPECT(document->apply({edit_of(EditOperation::Set, action, "action_type", int64_t(bms::ActionType::PlayWavList)),
		                             edit_of(EditOperation::Set, action, "param1", int64_t(12))},
		                            error));
		Extracted plays;
		extract_from_document(*document, plays);
		size_t dialogs = 0;
		for (const GraphEdge &e : plays.edges)
			if (e.kind == ReferenceKind::Dialog && e.address == action) {
				++dialogs;
				TEST_EXPECT(e.value == "dlg012" && e.scope == "SYNTH_LOGIC.DBF" && e.field == "param1" && e.rewritable &&
				            e.key_prefix == "dlg" && !e.optional);
			}
		TEST_EXPECT(dialogs == 1 && count_edges(plays, ReferenceKind::DialogBank) == 1);
		// The parameter is picked by the bank's dialogs, its number written.
		for (const FieldSchema &schema : document->fields(action.kind))
			if (schema.id == "param1") {
				const FieldUse use = document->field_on(action, schema);
				TEST_EXPECT(use.picks == ReferenceKind::Dialog && use.key_prefix && std::string(use.key_prefix) == "dlg" &&
				            use.scope == "SYNTH_LOGIC.DBF" && use.key_first == 1);
			}
		TEST_EXPECT(document->apply(edit_of(EditOperation::Set, action, "param1", int64_t(0)), error));
		Extracted none;
		extract_from_document(*document, none);
		TEST_EXPECT(count_edges(none, ReferenceKind::Dialog) == 0);
		while (document->can_undo()) document->undo();
	}
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
	// A zone parameter of 0 names a zone like any other (no witness makes 0 name none; the load finds
	// no area of it and neuters the trigger [orig: EventTrigger_ResolveZoneTriggerRefs @0x453000]): an
	// edge, which no area of the file defines.
	TEST_EXPECT(document->apply(edit_of(EditOperation::Set, trigger, "param2", int64_t(0)), error));
	Extracted unzoned;
	extract_from_document(*document, unzoned);
	const GraphEdge *zero = edge(unzoned, ReferenceKind::MissionZone, "0");
	TEST_EXPECT(zero && zero->field == "param2" && zero->address == trigger && zero->scope == "SYNTH_LOGIC.BMS" &&
	            count_edges(unzoned, ReferenceKind::MissionZone) == count_edges(extracted, ReferenceKind::MissionZone));
	for (const GraphSymbol &symbol : unzoned.symbols) TEST_EXPECT(symbol.kind != ReferenceKind::MissionZone || symbol.name != "0");
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
	// The event and the one trigger naming it, the event named first: the trigger still goes first.
	removal.clear();
	const NodeAddress namer = first_child(*document, event1, MissionKind::Trigger);
	TEST_EXPECT(document->removal_edits({event0, namer}, removal, why) && removal.size() == 2 &&
	            removal[0].address == namer && removal[1].address == event0);
	TEST_EXPECT(document->apply(removal, error) && as_mission(*document).rows_of(MissionKind::Event).size() == 1);
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

// The weapon loadout as Save writes it reads back as the same entries (the game's reader takes a
// fourth string as the damage class only when it is a nonzero number or holds no letter): an edit
// that would make it read otherwise is refused; one that keeps it is written and read back the same.
// The fourth string left out (Clear) and written again (Write) round-trips too.
int test_loadout() {
	std::unique_ptr<Document> document = open(fixture_bytes());
	TEST_EXPECT(document);
	const std::string original = bytes_of(*document);
	Diagnostic error;
	const NodeAddress mission = row_at(*document, MissionKind::Mission, 0);
	std::vector<NodeId> entries;
	for (const Document::Collection &collection : document->collections_of(mission))
		if (collection.spec.kind == k(MissionKind::Loadout)) entries = collection.ids;
	TEST_EXPECT(entries.size() == 4);
	if (entries.size() != 4) return 1;
	const auto entry = [&](size_t i) { return NodeAddress{mission.row, k(MissionKind::Loadout), entries[i]}; };
	const auto reads_back = [&](const char *what) {
		const SerializeResult written = document->serialize();
		std::unique_ptr<Document> again = open(std::vector<uint8_t>(written.text.begin(), written.text.end()), "again.bms");
		bms::File mine, theirs;
		const bool same = written.ok() && again && as_mission(*document).compose(mine) && as_mission(*again).compose(theirs) &&
		                  mine.loadout.entries.size() == theirs.loadout.entries.size();
		for (size_t i = 0; same && i < mine.loadout.entries.size(); ++i) {
			const bms::WeaponLoadoutRecord &a = mine.loadout.entries[i], &b = theirs.loadout.entries[i];
			if (a.name != b.name || a.has_flags != b.has_flags || (a.has_flags && a.flags != b.flags)) {
				std::fprintf(stderr, "loadout: %s: entry %zu reads back otherwise\n", what, i + 1);
				return false;
			}
		}
		return same;
	};
	// A damage class with a letter would read as the next entry's name: refused, named.
	TEST_EXPECT(!document->apply(edit_of(EditOperation::Set, entry(0), "flags", std::string("abc")), error) &&
	            error.message.find("Weapon loadout entry 1") != std::string::npos);
	TEST_EXPECT(bytes_of(*document) == original);
	// A number is a damage class: written, read back the same.
	TEST_EXPECT(document->apply(edit_of(EditOperation::Set, entry(0), "flags", std::string("2")), error) && reads_back("a number"));
	// Left out, the entry writes three strings and reads back so; written again, four.
	TEST_EXPECT(document->apply(edit_of(EditOperation::Clear, entry(1), "flags"), error) && reads_back("left out"));
	TEST_EXPECT(document->apply(edit_of(EditOperation::Write, entry(1), "flags"), error) && reads_back("written again"));
	// A three-string entry before a name the reader would take for a damage class (a nonzero number):
	// refused, whichever edit makes it.
	TEST_EXPECT(document->apply(edit_of(EditOperation::Clear, entry(1), "flags"), error));
	TEST_EXPECT(!document->apply(edit_of(EditOperation::Set, entry(2), "name", std::string("7")), error) &&
	            error.message.find("Weapon loadout entry 2") != std::string::npos);
	while (document->can_undo()) document->undo();
	TEST_EXPECT(bytes_of(*document) == original);
	std::printf("loadout: an entry that would read back as another refused, the rest written and read back the same\n");
	return 0;
}

// The clipboard (mission_clipboard.cpp, S14): rows of the three kinds copied together as a mission
// fragment and pasted as rows, each in its band, an SSN or a zone id a row there holds given a fresh
// one with the copies' parameters following it, an event index naming a copied event naming the copy
// wherever the copies land; a nested kind's records pasted into one owner, never another kind's.
int test_clipboard() {
	std::unique_ptr<Document> document = open(fixture_bytes());
	TEST_EXPECT(document);
	const std::string original = bytes_of(*document);
	Diagnostic error;
	const MissionDocument &m = as_mission(*document);
	// The walker (SSN 11, which the first event's trigger names), zone 20 (which it names too) and
	// both events (the first's ResetEvent names the second, the second's Event trigger the first),
	// copied together: a payload of rows.
	const NodeAddress walker = row_at(*document, MissionKind::Organic, 0);
	const NodeAddress zone = row_at(*document, MissionKind::Area, 0);
	const NodeAddress event0 = row_at(*document, MissionKind::Event, 0), event1 = row_at(*document, MissionKind::Event, 1);
	const std::string rows_payload = document->copy({walker, zone, event0, event1});
	TEST_EXPECT(!rows_payload.empty() && document->pastes_rows(rows_payload));
	// A path is never copied; a nested record's payload is no payload of rows.
	TEST_EXPECT(document->copy({walker, row_at(*document, MissionKind::WaypointPath, 1)}).empty());
	const std::string trigger_payload = document->copy({first_child(*document, event0, MissionKind::Trigger)});
	TEST_EXPECT(!trigger_payload.empty() && !document->pastes_rows(trigger_payload));
	// Pasted at the end: the organic after the organics with the next SSN, the area with the lowest
	// free zone id, the events after the events; the copies' parameters follow the fresh ids, and their
	// event indexes name the events they named (the original editor's paste, D-MIS-9).
	Edit paste = edit_of(EditOperation::Paste, {0, 0, 0}, "", rows_payload);
	paste.position = SIZE_MAX;
	TEST_EXPECT(document->apply(paste, error));
	TEST_EXPECT(m.rows_of(MissionKind::Organic).size() == 3 && m.rows_of(MissionKind::Area).size() == 3 &&
	            m.rows_of(MissionKind::Event).size() == 4 && document->rows().size() == 149);
	{
		const EntityRow &copy = static_cast<const EntityRow &>(*m.rows_of(MissionKind::Organic)[2]);
		const AreaRow &area = static_cast<const AreaRow &>(*m.rows_of(MissionKind::Area)[2]);
		const EventRow &first = static_cast<const EventRow &>(*m.rows_of(MissionKind::Event)[2]);
		const EventRow &second = static_cast<const EventRow &>(*m.rows_of(MissionKind::Event)[3]);
		TEST_EXPECT(copy.native.id == 13 && mission::entity_item_id(copy.native) == 106102 && area.native.id == 1);
		TEST_EXPECT(first.native.triggers.size() == 1 && first.native.triggers[0].param1 == 13 && first.native.triggers[0].param2 == 1);
		TEST_EXPECT(first.native.actions.size() == 1 && first.native.actions[0].param1 == 1);
		TEST_EXPECT(second.native.triggers.size() == 1 && second.native.triggers[0].param1 == 0);
		bms::File composed;
		TEST_EXPECT(m.compose(composed) && composed.organics.size() == 3 && composed.area_triggers.size() == 3 &&
		            composed.events.size() == 4 && composed.triggers.size() == 4);
	}
	document->undo();
	TEST_EXPECT(bytes_of(*document) == original);
	// Pasted before the second event: the copies land between the two, and every index at or past the
	// paste moves by the two, the copies' with the rest: the first event's ResetEvent and its copy's
	// follow the second event to its new place, the second's trigger and its copy's name the first.
	paste.position = document->rows().size() - 1;
	TEST_EXPECT(document->apply(paste, error));
	{
		const std::vector<const Node *> events = m.rows_of(MissionKind::Event);
		TEST_EXPECT(events.size() == 4 && events[3]->id == event1.row && events[0]->id == event0.row);
		const EventRow &kept0 = static_cast<const EventRow &>(*events[0]);
		const EventRow &first = static_cast<const EventRow &>(*events[1]);
		const EventRow &second = static_cast<const EventRow &>(*events[2]);
		const EventRow &kept1 = static_cast<const EventRow &>(*events[3]);
		TEST_EXPECT(kept0.native.actions[0].param1 == 3 && kept1.native.triggers[0].param1 == 0);
		TEST_EXPECT(first.native.actions[0].param1 == 3 && second.native.triggers[0].param1 == 0);
	}
	document->undo();
	TEST_EXPECT(bytes_of(*document) == original);
	// Pasted with an item selected (the session's Paste asks for the place after it, in the items'
	// band): each row lands in its band nearest that place, the copies of a band in their order, so
	// the events go first among the events, the first copy before the second, each naming the event its
	// original named where that event went.
	paste.position = 2; // after the mission row and the first item
	TEST_EXPECT(document->apply(paste, error));
	{
		const std::vector<const Node *> events = m.rows_of(MissionKind::Event);
		TEST_EXPECT(events.size() == 4 && events[2]->id == event0.row && events[3]->id == event1.row);
		const EventRow &first = static_cast<const EventRow &>(*events[0]);
		const EventRow &second = static_cast<const EventRow &>(*events[1]);
		const EventRow &kept0 = static_cast<const EventRow &>(*events[2]);
		const EventRow &kept1 = static_cast<const EventRow &>(*events[3]);
		TEST_EXPECT(first.native.actions[0].param1 == 3 && second.native.triggers[0].param1 == 2);
		TEST_EXPECT(kept0.native.actions[0].param1 == 3 && kept1.native.triggers[0].param1 == 2);
		const std::vector<const Node *> organics = m.rows_of(MissionKind::Organic);
		TEST_EXPECT(organics.size() == 3 && static_cast<const EntityRow &>(*organics[0]).native.id == 13);
	}
	document->undo();
	TEST_EXPECT(bytes_of(*document) == original);
	// A rider (a waypoint list of 123..125 beside the SSN of the entity to board [orig:
	// Entity_UpdateInfantryAI @0x4ba9ad]) names that entity: an edge of its own, and a paste of the
	// two follows the boarded one's fresh SSN.
	{
		const NodeAddress item0 = row_at(*document, MissionKind::Item, 0);
		const int32_t item_ssn = static_cast<const EntityRow *>(document->row(item0.row))->native.id;
		TEST_EXPECT(document->apply(edit_of(EditOperation::Set, walker, "waypoint_id", int64_t(123)), error) &&
		            document->apply(edit_of(EditOperation::Set, walker, "wp_number", int64_t(item_ssn)), error));
		Extracted rides;
		extract_from_document(*document, rides);
		bool rider = false;
		for (const GraphEdge &e : rides.edges)
			rider = rider || (e.kind == ReferenceKind::MissionEntity && e.address == walker && e.field == "wp_number" &&
			                  e.value == std::to_string(item_ssn) && e.rewritable);
		TEST_EXPECT(rider);
		Edit both = edit_of(EditOperation::Paste, {0, 0, 0}, "", document->copy({item0, walker}));
		both.position = SIZE_MAX;
		TEST_EXPECT(document->apply(both, error));
		const EntityRow &item_copy = static_cast<const EntityRow &>(*m.rows_of(MissionKind::Item).back());
		const EntityRow &walker_copy = static_cast<const EntityRow &>(*m.rows_of(MissionKind::Organic).back());
		TEST_EXPECT(item_copy.native.id != item_ssn && walker_copy.native.waypoint_id == 123 &&
		            walker_copy.native.wp_number == item_copy.native.id);
		while (document->can_undo()) document->undo();
		TEST_EXPECT(bytes_of(*document) == original);
	}
	// A trigger pasted into the other event (its one owner), a stop pasted into an empty path; the
	// trigger refused by a path, the stop by an event.
	Edit into = edit_of(EditOperation::Paste, {event1.row, k(MissionKind::Trigger), 0}, "", trigger_payload);
	into.position = SIZE_MAX;
	TEST_EXPECT(document->apply(into, error));
	{
		const EventRow &event = static_cast<const EventRow &>(*document->row(event1.row));
		TEST_EXPECT(event.native.triggers.size() == 2 && event.native.triggers[1].main_type == bms::TriggerMainType::Single &&
		            event.native.triggers[1].param1 == 11 && document->last_added_records().size() == 1);
	}
	const NodeAddress path1 = row_at(*document, MissionKind::WaypointPath, 1), path2 = row_at(*document, MissionKind::WaypointPath, 2);
	const std::string stop_payload = document->copy({first_child(*document, path1, MissionKind::Stop)});
	TEST_EXPECT(!stop_payload.empty());
	Edit stop = edit_of(EditOperation::Paste, {path2.row, k(MissionKind::Stop), 0}, "", stop_payload);
	stop.position = SIZE_MAX;
	TEST_EXPECT(document->apply(stop, error));
	{
		const PathRow &path = static_cast<const PathRow &>(*document->row(path2.row));
		TEST_EXPECT(path.native.record.waypoint_numbers == std::vector<uint32_t>({0}) && path.native.record.marker_count == 1);
	}
	Edit wrong = edit_of(EditOperation::Paste, {path2.row, k(MissionKind::Stop), 0}, "", trigger_payload);
	TEST_EXPECT(!document->apply(wrong, error) && error.code() == "document.paste");
	wrong = edit_of(EditOperation::Paste, {event1.row, k(MissionKind::Trigger), 0}, "", stop_payload);
	TEST_EXPECT(!document->apply(wrong, error) && error.code() == "document.paste");
	while (document->can_undo()) document->undo();
	TEST_EXPECT(bytes_of(*document) == original);

	// A Duplicate of an event that re-arms itself: the copy re-arms the original, as the original
	// editor's paste leaves a copy naming the event its original named (D-MIS-9), the original itself,
	// and the second event's trigger names the first where it is.
	const NodeAddress reset = first_child(*document, event0, MissionKind::Action);
	TEST_EXPECT(document->apply(edit_of(EditOperation::Set, reset, "param1", int64_t(0)), error));
	TEST_EXPECT(document->apply(edit_of(EditOperation::Duplicate, event0), error));
	{
		const std::vector<const Node *> events = m.rows_of(MissionKind::Event);
		TEST_EXPECT(events.size() == 3 && events[0]->id == event0.row && events[2]->id == event1.row);
		TEST_EXPECT(static_cast<const EventRow &>(*events[0]).native.actions[0].param1 == 0 &&
		            static_cast<const EventRow &>(*events[1]).native.actions[0].param1 == 0 &&
		            static_cast<const EventRow &>(*events[2]).native.triggers[0].param1 == 0);
	}
	while (document->can_undo()) document->undo();
	TEST_EXPECT(bytes_of(*document) == original);

	// An event index a file holds is never read as anything but an index: one far past the events
	// stays past them across an Add (it names none still), never the new event.
	{
		bms::File file;
		std::string message;
		const std::vector<uint8_t> bytes = fixture_bytes();
		TEST_EXPECT(bms::parse(bytes.data(), bytes.size(), file, message));
		std::vector<mission::EventChain> chains;
		mission::RunReport report;
		TEST_EXPECT(mission::split_event_chains(file, chains, report) && chains.size() == 2);
		chains[0].actions[0].param1 = 0x10000000;
		mission::join_event_chains(chains, file);
		std::vector<uint8_t> far;
		TEST_EXPECT(bms::write(file, far, message));
		std::unique_ptr<Document> past = open(far, "far.bms");
		TEST_EXPECT(past && !past->blocked());
		TEST_EXPECT(past->apply(edit_of(EditOperation::Add, {0, k(MissionKind::Event), 0}), error));
		const EventRow &held = static_cast<const EventRow &>(*as_mission(*past).rows_of(MissionKind::Event)[0]);
		TEST_EXPECT(held.native.actions[0].param1 == 0x10000001);
	}

	// Ninety-nine area triggers hold every zone id: a Duplicate of one is refused (its copy would
	// share the id), as an Add is.
	{
		std::vector<Edit> adds;
		for (int i = 0; i < 97; ++i) adds.push_back(edit_of(EditOperation::Add, {0, k(MissionKind::Area), 0}));
		TEST_EXPECT(document->apply(adds, error) && m.rows_of(MissionKind::Area).size() == 99);
		TEST_EXPECT(!document->apply(edit_of(EditOperation::Add, {0, k(MissionKind::Area), 0}), error));
		TEST_EXPECT(!document->apply(edit_of(EditOperation::Duplicate, zone), error) &&
		            error.message.find("Every zone id 1 to 99 is taken") != std::string::npos);
		while (document->can_undo()) document->undo();
	}
	// No player's net id is given: after an entity of SSN 9999, an Add takes 10256, past the 256 the game
	// stamps on its players (master's mission::ssn_after, D-MIS-10).
	TEST_EXPECT(document->apply(edit_of(EditOperation::Set, walker, "id", int64_t(9999)), error));
	TEST_EXPECT(document->apply(edit_of(EditOperation::Add, {0, k(MissionKind::Item), 0}), error) &&
	            static_cast<const EntityRow *>(document->row(document->last_added()))->native.id == 10256);
	while (document->can_undo()) document->undo();
	TEST_EXPECT(bytes_of(*document) == original);
	std::printf("clipboard: rows told apart and placed, a nested kind into its owner\n");
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
	// Undone, each state's first holders made again (MissionDocument::first_holders, per revision): none.
	while (document->can_undo()) document->undo();
	Extracted undone;
	extract_from_document(*document, undone);
	for (const GraphSymbol &symbol : undone.symbols)
		if (symbol.kind == ReferenceKind::MissionEntity || symbol.kind == ReferenceKind::MissionZone) TEST_EXPECT(!symbol.inert);
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
	// An SSN a second record carries: the one the lookups find (the organic) stands, the item is noted,
	// worded by what reaches it (the lookups by SSN never, an area check every organic and item).
	TEST_EXPECT(one(edit_of(EditOperation::Set, item0, "id", int64_t(walker_ssn)), "mission.ssn_duplicate",
	                DiagnosticSeverity::Warning, item0, "id"));
	{
		TEST_EXPECT(document->apply(edit_of(EditOperation::Set, item0, "id", int64_t(walker_ssn)), error));
		const std::vector<Diagnostic> found = type.validate_file(*document);
		document->undo();
		TEST_EXPECT(found.size() == 1 && found[0].message.find("never this, but an area check (SingleIsWithinArea) tests "
		                                                       "every organic and item carrying it, this one too.") !=
		                                         std::string::npos);
	}
	// An SSN a lookup scanning fewer pools finds no row of: an alive test's only a marker carries (the
	// test never scans the markers: SingleAlive false, SingleDestroyed true), a ChangeSingleAI's too, a
	// teammate operation's patient an item carries (the patient is looked up among the organics).
	{
		const int marker_ssn = static_cast<const EntityRow *>(document->row(row_at(*document, MissionKind::Marker, 0).row))->native.id;
		const int item_ssn = static_cast<const EntityRow *>(document->row(item0.row))->native.id;
		const auto unscanned = [&](const NodeAddress &on, const char *needle) {
			const std::vector<Diagnostic> found = type.validate_file(*document);
			return found.size() == 1 && found[0].code() == "mission.ssn_unscanned" &&
			       found[0].severity == DiagnosticSeverity::Warning && found[0].child_id == on.child &&
			       found[0].field == "param1" && found[0].message.find(needle) != std::string::npos;
		};
		TEST_EXPECT(document->apply(edit_of(EditOperation::Set, trigger0, "sub_type", int64_t(bms::SingleTriggerType::SingleAlive)), error) &&
		            document->apply(edit_of(EditOperation::Set, trigger0, "param1", int64_t(marker_ssn)), error));
		TEST_EXPECT(unscanned(trigger0, "among the organics, items and buildings alone: SingleAlive reads false"));
		// The holding test on an item's SSN: it scans the organics alone.
		TEST_EXPECT(document->apply(edit_of(EditOperation::Set, trigger0, "sub_type", int64_t(bms::SingleTriggerType::SingleHoldingGroup)), error) &&
		            document->apply(edit_of(EditOperation::Set, trigger0, "param1", int64_t(item_ssn)), error));
		TEST_EXPECT(unscanned(trigger0, "among the organics alone: it reads false."));
		// On the walker's SSN it reads as written: no finding.
		TEST_EXPECT(document->apply(edit_of(EditOperation::Set, trigger0, "param1", int64_t(walker_ssn)), error) &&
		            type.validate_file(*document).empty());
		TEST_EXPECT(document->apply(edit_of(EditOperation::Set, action1, "action_type", int64_t(bms::ActionType::ChangeSingleAI)), error) &&
		            document->apply(edit_of(EditOperation::Set, action1, "action_sub_type", int64_t(1)), error) &&
		            document->apply(edit_of(EditOperation::Set, action1, "param1", int64_t(marker_ssn)), error));
		TEST_EXPECT(unscanned(action1, "it finds no record and does nothing."));
		TEST_EXPECT(document->apply(edit_of(EditOperation::Set, action1, "action_type", int64_t(bms::ActionType::Teammates)), error) &&
		            document->apply(edit_of(EditOperation::Set, action1, "param1", int64_t(item_ssn)), error));
		TEST_EXPECT(unscanned(action1, "among the organics alone: it finds no patient"));
		TEST_EXPECT(document->apply(edit_of(EditOperation::Set, action1, "param1", int64_t(walker_ssn)), error) &&
		            type.validate_file(*document).empty());
		while (document->can_undo()) document->undo();
	}
	// Event's and SecondTimeThrough's cases read no sub-type: any is theirs, and the Inspector shows it
	// unread.
	{
		TEST_EXPECT(document->apply(edit_of(EditOperation::Set, trigger0, "main_type", int64_t(bms::TriggerMainType::SecondTimeThrough)), error) &&
		            document->apply(edit_of(EditOperation::Set, trigger0, "sub_type", int64_t(3)), error));
		TEST_EXPECT(type.validate_file(*document).empty());
		FieldUse sub;
		for (const FieldSchema &schema : document->fields(trigger0.kind))
			if (schema.id == "sub_type") sub = document->field_on(trigger0, schema);
		TEST_EXPECT(sub.applies == Applicability::Ignored);
		while (document->can_undo()) document->undo();
	}
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
	// No game mode bit: single player, whose player starts at a 6094 or 6001 marker, the minted mission
	// placing neither (the player at the map's origin); a marker made a 6001 start clears it.
	TEST_EXPECT(one(edit_of(EditOperation::Set, mission, "attrib_flags", int64_t(0)), "mission.no_start", DiagnosticSeverity::Warning,
	                mission, "attrib_flags"));
	{
		const NodeAddress marker0 = row_at(*document, MissionKind::Marker, 0);
		TEST_EXPECT(document->apply({edit_of(EditOperation::Set, mission, "attrib_flags", int64_t(0)),
		                             edit_of(EditOperation::Set, marker0, "item", int64_t(106001))},
		                            error));
		TEST_EXPECT(type.validate_file(*document).empty());
		document->undo();
	}
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
	int64_t own_item = 0;
	{
		const std::vector<uint8_t> bytes = fixture_bytes();
		bms::File file;
		std::string message;
		TEST_EXPECT(bms::parse(bytes.data(), bytes.size(), file, message));
		own_item = mission::kItemIdOffset + file.items[0].type_id;
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
	// The record set back to its own item: the finding goes with the graph's next state and comes back
	// with the undo (the check's findings are made once per state of the graph, not per composition).
	editor_test::handle_to_end(session, request::open_document("missions/pools.bms"));
	Document *mission = session.document_for("missions/pools.bms");
	TEST_EXPECT(mission);
	if (!mission) return 1;
	const auto pools = [&]() {
		size_t n = 0;
		for (const Diagnostic &d : session.view().findings.diagnostics) n += d.code() == "mission.pool" ? 1 : 0;
		return n;
	};
	editor_test::handle_to_end(session, request::edit_record(mission->path(), edit_of(EditOperation::Set,
	                                                                                  row_at(*mission, MissionKind::Item, 0),
	                                                                                  "item", own_item)));
	TEST_EXPECT(session.last_edit_ok() && pools() == 0);
	editor_test::handle_to_end(session, request::undo(mission->path()));
	TEST_EXPECT(pools() == 1);
	return 0;
}

// A mission renamed takes the files the game finds by its name that the project has (ADR 0046 S14):
// the plan lists them as companions (its own derived edges rewriting nothing), the commit moves
// them with it (an open companion with unsaved edits joining the unsaved prompt, then reopened at
// its new name), and the renamed mission's set resolves under the new names; a stale file of the new
// name and an imported companion refuse it.
int test_rename_companions() {
	editor_test::TempProjectDir dir("opennova_mission_rename_companions");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Companions"));
	editor_test::create_missing_files(session);
	const std::string root = session.view().project.root;
	const std::string repo = test_paths_repo_root(__FILE__);
	TEST_EXPECT(editor_test::write_bytes(root + "/missions/walk.bms", fixture_bytes()));
	TEST_EXPECT(editor_test::write_bytes(root + "/missions/walk.bin", test_io::read_file(repo + "/fixtures/bms/synth_logic.bin")));
	TEST_EXPECT(editor_test::write_text(root + "/missions/walk.wac", "; the walk\r\n"));
	editor_test::handle_to_end(session, request::rescan());
	const SessionView &view = session.view();
	const AssetGraph &graph = *view.findings.graph;
	const GraphEdge *table = edge_to_file(graph, "missions/walk.bms", ReferenceKind::MissionStrings);
	TEST_EXPECT(table && graph.resolve(*table) == ReferenceStatus::Present);
	TEST_EXPECT(graph.resolve(*edge_to_file(graph, "missions/walk.bms", ReferenceKind::Script)) == ReferenceStatus::Present);
	// Of the set, the optional sidecars the project lacks make no finding; the tile placement, which
	// every shipped mission has, is a warning (the fixture's terrain, environment, items and weapons,
	// which this project lacks too, are the records' own missing references).
	size_t missing_sidecars = 0;
	for (const Diagnostic &d : view.findings.diagnostics)
		if (d.code() == "reference.missing" && d.asset == "missions/walk.bms" && mission_file_set_row(d.field)) {
			++missing_sidecars;
			TEST_EXPECT(d.field == "tiles" && d.severity == DiagnosticSeverity::Warning);
		}
	TEST_EXPECT(missing_sidecars == 1);
	const RenamePlan plan = plan_rename(ProjectPaths::for_root(root), *view.project.scan, graph, "walk.bms", "run.bms");
	TEST_EXPECT(plan.ok() && plan.companions.size() == 2 && plan.companions[0].old_name == "walk.bin" &&
	            plan.companions[0].new_name == "run.bin" && plan.companions[1].new_name == "run.wac");
	TEST_EXPECT(!plan_rename(ProjectPaths::for_root(root), *view.project.scan, graph, "walk.bms", "a_name_far_too_long.bms").ok());
	// A file the game would find by the new name, of no mission (a stale script), refuses the rename:
	// the renamed mission would take it as its own.
	TEST_EXPECT(editor_test::write_text(root + "/missions/jog.wac", "; a stale one\r\n"));
	editor_test::handle_to_end(session, request::rescan());
	{
		const RenamePlan stale = plan_rename(ProjectPaths::for_root(root), *view.project.scan, *view.findings.graph, "walk.bms", "jog.bms");
		TEST_EXPECT(!stale.ok() && stale.refusals.size() == 1 && stale.refusals[0].code() == "rename.exists" &&
		            stale.refusals[0].message.find("jog.wac") != std::string::npos);
	}
	// The mission's script open with unsaved edits: the rename waits on the unsaved prompt, which lists
	// it (the commit moves its file); saved there, the rename moves it and the document follows to the
	// new name, active, its edit in the file.
	editor_test::handle_to_end(session, request::open_document("walk.wac"));
	editor_test::handle_to_end(session, request::edit_record("missions/walk.wac", {TextDocument::replace(TextSpan{1, 1, 0}, "X")}));
	TEST_EXPECT(session.document_base_for("missions/walk.wac") && session.document_base_for("missions/walk.wac")->dirty());
	session.handle(request::rename_asset("walk.bms", "run.bms"));
	TEST_EXPECT(session.outcome().unsaved_prompt &&
	            view.dialogs.unsaved_prompt.files == std::vector<std::string>({"missions/walk.wac"}));
	editor_test::handle_to_end(session, request::resolve_unsaved(UnsavedChoice::Save));
	namespace fs = std::filesystem;
	TEST_EXPECT(fs::exists(root + "/missions/run.bms") && fs::exists(root + "/missions/run.bin") && fs::exists(root + "/missions/run.wac"));
	TEST_EXPECT(!fs::exists(root + "/missions/walk.bms") && !fs::exists(root + "/missions/walk.bin") && !fs::exists(root + "/missions/walk.wac"));
	TEST_EXPECT(!session.document_base_for("missions/walk.wac") && session.document_base_for("missions/run.wac") &&
	            !session.document_base_for("missions/run.wac")->dirty() && view.documents.active == "missions/run.wac");
	TEST_EXPECT(test_io::read_file_text(root + "/missions/run.wac") == "X; the walk\r\n");
	const GraphEdge *renamed = edge_to_file(*view.findings.graph, "missions/run.bms", ReferenceKind::MissionStrings);
	TEST_EXPECT(renamed && renamed->value == "run.bin" && view.findings.graph->resolve(*renamed) == ReferenceStatus::Present);
	// A companion the import pass makes (the loading image, out of a PNG source) refuses the rename:
	// the next import would make it again under the old name; its source is the file to rename.
	{
		TEST_EXPECT(editor_test::write_bytes(root + "/art/run.png", test_png::gradient_png(8, 8)));
		const Importer *importer = importer_for(root + "/art/run.png");
		TEST_EXPECT(importer);
		if (!importer) return 1;
		ImportSidecar sidecar;
		sidecar.importer = importer->id;
		sidecar.version = importer->version;
		// A PCX, the 8-bit indexed file these sources make (the image importer's format option; its default
		// is a 32-bit TGA).
		sidecar.options = {{"format", "pcx"}};
		Diagnostic error;
		TEST_EXPECT(save_import_sidecar(root + "/art/run.png.import", sidecar, error));
		editor_test::handle_to_end(session, request::rescan());
		const AssetEntry *image = view.project.scan->find("run.pcx");
		TEST_EXPECT(image && image->imported_from == "art/run.png");
		const RenamePlan imported =
		        plan_rename(ProjectPaths::for_root(root), *view.project.scan, *view.findings.graph, "run.bms", "ride.bms");
		TEST_EXPECT(!imported.ok() && imported.refusals.size() == 1 && imported.refusals[0].code() == "rename.imported" &&
		            imported.refusals[0].message.find("run.pcx") != std::string::npos);
	}
	return 0;
}

// A mission named like a game sound bank (menu.bms beside the menus' menu.lwf), with no dialog bank:
// the game reads <stem>.lwf only beside <stem>.dbf (review F5), so the mission's dialog sounds name no
// file: the bank is not the mission's (no use of it, no companion of a rename); with a .dbf it is.
int test_mission_named_like_a_bank() {
	editor_test::TempProjectDir dir("opennova_mission_named_like_a_bank");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Banks"));
	const std::string root = session.view().project.root;
	TEST_EXPECT(editor_test::write_bytes(root + "/missions/menu.bms", fixture_bytes()));
	TEST_EXPECT(editor_test::write_text(root + "/sounds/menu.lwf", "lwf"));
	editor_test::handle_to_end(session, request::rescan());
	const SessionView &view = session.view();
	const auto sounds = [&view]() -> const GraphEdge * {
		for (const GraphEdge *edge : view.findings.graph->references_of("missions/menu.bms"))
			if (edge->field == "dialog_sounds" && edge->record.empty()) return edge;
		return nullptr;
	};
	const auto users = [&view]() {
		size_t n = 0;
		for (const GraphEdge *edge : view.findings.graph->referrers_of_file("sounds/menu.lwf"))
			n += edge->source == "missions/menu.bms" ? 1 : 0;
		return n;
	};
	const auto takes_bank = [&](const RenamePlan &plan) {
		for (const auto &companion : plan.companions)
			if (companion.old_name == "menu.lwf") return true;
		return false;
	};
	TEST_EXPECT(sounds() && sounds()->needs == "menu.dbf" &&
	            view.findings.graph->resolve(*sounds()) == ReferenceStatus::NotAReference && users() == 0);
	RenamePlan plan = plan_rename(ProjectPaths::for_root(root), *view.project.scan, *view.findings.graph, "menu.bms", "walk.bms");
	TEST_EXPECT(plan.ok() && !takes_bank(plan));
	// With its dialog bank, the bank of its name is its dialog's sounds.
	TEST_EXPECT(editor_test::write_text(root + "/missions/menu.dbf", "dbf"));
	editor_test::handle_to_end(session, request::rescan());
	TEST_EXPECT(sounds() && view.findings.graph->resolve(*sounds()) == ReferenceStatus::Present && users() == 1);
	plan = plan_rename(ProjectPaths::for_root(root), *view.project.scan, *view.findings.graph, "menu.bms", "walk.bms");
	TEST_EXPECT(plan.ok() && takes_bank(plan));
	// With its dialog bank and no sounds of its name, the sounds are missing (the dialogs play silent):
	// a warning, the sound bank kind's, no longer an optional row (the data lane's m13).
	std::filesystem::remove(root + "/sounds/menu.lwf");
	editor_test::handle_to_end(session, request::rescan());
	size_t silent = 0;
	for (const Diagnostic &d : view.findings.diagnostics)
		if (d.code() == "reference.missing" && d.asset == "missions/menu.bms" && d.field == "dialog_sounds")
			silent += d.severity == DiagnosticSeverity::Warning ? 1 : 100;
	TEST_EXPECT(silent == 1);
	return 0;
}

// A mission named with an inner dot finds its files by its name to the first dot, as the game's
// readers cut it (review F8): op.v2.bms's script is op.wac, which no note calls unused, a rename takes
// it, and Play mission from op.wac starts op.v2.bms.
int test_mission_first_dot() {
	editor_test::TempProjectDir dir("opennova_mission_first_dot");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Dots"));
	const std::string root = session.view().project.root;
	TEST_EXPECT(editor_test::write_bytes(root + "/missions/op.v2.bms", fixture_bytes()));
	TEST_EXPECT(editor_test::write_text(root + "/missions/op.wac", "; the op\r\n"));
	editor_test::handle_to_end(session, request::rescan());
	const SessionView &view = session.view();
	const GraphEdge *script = edge_to_file(*view.findings.graph, "missions/op.v2.bms", ReferenceKind::Script);
	TEST_EXPECT(script && script->value == "op.wac" && view.findings.graph->resolve(*script) == ReferenceStatus::Present);
	for (const Diagnostic &d : view.findings.diagnostics) TEST_EXPECT(d.code() != "mission.sidecar.unused");
	const RenamePlan plan = plan_rename(ProjectPaths::for_root(root), *view.project.scan, *view.findings.graph, "op.v2.bms", "raid.bms");
	TEST_EXPECT(plan.ok() && plan.companions.size() == 1 && plan.companions[0].old_name == "op.wac" &&
	            plan.companions[0].new_name == "raid.wac");
	editor_test::handle_to_end(session, request::open_document("op.wac"));
	TEST_EXPECT(play_mission_for(view) == "op.v2.bms");
	return 0;
}

// What the shipped missions reference (OPENNOVA_JO_DIR, base and each expansion through the VFS): each
// opened as a document, unblocked, its runs canonical (the five with a damaged loadout chunk noted,
// mission_corpus's); the references counted: the entity parameters and those naming no SSN of their
// mission, the zone parameters and those naming no zone, the event references and those past the
// table, the stops and those past the markers, the text keys by key; and the findings the validator
// makes over the install, by code.
// ADR 0046 S23 D, the rules the witnesses settled: a bounding box made from nothing is a type-0 box, its
// value worded and read by its type (D-MIS-8); a waypoint name read on the two waypoint marker types
// alone; a type-6005 marker's advance trigger an event of the mission (its edge, renumbered with the
// events, cleared when its event goes, one past the events a finding); and where a player of the
// single-player, a solo and a team mode starts (mission.no_start).
int test_witnessed_rules() {
	const DocumentType &type = *document_type_for(AssetKind::Mission);
	std::unique_ptr<Document> document = open(fixture_bytes());
	TEST_EXPECT(document != nullptr);
	if (!document) return 1;
	const MissionDocument &m = as_mission(*document);
	const std::string original = bytes_of(*document);
	Diagnostic error;
	const auto use = [&](const NodeAddress &record, const char *id) {
		for (const FieldSchema &schema : document->fields(record.kind))
			if (schema.id == id) return document->field_on(record, schema);
		return FieldUse();
	};
	const auto codes = [&]() {
		std::map<std::string, size_t> out;
		for (const Diagnostic &d : type.validate_file(*document)) ++out[d.code()];
		return out;
	};

	// A box from nothing, of type 0: its value read by nothing until its type is one the walk reads.
	const NodeAddress mission_row = row_at(*document, MissionKind::Mission, 0);
	TEST_EXPECT(document->apply(edit_of(EditOperation::Add, {mission_row.row, k(MissionKind::BoundingBox), 0}), error));
	const NodeAddress box = first_child(*document, mission_row, MissionKind::BoundingBox);
	TEST_EXPECT(box.child != 0 && use(box, "type").applies == Applicability::Reads &&
	            use(box, "ref_id").applies == Applicability::Ignored);
	TEST_EXPECT(document->apply(edit_of(EditOperation::Set, box, "type", int64_t(1)), error) &&
	            use(box, "ref_id").applies == Applicability::Reads && use(box, "ref_id").label &&
	            std::string(use(box, "ref_id").label) == "Health per tick");
	TEST_EXPECT(document->apply(edit_of(EditOperation::Set, box, "type", int64_t(5)), error) &&
	            std::string(use(box, "ref_id").label) == "Location");
	// A Mission box's value is the first four characters of the mission the player leaves for [orig:
	// Entity_UpdateInfantryPlayerBody @0x4b60b6..0x4b60c4]; a type no case of the walk reads, nothing.
	TEST_EXPECT(document->apply(edit_of(EditOperation::Set, box, "type", int64_t(3)), error) &&
	            use(box, "ref_id").applies == Applicability::Reads &&
	            std::string(use(box, "ref_id").label) == "Mission name (its first four characters)");
	TEST_EXPECT(document->apply(edit_of(EditOperation::Set, box, "type", int64_t(9)), error) &&
	            use(box, "ref_id").applies == Applicability::Ignored);
	while (document->can_undo()) document->undo();
	TEST_EXPECT(bytes_of(*document) == original);

	// A waypoint marker (item 106005): its name and its advance trigger read; another marker's not.
	const NodeAddress marker0 = row_at(*document, MissionKind::Marker, 0), navpoint = row_at(*document, MissionKind::Marker, 4);
	TEST_EXPECT(use(marker0, "ttool_index").applies == Applicability::Ignored &&
	            use(marker0, "wp_adv_trigger").applies == Applicability::Ignored);
	TEST_EXPECT(document->apply(edit_of(EditOperation::Set, marker0, "item", int64_t(106005)), error));
	TEST_EXPECT(use(marker0, "ttool_index").applies == Applicability::Reads &&
	            use(marker0, "wp_adv_trigger").applies == Applicability::Reads &&
	            use(navpoint, "ttool_index").applies == Applicability::Ignored);
	// -1 (a new record's) names no event; 1 names the second.
	TEST_EXPECT(use(marker0, "wp_adv_trigger").reference == ReferenceKind::None);
	Extracted before;
	extract_from_document(*document, before);
	TEST_EXPECT(document->apply(edit_of(EditOperation::Set, marker0, "wp_adv_trigger", int64_t(1)), error) &&
	            use(marker0, "wp_adv_trigger").reference == ReferenceKind::MissionEvent);
	Extracted after;
	extract_from_document(*document, after);
	TEST_EXPECT(count_edges(after, ReferenceKind::MissionEvent) == count_edges(before, ReferenceKind::MissionEvent) + 1);
	const auto advance = [&]() { return static_cast<const EntityRow &>(*m.rows_of(MissionKind::Marker)[0]).native.wp_adv_trigger; };
	// A Duplicate of the first event puts the copy before the second: the marker follows its event.
	const NodeAddress event0 = row_at(*document, MissionKind::Event, 0), event1 = row_at(*document, MissionKind::Event, 1);
	TEST_EXPECT(document->apply(edit_of(EditOperation::Duplicate, event0), error) && advance() == 2);
	document->undo();
	TEST_EXPECT(advance() == 1);
	// A removal of its event clears it first, as the original editor's delete does (WP_EVENT_DELETED).
	std::vector<Edit> removal;
	std::string why;
	TEST_EXPECT(document->removal_edits({event0, event1}, removal, why) && !removal.empty() &&
	            removal[0].operation == EditOperation::Set && removal[0].address.row == marker0.row &&
	            removal[0].field == "wp_adv_trigger" && removal[0].value == Value(int64_t(-1)));
	// One past the events: no event of it fires, a warning on the marker.
	TEST_EXPECT(document->apply(edit_of(EditOperation::Set, marker0, "wp_adv_trigger", int64_t(7)), error));
	{
		const std::vector<Diagnostic> findings = type.validate_file(*document);
		TEST_EXPECT(findings.size() == 1 && findings[0].code() == "mission.event_missing" &&
		            findings[0].severity == DiagnosticSeverity::Warning && findings[0].row_id == marker0.row &&
		            findings[0].field == "wp_adv_trigger");
	}
	// 0 names event 1, which the game never completes a waypoint on, and turns the proximity advance off
	// [orig: Player_UpdatePerFrame @0x4de650; EventTrigger_MarkLinkedSpawnPoints @0x452d34, `> 0`]: an event
	// reference, a warning, and renumbered as any index [orig: JOTACmed.exe sub_411C90 @ 0x411dd8..0x411ded].
	TEST_EXPECT(document->apply(edit_of(EditOperation::Set, marker0, "wp_adv_trigger", int64_t(0)), error) &&
	            use(marker0, "wp_adv_trigger").reference == ReferenceKind::MissionEvent);
	{
		const std::vector<Diagnostic> findings = type.validate_file(*document);
		TEST_EXPECT(findings.size() == 1 && findings[0].code() == "mission.event_missing" &&
		            findings[0].message.find("never completes a waypoint") != std::string::npos);
	}
	{
		size_t at = 0;
		for (size_t i = 0; i < document->rows().size(); ++i)
			if (document->rows()[i]->id == event0.row) at = i;
		Edit up = edit_of(EditOperation::Move, event1);
		up.position = at;
		TEST_EXPECT(document->apply(up, error) && advance() == 1);
	}
	while (document->can_undo()) document->undo();
	TEST_EXPECT(bytes_of(*document) == original);

	// The starts: the minted mission is co-op (a spawn vehicle may start its players: unchecked here); with
	// no game mode its player starts at a 6094 or a 6001 marker, which it lacks; a deathmatch's at a 6095 or
	// a 6002; a team deathmatch's team 1 at a 6096 or a 6003, team 2 at a 6097 or a 6004.
	const auto mode = [&](uint32_t bits) {
		return document->apply(edit_of(EditOperation::Set, mission_row, "attrib_flags", int64_t(bits)), error);
	};
	TEST_EXPECT(codes().empty());
	TEST_EXPECT(mode(0) && codes() == (std::map<std::string, size_t>{{"mission.no_start", 1}}));
	TEST_EXPECT(mode(uint32_t(bms::AttribFlags::Deathmatch)) && codes() == (std::map<std::string, size_t>{{"mission.no_start", 1}}));
	TEST_EXPECT(mode(uint32_t(bms::AttribFlags::TeamDeathmatch)) &&
	            codes() == (std::map<std::string, size_t>{{"mission.no_start", 2}}));
	// A team 1 start: team 2's alone.
	TEST_EXPECT(document->apply(edit_of(EditOperation::Set, marker0, "item", int64_t(106003)), error) &&
	            codes() == (std::map<std::string, size_t>{{"mission.no_start", 1}}));
	while (document->can_undo()) document->undo();
	TEST_EXPECT(bytes_of(*document) == original);
	std::printf("witnessed rules: a box from nothing worded by its type, a waypoint's name and advance trigger, the "
	            "starts of each mode\n");
	return 0;
}

int test_retail() {
	const std::string root = retail::install();
	if (root.empty()) return retail::skip_leg("OPENNOVA_JO_DIR (every shipped mission as a document)");
	std::vector<std::string> expansions = opennova::vfs_list_expansions(root);
	expansions.insert(expansions.begin(), std::string());
	std::set<std::string> seen;
	size_t missions = 0, differing = 0;
	size_t entity_refs = 0, entity_missing = 0, zone_refs = 0, zone_missing = 0, event_refs = 0, event_past = 0,
	       stops = 0, stops_past = 0, text_refs = 0;
	std::map<std::string, size_t> findings_by_code, text_by_key;
	// What the extractions took (timed, not held to a bound): all of them, and the slowest with its rows.
	double extraction_ms = 0, slowest_ms = 0;
	size_t slowest_rows = 0;
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
			const auto started = std::chrono::steady_clock::now();
			extract_from_document(*document, extracted);
			const double took = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
			extraction_ms += took;
			if (took > slowest_ms) {
				slowest_ms = took;
				slowest_rows = document->rows().size();
			}
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
				case ReferenceKind::TextId:
					++text_refs;
					++text_by_key[edge.value.substr(0, edge.value.find_first_of("0123456789"))];
					break;
				default: break;
				}
			}
			for (const Diagnostic &d : type.validate_file(*document)) {
				++findings_by_code[d.code()];
				if (d.code() == "mission.ssn_unscanned")
					std::printf("  %s, %s: %s\n", file.logical_name.c_str(), d.record.c_str(), d.message.c_str());
			}
		}
	}
	if (missions == 0) return retail::skip_leg("OPENNOVA_JO_DIR with the game's missions in its archives");
	std::printf("retail: %zu missions opened, %zu with a chunk the writer writes otherwise; %zu entity parameters (%zu naming "
	            "no SSN), %zu zone parameters (%zu naming no zone), %zu event references (%zu past the table), %zu stops (%zu "
	            "past the markers), %zu text keys\n",
	            missions, differing, entity_refs, entity_missing, zone_refs, zone_missing, event_refs, event_past, stops,
	            stops_past, text_refs);
	std::printf("retail: the %zu extractions took %.1f ms, the slowest %.1f ms (%zu rows)\n", missions, extraction_ms,
	            slowest_ms, slowest_rows);
	for (const auto &[key, count] : text_by_key) std::printf("  %s%%03i: %zu\n", key.c_str(), count);
	for (const auto &[code, count] : findings_by_code) std::printf("  %s: %zu\n", code.c_str(), count);
	// What the install holds, measured by this document (the design's emulation counted 3,272 entity
	// parameters with 140 missing, before the waypoint riders and the Redirect actions' entity slot
	// were parameters of that kind; the rest as it counted): no event reference past its table, no
	// stop past the markers, and the findings the validator makes over the shipped missions, one
	// path counted past its slots (CP19), 36 SSNs carried twice, one alive test on a marker's SSN
	// (CP13's SSN 2072), every mission with an entity on an empty path.
	TEST_EXPECT(missions == 115 && differing == 5);
	TEST_EXPECT(entity_refs == 4561 && entity_missing == 162 && zone_refs == 879 && zone_missing == 53);
	// The event references: 841 Event triggers' and ResetEvent actions' and 73 type-6005 waypoints' advance
	// triggers.
	TEST_EXPECT(event_refs == 914 && event_past == 0 && stops == 11235 && stops_past == 0 && text_refs == 12824);
	// The text keys by key: no shipped action outputs a triggered text; each of the 11,250 type-6005 waypoints
	// forms its name's, 822 of them of id -1 ("STRWPNAME-01", the key the spawn forms of it).
	const std::map<std::string, size_t> keys = {{"LOCATION", 581},     {"STRLOSEDIRECTIVE", 17}, {"STRLOSEMSG", 29},
	                                            {"STRNAME", 544},      {"STRWINCOND", 130},      {"STRWINDIRECTIVE", 133},
	                                            {"STRWINMSG", 140},      {"STRWPNAME", 10428},     {"STRWPNAME-", 822}};
	TEST_EXPECT(text_by_key == keys);
	const std::map<std::string, size_t> expected = {{"mission.path_count", 1},      {"mission.path_empty", 115},
	                                                {"mission.path_one_shot", 72},   {"mission.path_start", 95},
	                                                {"mission.rewrite_differs", 5}, {"mission.ssn_duplicate", 36},
	                                                {"mission.ssn_unscanned", 1}};
	TEST_EXPECT(findings_by_code == expected);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	if (test_rows() != 0) return 1;
	if (test_references() != 0) return 1;
	if (test_edits() != 0) return 1;
	if (test_loadout() != 0) return 1;
	if (test_clipboard() != 0) return 1;
	if (test_parse_findings() != 0) return 1;
	if (test_reads_and_symbols() != 0) return 1;
	if (test_item_type_on_symbol() != 0) return 1;
	if (test_validation() != 0) return 1;
	if (test_pool_check() != 0) return 1;
	if (test_rename_companions() != 0) return 1;
	if (test_mission_named_like_a_bank() != 0) return 1;
	if (test_witnessed_rules() != 0) return 1;
	if (test_mission_first_dot() != 0) return 1;
	return test_retail();
}
