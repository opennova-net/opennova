#pragma once

// The mission's table (ADR 0046 S13 D10, S14): a mission's records as rows of the one table shape
// (model/table_shape.h), which the mission document (documents/mission_document.h) is the record
// document over. The rows of the file, in the order the writer writes them, each a band of its kind:
// the mission (its header, and the tables no other row is: the weapon loadout, the item availability
// rules, the 64 groups and 32 layers, which are set and never added to or removed, and the bounding
// boxes); each entity, of its pool's kind (the four pools, so an Add names its pool); each of the 128
// waypoint paths (fixed: set, never added or removed), holding its stops; each area trigger; each
// event, holding its triggers and its actions (formats/mission/mission_chains.h: an event owns the
// records of its runs, and the writer derives each run's first index and count). Every field is a row
// of the format's own table (formats/mission/mission_field.h: every member the writer takes from a
// record, its key, width, range and rule the format's), the editor's check before it (a whole number
// in its range, a 16.16 number in mission units in the word's range, a finite real, a text in its
// slot, a value one of the field's choices names where its choices are all the record holds).
//
// What one record names of another: by its index, a Record reference (S13 D8), resolved in the file
// and renumbered by the mission document when the records it names move (a stop's marker, an Event
// trigger's and a ResetEvent action's event), or naming a record of a fixed table (an entity's and a
// parameter's group, an entity's and a parameter's waypoint path); by its id, a symbol the record
// defines in its file (an entity's SSN, an area trigger's zone id), which no edit of the rows
// renumbers. What a trigger's or an action's parameter is, its record's type decides
// (formats/mission/mission_params.h): the table's labelled fields answer it per record.

#include <cstddef>
#include <vector>

#include <editor/model/table_shape.h>
#include <formats/mission/bms.h>
#include <formats/mission/mission_chains.h>

namespace opennova::editor {

// The kinds: the mission row and the lists it holds (a weapon loadout entry, an item availability
// rule, a group, a layer, a bounding box), the four entity pools (each its own kind), a waypoint path
// and its stop, an area trigger, an event and the trigger and the action it holds. The rows' kinds
// stand in the order the writer writes their records (mission_band).
enum class MissionKind : NodeKind {
	Mission, Loadout, Availability, Group, Layer, BoundingBox, Item, Building, Marker, Organic, WaypointPath, Stop,
	Area, Event, Trigger, Action, kCount,
};
inline constexpr size_t kMissionKindCount = size_t(MissionKind::kCount);
constexpr NodeKind node_kind(MissionKind kind) { return static_cast<NodeKind>(kind); }
constexpr bool is_entity_kind(NodeKind kind) {
	return kind >= node_kind(MissionKind::Item) && kind <= node_kind(MissionKind::Organic);
}

// A waypoint path as a row: the file's record and its number, which is its place among the file's 128
// (an entity's waypoint_id and a parameter name it by; the file writes no number).
struct MissionPath {
	bms::WaypointRecord record;
	int number = 0;
};

// The native record behind each kind: the mission row a bms::File holding its header and its own
// tables alone (its pools, paths, area triggers and events are rows of their own); an entity a
// bms::Entity; a path a MissionPath; an area trigger a bms::AreaTrigger; an event a
// mission::EventChain; a loadout entry a bms::WeaponLoadoutRecord, an availability rule a
// bms::ItemAvailabilityEntry, a group a bms::GroupRecord, a layer a bms::LayerRecord, a bounding box a
// bms::BoundingBox, a stop its marker's index (uint32_t), a trigger a bms::Trigger, an action a
// bms::Action.
const RecordTable &mission_table();

// The band a row's kind stands in among the file's rows, in the writer's order (0 the mission, then
// the items, buildings, markers, organics, paths, area triggers and events); -1 for a nested kind.
int mission_band(NodeKind kind);

// What a bounding box's value is to the game by its type (bms::BoundingBoxType): "Health per tick",
// "Mana per tick", "Reverb preset", "Location", "Music variable 4"; null where its value word is read as
// no number (a Mission box's words are its mission's name, any other type's are read by nothing).
const char *box_value_label(int32_t type);

// The most records an event holds of each of its two lists, a path of its stops.
inline constexpr size_t kMaxEventRecords = mission::kMaxEventChainEntries;

} // namespace opennova::editor
