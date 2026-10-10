#pragma once

// The mission document's edit operations and typed views as free functions
// over the parsed bms::File (ADR 0043 slice E11: the MissionDocument facade
// died -- the file IS the document; the one stateful consumer, the
// MissionData binding, keeps its own source path, last error and
// loaded/header-only facts). Every witnessed rule the facade carried (the fixed-width slot
// copies, the clamps, the chain-index bookkeeping, the reference repairs)
// lives here unchanged; each body keeps its citation.

#include <formats/mission/bms.h>
#include <formats/mission/mission.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::mission {

// --- the document as a whole ------------------------------------------------
// A minimal, valid, empty mission: the magic + version the parser gates on,
// the fixed waypoint/group/layer tables backfilled through sync_counts, so
// bms::write produces a buffer bms::parse accepts.
void make_default(bms::File &file);
// A new mission as an author starts one: make_default's file, named, on a terrain and under an
// environment, with the header values the shipped missions hold in common [corpus, bms_edit.cpp:
// mission_corpus's retail leg holds each to the shipped missions' most common value]. It holds no
// entity, area trigger or event. False, with `error` and `file` untouched, for a name past its slot
// (the mission name's 32 bytes, the designer's 32, the terrain's and the environment's 16).
struct BlankMission {
	std::string name;
	std::string designer;
	std::string terrain;     // the .trn's base name
	std::string environment; // the .env's base name
};
bool make_blank(bms::File &file, const BlankMission &blank, std::string &error);
// Re-derive every header count and chunk length from the vectors, backfill
// the fixed tables and normalize the waypoint padding (idempotent for a
// loaded file; the from-scratch path needs it). Every mutator below runs it.
void sync_counts(bms::File &file);
// The header as the typed view (basenames for the terrain/environment refs).
MissionInfo mission_info(const bms::File &file);

// --- the header ---------------------------------------------------------------
// By the key of one of the header's field rows (formats/mission/mission_field.h),
// of the setter's type; any other key is refused by name.
bool set_header_string(bms::File &file, const std::string &field, const std::string &value, std::string &error);
bool set_header_int(bms::File &file, const std::string &field, int value, std::string &error);
void set_header_flag(bms::File &file, int bit, bool on); // one attrib_flags bit
bool set_header_float(bms::File &file, const std::string &field, float value, std::string &error);

// --- entities -----------------------------------------------------------------
std::vector<bms::Entity> *entities(bms::File &file, EntityKind kind);
const std::vector<bms::Entity> *entities(const bms::File &file, EntityKind kind);
size_t entity_count(const bms::File &file, EntityKind kind);
// The items.def id a record names (bms type_id + kItemIdOffset).
int entity_item_id(const bms::Entity &entity);
// The record's position (16.16 -> float mission units) and integer-degree eulers.
EntityTransform entity_transform(const bms::Entity &entity);
// The fixed 8-byte AI class / AI script slots as strings.
std::string entity_name1(const bms::Entity &entity);
std::string entity_name2(const bms::Entity &entity);
// The editable int properties by editor key (the entity's field rows,
// formats/mission/mission_field.cpp: group, waypoint_id, wp_number, team,
// lfp_group, ai_flags, perception, accuracy, alert_state,
// min/max_engagement_distance, max_attack_distance, spawn_count,
// max_simultaneous, no_less_than, map_symbol, and the eulers pitch, yaw and
// roll); the uint8-backed fields clamp, ai_flags rejects bits outside the
// known attribute mask.
bool set_entity_property_int(bms::File &file, EntityKind kind, size_t index,
		const std::string &name, int value, std::string &error);
// name1 / name2: the fixed 8-byte slots (copied at full width).
bool set_entity_property_string(bms::File &file, EntityKind kind, size_t index,
		const std::string &name, const std::string &value, std::string &error);
bool set_entity_transform(bms::File &file, EntityKind kind, size_t index,
		const EntityTransform &transform, std::string &error);
// A new record of `kind` naming `item_id`, with the SSN `id`: what the original editor's initializer
// gives an item it places (bms_edit.cpp, D-MIS-10); its position the origin.
bms::Entity new_entity(EntityKind kind, int item_id, int id);
// The SSN a new entity takes beside the file's: one past the largest any holds, the players' net ids
// skipped (mission_params.h ssn_after; the original editor's allocator takes one past the largest with
// no skip, D-MIS-10).
int next_entity_ssn(const bms::File &file);
// Append new_entity for `item_id` at `transform`, its SSN the next; returns its index.
size_t add_entity(bms::File &file, EntityKind kind, int item_id, const EntityTransform &transform);
// Erase the record (later records of that kind shift down); a marker removal
// repairs every waypoint path that referenced it.
bool remove_entity(bms::File &file, EntityKind kind, size_t index, std::string &error);

// --- waypoints ------------------------------------------------------------------
// A path's stops are its markers' (the original editor's model, D-MIS-6): a waypoint marker (type 6005,
// DEF_TYPE_WAYPOINT) on a path carries the path's number, its record's index, in its waypoint_id and its
// place in the path's order in its wp_number, and the original editor's writer lays out every path from
// those two fields, its count the waypoint markers that carry its number, its 32 slots the first of them in
// wp_number order, each a marker's index in the file's markers; a number of 0 (or past the records) is on
// no path, so record 0 holds none [orig: JOTACmed.exe sub_44C8E0 @ 0x44c8e0, the path number to the
// record's +0x4f @ 0x44c9f5 and the order to its +0x30 @ 0x44c9ff; sub_44CFD0 @ 0x44cfd0, the 6005 item's
// markers alone, a number outside 1..128 cleared, a bubble sort on the order @ 0x44d0e9..0x44d0f3;
// sub_44F920 @ 0x44f920, the count @ 0x4508a6 and the 32 slots @ 0x4508c1]. Every shipped path agrees with
// its markers (mission_corpus's retail leg). The edits below set the markers' two fields and lay out each
// path they touch as the original's writer does; a path of more than 32 stops keeps its count over its 32
// slots, as CP19.bms's path 6 of 39 does, the game reading the next record's words for the stops past them [orig:
// AIWaypoint_UpdateTarget @0x457476]. Which marker a stop names the runtime reads unbounded [orig:
// Pool_GetEntryUnchecked @0x441FC0].
std::vector<WaypointSummary> waypoint_summaries(const bms::File &file);
// Path `index`: its flags and its stops, its markers in its order.
bool waypoint_path(const bms::File &file, size_t index, WaypointPath &out);
// The markers on path `index` in its order: the waypoint markers whose waypoint_id is `index` (none for
// path 0), by wp_number, two of one number in their order in the file (the original's bubble sort keeps
// them so).
std::vector<int> waypoint_path_markers(const bms::File &file, size_t index);
// Path `index`'s record laid out from its markers: the count how many carry it, the slots the first 32,
// the slot bytes past them zero.
void lay_out_waypoint_path(bms::File &file, size_t index);
// Path `index`'s stops made `marker_indices`, in that order, and its flags `flags`: each marker carries the
// path at its place (one that stood on another path leaves it, that path laid out again), a marker the path
// held that the list leaves out carries none (waypoint_id 0, wp_number 0). Refused for path 0 (a marker
// carrying 0 is on no path), a marker out of range, named twice, or no waypoint marker (the original lays
// out a path from those alone).
bool set_waypoint_path(bms::File &file, size_t index, const std::vector<int> &marker_indices,
		int flags, std::string &error);
bool clear_waypoint_path(bms::File &file, size_t index, std::string &error);
// A new marker record put on `path_index` at `insert_index` (-1 = at its end); returns the marker's
// index through `out_marker_index`.
bool add_waypoint_marker(bms::File &file, size_t path_index, int marker_item_id,
		const EntityTransform &transform, int insert_index, std::string &error,
		size_t *out_marker_index = nullptr);
// A path's stops one at a time (ADR 0046 S13 D10, the editor's mission table): marker `marker` put on
// path `path_index` at `index` (its end past it), refused for a marker already one of its stops; or the
// stop at `index` taken off the path.
bool insert_waypoint_stop(bms::File &file, size_t path_index, size_t index, uint32_t marker, std::string &error);
bool erase_waypoint_stop(bms::File &file, size_t path_index, size_t index, std::string &error);

// --- area triggers ----------------------------------------------------------------
bool area_trigger(const bms::File &file, size_t index, AreaTriggerRecord &out);
std::vector<AreaTriggerRecord> area_triggers(const bms::File &file);
size_t add_area_trigger(bms::File &file, const AreaTriggerRecord &record);
bool set_area_trigger(bms::File &file, size_t index, const AreaTriggerRecord &record, std::string &error);
// The record taken out. A trigger or an action names a zone by its id, which the game remaps to an
// index at mission start [orig: EventTrigger_ResolveZoneTriggerRefs @0x453000]: no other zone's id
// moves, so nothing that names one is rewritten.
bool remove_area_trigger(bms::File &file, size_t index, std::string &error);

// --- the weapon loadout, the item availability rules, the groups -----------------
std::vector<WeaponLoadoutEntry> weapon_loadout(const bms::File &file);
bool set_weapon_loadout(bms::File &file, const std::vector<WeaponLoadoutEntry> &entries, std::string &error);
bool group(const bms::File &file, size_t index, GroupFields &out);
std::vector<GroupFields> groups(const bms::File &file);
bool set_group(bms::File &file, size_t index, int field0, int field8, int field12, std::string &error);

// --- the event logic ------------------------------------------------------------------
bool event(const bms::File &file, size_t index, MissionEventRecord &out);
std::vector<MissionEventRecord> events(const bms::File &file);
bool set_event(bms::File &file, size_t index, const MissionEventRecord &record, std::string &error);
bool trigger(const bms::File &file, size_t index, MissionTriggerRecord &out);
std::vector<MissionTriggerRecord> triggers(const bms::File &file);
bool set_trigger(bms::File &file, size_t index, const MissionTriggerRecord &record, std::string &error);
bool action(const bms::File &file, size_t index, MissionActionRecord &out);
std::vector<MissionActionRecord> actions(const bms::File &file);
bool set_action(bms::File &file, size_t index, const MissionActionRecord &record, std::string &error);
bool event_chain(const bms::File &file, size_t index, MissionEventChain &out);
MissionLogicSummary logic_summary(const bms::File &file);
// An event's triggers and actions are records the event owns (formats/mission/mission_chains.h): each
// chain edit below splits the file's three tables into the events' chains, edits one and joins them
// back, every run where its event stands, an empty run's first index the running offset [corpus: 115 of
// 115 shipped missions]. A file the chains cannot hold (a record in two runs or in none, a run past its
// table) refuses the edit and stays as it was. A chain holds 20 records at most (kMaxEventChainEntries).
bool insert_event_trigger(bms::File &file, size_t event_index, size_t local_index,
		const MissionTriggerRecord &record, std::string &error);
bool remove_event_trigger(bms::File &file, size_t event_index, size_t local_index, std::string &error);
bool move_event_trigger(bms::File &file, size_t event_index, size_t local_index, int delta, std::string &error);
bool insert_event_action(bms::File &file, size_t event_index, size_t local_index,
		const MissionActionRecord &record, std::string &error);
bool remove_event_action(bms::File &file, size_t event_index, size_t local_index, std::string &error);
bool move_event_action(bms::File &file, size_t event_index, size_t local_index, int delta, std::string &error);
// Append a fresh empty event (no triggers/actions; fill it through the inserts
// above); returns its index.
size_t add_event(bms::File &file, const MissionEventRecord &record);
// The event with its chain taken out, and what names an event by its index repaired: an Event
// trigger's and a ResetEvent action's first parameter one less past the hole, -1 on it.
bool remove_event(bms::File &file, size_t index, std::string &error);

} // namespace opennova::mission
