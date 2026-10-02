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
using MissionRow = MissionRecordRow<bms::File>;
using EntityRow = MissionRecordRow<bms::Entity>;
using PathRow = MissionRecordRow<MissionPath>;
using AreaRow = MissionRecordRow<bms::AreaTrigger>;
using EventRow = MissionRecordRow<mission::EventChain>;

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
	// A duplicated entity takes a fresh SSN, a duplicated area trigger a fresh zone id.
	void prepare_duplicate(Node &copy, const std::vector<std::shared_ptr<const Node>> &rows) const override;
	// A stop put into or taken out of a path whose stored count exceeds its 32 slots is refused
	// (D-MIS-6: the original editor's count for such a path is not witnessed); a chain holds 20
	// records at most (the table's lists say so).
	bool accept_list_edit(const Node &row, const ListChange &change, std::string &error) const override;
	// The mission row and the 128 paths are never added, removed or moved: a step that changes
	// them is refused.
	bool accept_step(const EditStep &step, const StagedRows &rows, std::string &error) const override;
	// The markers or the events moved: every stop's marker, every Event trigger's and ResetEvent
	// action's event renumbered (RecordShift::now); a reference to a record the edit removed refuses
	// the edit with its site.
	bool renumber_references(const StagedRows &rows, const RecordShift &shift,
	                         std::vector<Edit> &sites, std::string &error) const override;

private:
	std::vector<MissionFinding> issue_codes_;
};

bool is_mission_kind(AssetKind kind);

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
// plus .bin, the table the mission text loads [orig: TextResource_LoadMissionTextBin @0x51ed90]),
// then in medmssn.bin, the table loaded in its place when the mission has none: LOCATION%03i for
// each type-2044 marker by its one-based spawn order (section Locations [orig:
// Entity_SpawnFromBMSRecord @0x40f182..0x40f221]), STRNAME%03i for a record's nonzero name_index
// (PeopleNames [orig: @0x40ecbf..0x40ed0a]), STRWINDIRECTIVE%03i and STRWINCOND%03i for each win
// condition's id (WinConditions [orig: HUD_ShowObjectiveNotification @0x5BA2E0, HUD_DrawWinConditions
// @0x5ba940]) and STRLOSEDIRECTIVE%03i for each lose condition's (LoseConditions), an id of 0 or 255
// naming none [orig: the row walk breaks on either, HUD_DrawWinConditions @0x5ba9e0]. None is
// rewritable.
void mission_references(const Document &document, Extracted &out);

} // namespace opennova::editor
