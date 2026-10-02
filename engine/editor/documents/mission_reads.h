#pragma once

// STAGED, NOT WIRED: the mission's picture (the view lane's preview/mission_scene, ViewportKind::
// Mission) is this header's live owner, reading each entity, area and path through it and writing a
// drag back through the field ids below; until it lands, editor_mission_document reads it.

#include <cstdint>
#include <string>
#include <vector>

#include <editor/documents/mission_document.h>
#include <editor/documents/mission_table.h>

namespace opennova::editor {

// What the mission's picture reads of a mission document (ADR 0046 S14), typed and read only: each
// entity with its address, pool, item, SSN, position and eulers; each area trigger with its box; each
// used waypoint path with its stops as the addresses of the markers they name. A gizmo writes back
// through the field ids below (Sets under one gesture); nothing else is asked of the document.

// The field ids a gizmo writes: a position in mission units (16.16, z vertical, absolute [orig: the
// spawn copies the record's words, Entity_SpawnFromBMSRecord @0x40e9f0]), the eulers in whole degrees
// (engine heading is 90 minus the yaw [orig: docs/mission/bms-event-runtime-re.md 6.6]), and an area
// trigger's bounds.
namespace mission_field_ids {
inline constexpr const char *kX = "x", *kY = "y", *kZ = "z";
inline constexpr const char *kPitch = "pitch", *kYaw = "yaw", *kRoll = "roll";
inline constexpr const char *kXMin = "x_min", *kXMax = "x_max", *kYMin = "y_min", *kYMax = "y_max";
inline constexpr const char *kZMin = "z_min", *kZMax = "z_max";
} // namespace mission_field_ids

struct MissionEntityRead {
	NodeAddress address;
	MissionKind pool = MissionKind::Item; // Item, Building, Marker or Organic
	int item_id = 0;                      // the items.def id
	int ssn = 0;
	double x = 0, y = 0, z = 0;           // mission units
	int pitch = 0, yaw = 0, roll = 0;     // whole degrees
	int group = 0;                        // the group's index (0 none)
	int waypoint_id = 0;                  // the path's number (0 none, 123..127 a command)
	int wp_number = 0;
	int team = 0;
};

struct MissionAreaRead {
	NodeAddress address;
	int id = 0; // the zone id
	double x_min = 0, x_max = 0, y_min = 0, y_max = 0, z_min = 0, z_max = 0;
	bool constrains_z = false;
	bool mission_area = false;
};

struct MissionPathRead {
	NodeAddress address;
	int number = 0;
	uint32_t flags = 0;
	// Each stop's record, and the marker row it names (an empty address past the markers).
	std::vector<NodeAddress> stops;
	std::vector<NodeAddress> markers;
};

// Every entity, in the rows' order (the pools in the writer's order).
std::vector<MissionEntityRead> mission_entities(const MissionDocument &document);
// One entity row; false for a row of another kind.
bool mission_entity(const MissionDocument &document, const NodeAddress &row, MissionEntityRead &out);
std::vector<MissionAreaRead> mission_areas(const MissionDocument &document);
bool mission_area(const MissionDocument &document, const NodeAddress &row, MissionAreaRead &out);
// The paths holding a stop, in their numbers' order.
std::vector<MissionPathRead> mission_paths(const MissionDocument &document);
bool mission_path(const MissionDocument &document, const NodeAddress &row, MissionPathRead &out);

} // namespace opennova::editor
