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
// A new record of `kind` naming `item_id`, with the SSN `id`: every member zero but those the shipped
// missions' records most often hold another value for [corpus, bms_edit.cpp]; its position the origin.
bms::Entity new_entity(EntityKind kind, int item_id, int id);
// The SSN a new entity takes beside the file's: one past the largest any holds. (The original editor's
// allocator is not witnessed, D-MIS-3.)
int next_entity_ssn(const bms::File &file);
// Append new_entity for `item_id` at `transform`, its SSN the next; returns its index.
size_t add_entity(bms::File &file, EntityKind kind, int item_id, const EntityTransform &transform);
// Erase the record (later records of that kind shift down); a marker removal
// repairs every waypoint path that referenced it.
bool remove_entity(bms::File &file, EntityKind kind, size_t index, std::string &error);

// --- waypoints ------------------------------------------------------------------
std::vector<WaypointSummary> waypoint_summaries(const bms::File &file);
bool waypoint_path(const bms::File &file, size_t index, WaypointPath &out);
bool set_waypoint_path(bms::File &file, size_t index, const std::vector<int> &marker_indices,
		int flags, std::string &error);
bool clear_waypoint_path(bms::File &file, size_t index, std::string &error);
// A new marker record linked into `path_index` at `insert_index` (-1 = append);
// returns the marker's index through `out_marker_index`.
bool add_waypoint_marker(bms::File &file, size_t path_index, int marker_item_id,
		const EntityTransform &transform, int insert_index, std::string &error,
		size_t *out_marker_index = nullptr);
// A path's stops one at a time (ADR 0046 S13 D10, the editor's mission table): a stop naming marker
// `marker` put in at `index` (the end past it; the path's 128-byte slot region holds 32), or the one at
// `index` taken out. Either changes how many stops the path has, so the count the path stores is
// written as its slots, its slot bytes past them zero (D-MIS-6: the original editor's count for an
// edited path is not witnessed); which marker a stop names is the editor's to check (a Record
// reference), the runtime reading any word [orig: Pool_GetEntryUnchecked @0x441FC0].
bool insert_waypoint_stop(bms::WaypointRecord &path, size_t index, uint32_t marker, std::string &error);
bool erase_waypoint_stop(bms::WaypointRecord &path, size_t index);

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
