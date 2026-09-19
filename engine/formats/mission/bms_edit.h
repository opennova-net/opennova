#pragma once

// The mission document's edit operations and typed views as free functions
// over the parsed bms::File (ADR 0043 slice E11: the MissionDocument facade
// died -- the file IS the document; the one stateful consumer, the ONED
// binding, keeps its own source path, last error and loaded/header-only
// facts). Every witnessed rule the facade carried (the fixed-width slot
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
// Re-derive every header count and chunk length from the vectors, backfill
// the fixed tables and normalize the waypoint padding (idempotent for a
// loaded file; the from-scratch path needs it). Every mutator below runs it.
void sync_counts(bms::File &file);
// The header as the typed view (basenames for the terrain/environment refs).
MissionInfo mission_info(const bms::File &file);

// --- the header ---------------------------------------------------------------
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
// The editable int properties by editor key (the kEntityIntFields table in
// bms_edit.cpp: group, waypoint_id, wp_number, team, ai_flags, perception,
// accuracy, alert_state, min/max_engagement_distance, max_attack_distance,
// spawn_count, max_simultaneous, no_less_than, map_symbol); the uint8-backed
// fields clamp, ai_flags rejects bits outside the known attribute mask.
bool set_entity_property_int(bms::File &file, EntityKind kind, size_t index,
		const std::string &name, int value, std::string &error);
// name1 / name2: the fixed 8-byte slots (copied at full width).
bool set_entity_property_string(bms::File &file, EntityKind kind, size_t index,
		const std::string &name, const std::string &value, std::string &error);
bool set_entity_transform(bms::File &file, EntityKind kind, size_t index,
		const EntityTransform &transform, std::string &error);
// Append a default-seeded record for `item_id`; returns its index.
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

// --- area triggers ----------------------------------------------------------------
bool area_trigger(const bms::File &file, size_t index, AreaTriggerRecord &out);
std::vector<AreaTriggerRecord> area_triggers(const bms::File &file);
size_t add_area_trigger(bms::File &file, const AreaTriggerRecord &record);
bool set_area_trigger(bms::File &file, size_t index, const AreaTriggerRecord &record, std::string &error);
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
// Drain the event's chains through the single-element removers, repair the
// ResetEvent references, erase the event.
bool remove_event(bms::File &file, size_t index, std::string &error);

} // namespace opennova::mission
