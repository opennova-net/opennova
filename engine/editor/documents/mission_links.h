#pragma once

// What a mission's logic names in its space (ADR 0046 S24, In the views): for an event, a trigger or an
// action, the records its parameters name as the game finds them, and the distances its triggers test;
// for the whole mission, how many events name each record. The views draw them (preview/
// mission_link_shapes): a line from what an event's triggers watch to what its actions act on, the
// records rung where they stand, a distance trigger's reach about its subject, and a badge on each mark
// some event names.
//
// What a parameter names is the game's reading of it (formats/mission/mission_params.h; mission_uses.h
// reads it the same way): an Entity parameter the first record holding the SSN in the lookups' pool
// order [orig: EntityPool_FindByNetId @0x4f0a20; MissionDocument::entity_holder], a Zone the first area
// trigger of the id in file order [orig: EventTrigger_ResolveZoneTriggerRefs @0x453000, the scan
// @0x453077; MissionDocument::zone_holder], a Path the waypoint path of the number, a stop of it by its
// number (-1 the nearest: no one stop), a Group the group of the index and the entities that carry it
// (group 0 names none [docs/mission/bms-event-runtime-re.md 3a]), an Event the event of the index, a
// teleport target the first pool-3 marker of record type 6088 whose wp_number is the value [orig: the
// pool-3 scans of EventAction_TeleportEntityToSpawn @0x43dfe0..0x43e003 and Entity_TeleportTeamToSpawn
// @0x43d3b0..0x43d3cf], an effect's marker number every such marker [orig: EventAction_SpawnParticleEffect
// @0x4540e0, `result[20] == 6088`] and a teammate's the first [orig: HeliLift_SpawnPickup, the pool-3
// scan; docs/mission/bms-event-runtime-re.md 7.5 row 39]. A value nothing holds (the player's SSN 10000,
// which no record carries; a zone no area has) names no record and is said so by an empty `record`.

#include <cstddef>
#include <cstdint>
#include <vector>

#include <editor/model/value.h>
#include <formats/mission/mission_params.h>

namespace opennova::editor {

class MissionDocument;

// One parameter of a trigger or an action and what it names: the record (an entity's, an area's, a
// path's or an event's row; a group's record on the mission row; empty where nothing holds the value),
// and the rows it reaches through it: a group's members (every entity carrying the group, in the rows'
// order), a path's stops (the markers its stops name, in its order), a stop's marker, the markers a
// marker number names. A trigger's links are its subjects (what it watches), an action's its targets
// (what it acts on).
struct MissionLink {
	NodeAddress from; // the trigger or the action
	bool action = false;
	int slot = 0; // 0 for param1
	mission::ParamKind kind = mission::ParamKind::Unused;
	int64_t value = 0;
	NodeAddress record;
	std::vector<NodeId> rows;
};

// A distance a trigger tests between two entities, as the game measures it: the straight 3D distance
// (x, y and height) between the two entities' positions, whole metres [orig: Entity_CheckProximity
// @0x4f14c0, sqrt(dx^2 + dy^2 + dz^2) over the 16.16 positions @0x4f1569..0x4f159f; the trigger's
// third parameter shifted << 16 at the call @0x453879]: within it for sub-type 43 (near); within it with
// nothing between them for 44 (a line of sight) [orig: Entity_CheckLineOfSightInRange @0x4f15e0, the
// same distance @0x4f1695..0x4f16cd, then the ray]; 45 (sees) measures between the two models' bound
// centres, then the ray, then the second within 30 degrees either side of the first's heading, seen from
// above [orig: Entity_CheckLineOfSight @0x4f17c0, the centres @0x4f1876..0x4f18dd, the cone
// @0x4f195f..0x4f1989, `cmp eax, 15555540h` @0x4f1984]. The subject is the first entity (param1),
// the other the second (param2), each its holder's row (0 where none holds the SSN).
struct MissionReach {
	NodeAddress from; // the trigger
	NodeId subject = 0, other = 0;
	int64_t metres = 0;
	bool sight = false; // nothing may stand between them (44, 45)
	bool facing = false; // the other within kMissionSeesHalfAngle of the subject's heading (45)
	bool centres = false; // measured between the models' bound centres, not the positions (45)
};
// Half of what a sees trigger's subject sees across, degrees: 0x15555540 of the binary angle's 2^32 a
// turn [orig: Entity_CheckLineOfSight @0x4f17c0, the compare @0x4f1984].
inline constexpr double kMissionSeesHalfAngle = 30.0;

// What a logic record names: for an event, what each of its triggers and actions names, in their order;
// for a trigger or an action, what it names alone, `event` its event. None for a record of another
// kind.
struct MissionLinks {
	NodeAddress event;
	std::vector<MissionLink> links;
	std::vector<MissionReach> reaches;
	bool any() const { return !links.empty() || !reaches.empty(); }
};
// Whether a record is an event, a trigger or an action of a mission.
bool mission_logic_record(const NodeAddress &record);
MissionLinks mission_links(const MissionDocument &document, const NodeAddress &record);

// How many events name each record (any of their triggers' or actions' parameters naming it as above):
// entities, areas and paths by row, groups by index, events by row. A group's members are not named by
// it (the group is). Made once per state of the document (its identity, load and revision).
class MissionLinkCounts {
public:
	// The counts for `document` as it stands now (made again only when it moved).
	void follow(const MissionDocument &document);
	// How many events name the row (0 for none, or a row no event names).
	size_t events(NodeId row) const;
	// How many events name the group of this index.
	size_t group_events(int64_t group) const;
	// The rows some event names, for the views' badges.
	size_t named() const { return rows_.size(); }
	// How many times it counted (for the tests).
	size_t made() const { return made_; }

private:
	uint64_t identity_ = 0, load_ = 0, revision_ = 0;
	bool held_ = false;
	std::vector<std::pair<NodeId, size_t>> rows_; // sorted by row
	std::vector<size_t> groups_;
	size_t made_ = 0;
};

} // namespace opennova::editor
