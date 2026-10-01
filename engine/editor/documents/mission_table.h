#pragma once

// STAGED, NOT WIRED: the mission document (the mission slice, ADR 0046's Missions row) is this table's
// live owner, its rows the records below and its parse and writer bms's; until it lands, the table's
// tests (editor_mission_table, editor_table_shape) read and edit a parsed bms::File through it, a
// handle per row into the file (mission_rows).
//
// The mission's table (ADR 0046 S13 D10): a mission's records as rows of the one table shape
// (model/table_shape.h), where bms_edit's setters by name were the only way in. The rows of the file,
// in the order the writer writes them: the mission (its header, and the tables no other row is: the
// weapon loadout, the item availability rules, the 64 groups and 32 layers, which are set and never
// added to or removed, and the bounding boxes); each entity, of its pool's kind (the four pools, so an
// Add names its pool); each of the 128 waypoint paths (fixed: set, never added or removed), holding
// its stops; each area trigger; each event; each trigger; each action. Every field is a row of the
// format's own table (formats/mission/mission_field.h: every member the writer takes from a record,
// its key, width, range and rule the format's), the editor's check before it (a whole number in its
// range, a 16.16 number in mission units in the word's range, a finite real, a text in its slot, a
// value one of the field's choices names where its choices are all the record holds).
//
// What one record names of another by its index is a Record reference (S13 D8), resolved in the file
// and renumbered by the mission document when the records it names move, never by a list edit here: a
// stop's marker (MissionMarker), an event's run of the trigger table and of the action table, by its
// first record (MissionTrigger, MissionAction, a run of none naming none) and its count. The other
// indexes a mission's records hold (an entity's group and waypoint path, an IsWithinArea trigger's
// zone, a ResetEvent action's event, which the mission's event view reads, mission_records) are the
// mission slice's to name as references. A stop put in or taken out writes the path's count as its
// slots (bms_edit's insert_waypoint_stop, D-MIS-6); nothing else here rewrites a record it does not
// set.

#include <cstddef>
#include <vector>

#include <editor/model/table_shape.h>
#include <formats/mission/bms.h>

namespace opennova::editor {

// The kinds: the mission row and the lists it holds (a weapon loadout entry, an item availability
// rule, a group, a layer, a bounding box), the four entity pools (each its own kind), a waypoint path
// and its stop, an area trigger, an event, a trigger and an action.
enum class MissionKind : NodeKind {
	Mission, Loadout, Availability, Group, Layer, BoundingBox, Item, Building, Marker, Organic, WaypointPath, Stop,
	Area, Event, Trigger, Action, kCount,
};
inline constexpr size_t kMissionKindCount = size_t(MissionKind::kCount);
constexpr NodeKind node_kind(MissionKind kind) { return static_cast<NodeKind>(kind); }

const RecordTable &mission_table();

// The rows of a parsed file, in the order the writer writes them: the mission (the file itself, whose
// header and own tables its kind reads), the entities pool by pool, the 128 waypoint paths, the area
// triggers, the events, the triggers and the actions. Each handle points into `file`.
std::vector<RecordHandle> mission_rows(bms::File &file);

} // namespace opennova::editor
