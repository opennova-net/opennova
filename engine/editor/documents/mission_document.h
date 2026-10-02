#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <editor/assets/asset_kinds.h>
#include <editor/documents/mission_table.h>
#include <editor/documents/mission_validation.h>
#include <editor/graph/graph_edge.h>
#include <editor/model/table_document.h>
#include <formats/mission/bms.h>
#include <formats/mission/mission_chains.h>

namespace opennova::editor {

// The mission document (ADR 0046 S14): a `.bms`, the mission file the game loads [orig:
// Mission_LoadBMSFile @0x40f4e0], as rows of the mission's table (documents/mission_table.h): the
// mission row (the header and the tables no other row is), each entity of its pool's kind, the 128
// waypoint paths, each area trigger and each event with the triggers and actions it owns. Its rows
// stand in bands, in the order the writer writes their records (an Add lands in its band,
// Document::row_position), so a reload gives the order the document holds. Its parse is bms::parse,
// then the events' runs split into their chains (a file whose runs share a record, leave one unowned
// or reach past a table opens blocked: mission.invalid_input; one laid out in another order is noted,
// mission.event_order); what it would write compared with the bytes read, a difference noted by its
// first section (mission.rewrite_differs). serialize composes the file (the mission row's file, each
// band's records, the chains joined) and writes it from scratch (ADR 0003).
//
// What a record names: a file (the terrain, the environment, the tile set's atlas, an item, an AI
// profile, a weapon), a record of the file by index (a stop's marker, a parameter's event: Record
// references the document renumbers; an entity's group and waypoint path, a parameter's: fixed
// tables), or a record of the file by id (an entity's SSN, an area trigger's zone id: symbols the
// mission defines in its own scope, its file name; the game remaps a zone id to its index at mission
// start [orig: EventTrigger_ResolveZoneTriggerRefs @0x453000] and finds an entity by its SSN [orig:
// EntityPool_FindByNetId @0x4f0a20], never by an index, so no edit renumbers them). A trigger's or
// an action's parameters are what its type says they are (formats/mission/mission_params.h), per
// record (FieldUse: the reference, the label, whether the game reads it). What no field's value is
// (the text keys a record's number forms) is mission_references, the type's record_references hook.

// A row of the mission: its native record (mission_table.h: the mission row a bms::File with the
// header and its own tables, an entity a bms::Entity, a path a MissionPath, an area trigger a
// bms::AreaTrigger, an event a mission::EventChain) and the identities of what it holds.
template <class Native> struct MissionRecordRow : TableRow {
	Native native{};

	explicit MissionRecordRow(NodeKind k) { kind = k; }
	MissionRecordRow(NodeKind k, Native record) : native(std::move(record)) { kind = k; }
	std::shared_ptr<Node> clone() const override { return std::make_shared<MissionRecordRow>(*this); }
	std::string name() const override;
	RecordHandle record() const override { return {kind, const_cast<Native *>(&native)}; }
	size_t footprint() const override;
};
// Each kind's name and footprint, defined beside the document ([temp.expl.spec]: declared before
// any use).
template <> std::string MissionRecordRow<bms::File>::name() const;
template <> std::string MissionRecordRow<bms::Entity>::name() const;
template <> std::string MissionRecordRow<MissionPath>::name() const;
template <> std::string MissionRecordRow<bms::AreaTrigger>::name() const;
template <> std::string MissionRecordRow<mission::EventChain>::name() const;
template <> size_t MissionRecordRow<bms::File>::footprint() const;
template <> size_t MissionRecordRow<bms::Entity>::footprint() const;
template <> size_t MissionRecordRow<MissionPath>::footprint() const;
template <> size_t MissionRecordRow<bms::AreaTrigger>::footprint() const;
template <> size_t MissionRecordRow<mission::EventChain>::footprint() const;
using MissionRow = MissionRecordRow<bms::File>;
using EntityRow = MissionRecordRow<bms::Entity>;
using PathRow = MissionRecordRow<MissionPath>;
using AreaRow = MissionRecordRow<bms::AreaTrigger>;

// An event row's parameter that names an event the same step puts in (a pasted event naming
// another copy of its paste, a duplicated event naming itself): its list (0 the triggers, 1 the
// actions), the record's index there and the new event's place among the events the step puts in,
// in their order. The step that puts the row in reads it (renumber_references), the parameter then
// naming that event wherever the rows landed; no other step does, and a copy of the row carries none.
// Kept beside the value, never in it: any index a parameter holds is one a file can hold.
struct EventLink {
	uint8_t list = 0;
	uint32_t index = 0;
	uint32_t put = 0;
};
struct EventRow : MissionRecordRow<mission::EventChain> {
	using MissionRecordRow::MissionRecordRow;
	std::vector<EventLink> links;
	std::shared_ptr<Node> clone() const override {
		auto copy = std::make_shared<EventRow>(*this);
		copy->links.clear();
		return copy;
	}
};

class MissionDocument : public TableDocument {
public:
	const RecordTable &table() const override { return mission_table(); }
	// A kind's fields without a document (DocumentType::fields, S13 V3): the table fields()
	// answers, the type's own for the process.
	static const std::vector<FieldSchema> &schema(NodeKind kind) { return mission_table().fields(kind); }
	// A trigger's sub-types by its main type, an action's by its type, a parameter's values by its
	// kind (Team, Bool, SubGoal): the record's own choices (formats/mission/mission_params.h).
	bool record_choices(const NodeAddress &address, const FieldUse &use,
			std::vector<FieldChoice> &out) const override;
	// A second record of an SSN or a zone id is inert: the lookups find the first (an SSN in pool
	// order, organics, items, buildings, markers [orig: Entity_KillByNetId @0x43DBD0]; a zone in file
	// order [orig: EventTrigger_ResolveZoneTriggerRefs @0x453000 scans the table]).
	void refine_symbol(const NodeAddress &address, SymbolFacts &facts) const override;
	SerializeResult serialize() const override;
	std::unique_ptr<DocumentBase> snapshot() const override { return std::make_unique<MissionDocument>(*this); }
	// Removing records as the mission removes them (S14): a marker takes the stops that visit it
	// with it, each before it; an event another's trigger or action names by index goes only with
	// that record, which goes first (else refused with the site); any other record a Remove of its
	// own.
	bool removal_edits(const std::vector<NodeAddress> &records, std::vector<Edit> &out,
	                   std::string &error) const override;
	// Records copied as a mission fragment the writer writes and the parser reads back (what the
	// format cannot carry is no payload): rows together (entities of any pools, area triggers and
	// events, in the rows' order; the mission row and a path are the file's own, never copied), or
	// records of one nested kind from one owner (loadout entries, availability rules, bounding boxes,
	// a path's stops, an event's triggers or its actions), in their list's order. "" for any other
	// selection.
	std::string copy(const std::vector<NodeAddress> &records) const override;
	// Whether a payload holds rows (pasted at the top level, each into its band).
	bool pastes_rows(const std::string &payload) const override;

	// The mission row (null before a load); the rows of each kind in their band's order.
	const MissionRow *mission_row() const;
	std::vector<const Node *> rows_of(MissionKind kind) const;
	// The file as the writer takes it: the mission row's file with every band's records and the
	// chains joined, its counts synced. False before a load.
	bool compose(bms::File &out) const;
	// The code of each source finding (issues(), in their order): what the parse made of the file.
	const std::vector<MissionFinding> &issue_codes() const { return issue_codes_; }

protected:
	// What the table's labelled field decides on its record (a parameter's reference and whether the
	// game reads it, by its type), then the mission's own: the scope of what the file defines and
	// names by id (the mission's file name), a parameter's label and its own choices, a sub-type's
	// own choices, and the player's SSN, which names no record.
	void refine_field(const NodeAddress &address, FieldUse &use) const override;
	bool parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
	           std::shared_ptr<const FileState> &state, std::vector<SourceIssue> &issues,
	           Diagnostic &error) override;
	// A new row: an entity of its pool with a new record's values (bms_edit's new_entity) and the
	// next SSN over the rows; an area trigger with the lowest free zone id in 1..99 (refused with
	// none free); an empty event. The mission row and the paths are the file's own: none is added.
	std::shared_ptr<Node> make_node(NodeKind kind, NodeId id,
	                                const std::vector<std::shared_ptr<const Node>> &rows,
	                                std::string &error) override;
	// A row goes into its kind's band (mission_band), an entity, an area trigger or an event; the
	// mission row and the paths stay where the file puts them.
	size_t row_position(const Node &row, const std::vector<std::shared_ptr<const Node>> &rows,
	                    size_t position) const override;
	// A duplicated entity takes a fresh SSN (never the player's 10000), a duplicated area trigger a
	// fresh zone id (accept_step refuses the step when 1..99 hold none), a duplicated event naming
	// itself names its copy (an EventLink).
	void prepare_duplicate(Node &copy, const Node &original,
	                       const std::vector<std::shared_ptr<const Node>> &rows) const override;
	// A stop put into or taken out of a path whose stored count exceeds its 32 slots is refused
	// (D-MIS-6: the original editor's count for such a path is not witnessed); a chain holds 20
	// records at most (the table's lists say so).
	bool accept_list_edit(const Node &row, const ListChange &change, std::string &error) const override;
	// The mission row and the 128 paths are never added, removed or moved, and an area trigger the
	// step puts in never takes a zone id another holds: a step that would is refused.
	bool accept_step(const EditStep &step, const StagedRows &rows, std::string &error) const override;
	// The markers or the events moved: every stop's marker, every Event trigger's and ResetEvent
	// action's event renumbered (RecordShift::now), an event the step put in naming another it put in
	// by its EventLink; a reference to a record the edit removed refuses the edit with its site.
	bool renumber_references(const StagedRows &rows, const RecordShift &shift,
	                         std::vector<Edit> &sites, std::string &error) const override;
	// A payload of rows as rows of the file, told apart from the rows there: an SSN a row there
	// holds given the next free one (never 10000), a zone id a row holds the lowest free one in 1..99,
	// every parameter and rider of the copies that named the old value following it; a copied event's
	// index naming another copied event naming that copy (an EventLink, which renumber_references
	// reads once the rows are placed), one naming an event that was not copied naming the event of
	// that index here.
	bool paste_rows(const Edit &edit, const std::vector<std::shared_ptr<const Node>> &rows,
	                std::vector<std::shared_ptr<Node>> &out, std::string &error) override;
	// A payload of a nested kind's records into the owner edit.parent names (0 = the row), which
	// must hold that kind, at edit.position; the type's own list rule holds (accept_list_edit).
	bool paste_records(Node &row, const Edit &edit, const IdAllocator &allocate, std::vector<NodeId> &added,
	                   std::string &error) override;

private:
	std::vector<MissionFinding> issue_codes_;
};

bool is_mission_kind(AssetKind kind);

// The SSN a new entity takes over `rows` (an Add, a Duplicate, a paste's copy told apart): one past
// the largest the rows hold, never the player's 10000 [bms-event-runtime-re.md 7.3: the single-player
// lookups take 10000 for the player].
int32_t next_free_ssn(const std::vector<std::shared_ptr<const Node>> &rows);

// The file a mission's rows make, as the writer takes it (MissionDocument::compose over any rows of
// the mission's kinds: a batch's, a copied fragment's): the mission row's file with each band's
// records in the rows' order and the events' chains joined, its counts synced. False with no mission
// row among them.
bool compose_mission(const std::vector<std::shared_ptr<const Node>> &rows, bms::File &out);

// The scope the mission's symbols are defined in and looked up by: its file name, upper case
// ("ASH_I5B.BMS").
std::string mission_scope(const DocumentBase &document);

// The mission's references that no field's value is (DocumentType::record_references): the text
// keys its records' numbers form, each a TextId in the mission's own string table (its base name
// plus .bin, the table the mission text loads [orig: TextResource_LoadMissionTextBin @0x51ed90]), or
// in medmssn.bin, the table loaded in its place where the mission has none (the edge's alternate):
// LOCATION%03i for each type-2044 marker by its one-based spawn order (section Locations [orig:
// Entity_SpawnFromBMSRecord @0x40f182..0x40f221]), STRNAME%03i for a record's nonzero name_index
// (PeopleNames [orig: @0x40ecbf..0x40ed0a]), STRWINCOND%03i for each win slot until a 0 or 255 id
// (WinConditions [orig: HUD_DrawWinConditions @0x5ba940, the break @0x5ba9e0]), and what the actions
// read by a slot's id: SubGoalWon's STRWINMSG%03i, SubGoalLost's STRLOSEMSG%03i, a shown
// ShowWinSubgoal's STRWINDIRECTIVE%03i and ShowLoseSubgoal's STRLOSEDIRECTIVE%03i, OutputText's
// Triggered Text ID%03i. None is rewritable. A marker's waypoint name (WPNames STRWPNAME%03i of its
// record's +0x60) is read only for the waypoints the HUD lists, which markers those are not traced:
// no edge (NEEDS-RE). Then the files the mission's name finds and a dialog's bank.
void mission_references(const Document &document, Extracted &out);

} // namespace opennova::editor
