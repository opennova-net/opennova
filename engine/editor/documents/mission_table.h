#pragma once

// STAGED, NOT WIRED: the mission document (the mission slice, ADR 0046's Missions row) is this table's
// live owner; until it lands, the table's tests (editor_mission_table, editor_table_shape) read and
// edit missions through it.
//
// The mission's table (ADR 0046 S13 D10): a mission's records as rows of the one table shape
// (model/table_shape.h), where bms_edit's setters by name were the only way in. A mission file is one
// record, the header, holding its lists in the order the file writes them: the weapon loadout, the
// four entity pools, the 128 waypoint paths (each with its stops, the markers it visits), the 64
// groups and 32 layers (fixed tables: set, never added to or removed), the area triggers, and the
// events, each holding its triggers and its actions. Every field is a row of the format's own table
// (formats/mission/mission_field.h), its key, width, range and rule the format's; a path's stops are
// the file's (bms_edit's waypoint stops), since a stop names a marker of the file.
//
// A list edit changes its own list and the file's counts (sync_counts), and an event's chain moves in
// the file's trigger and action tables with it; what other records name by an index (a path's stops
// their markers, an IsWithinArea trigger its zone, a ResetEvent action its event) is a Record
// reference the mission document renumbers (S13 D8's renumber_references, the repairs bms_edit's
// removers make), never the list edit's.

#include <cstddef>

#include <editor/model/table_shape.h>
#include <formats/mission/bms.h>

namespace opennova::editor {

// The kinds, in the order a mission's lists hold them: the mission (the header), a weapon loadout
// entry, the four entity pools (each its own kind, so an Add names its pool), a waypoint path and its
// stop, a group, a layer, an area trigger, an event, a trigger and an action.
enum class MissionKind : NodeKind {
	Mission, Loadout, Item, Building, Marker, Organic, WaypointPath, Stop, Group, Layer, Area, Event, Trigger,
	Action, kCount,
};
inline constexpr size_t kMissionKindCount = size_t(MissionKind::kCount);
constexpr NodeKind node_kind(MissionKind kind) { return static_cast<NodeKind>(kind); }

const RecordTable &mission_table();

// The mission's own record, the top of every record's trail: what the lists of an event and of a path
// read the file's tables through (RecordHandle::top).
RecordHandle mission_record(bms::File &file);

} // namespace opennova::editor
