#pragma once

// What each of a trigger's and an action's four parameters is, as rows: one row per trigger sub-type
// and per action type (and, where an action's sub-type decides its parameters, per sub-type), saying
// for each of param1..param4 what the game reads it as and what the editor calls it. The rows are the
// witness record's (docs/mission/bms-event-runtime-re.md sections 7.4 and 7.5) [orig:
// EventTrigger_EvaluateCondition @0x453620 reads a trigger's params, EventAction_Dispatch @0x4542e0 an
// action's, Entity_ApplyCommand @0x43ab60 an AI sub-type's]; the slot names where the original editor
// names a slot are section 8.2's [orig editor: dfx2med Med_TriggerConditionParams @0x44a390,
// Med_AiSubTypeParams @0x44a920, Med_ActionParams @0x44ac30]; the value domains section 8.4's. A
// parameter no row names is Raw: a number the record holds, shown and set, named nothing.
//
// The sub-types' names live here too, one copy each: the typed views' name functions
// (mission_names.h) and the editor's choices read them.

#include <cstddef>
#include <cstdint>

#include <formats/mission/bms.h>
#include <formats/mission/mission_field.h>

namespace opennova::mission {

// What a parameter is. A reference to a record of the mission:
//   Group        a group by its index 0..63, 0 naming none (GROUP_REF);
//   Entity       an entity by its SSN, the record's id, never an index (ENTITY_REF), 0 naming none;
//   Zone         an area trigger by its id in the file, remapped to its index at mission start
//                [orig: EventTrigger_ResolveZoneTriggerRefs @0x453000, ...ZoneActionRefs @0x453100];
//   Event        an event by its index in the event table (EVENT_REF);
//   Path         a waypoint path by its number: 0 none, 1..122 a path, 123..127 a command
//                (docs/world/world-wac-ai-re.md section 11; dfx2med Med_ParamWaypointList @0x449c60);
//   PathNode     a stop of that path by its number, -1 the nearest.
// A number with a domain: MissionVar (an index into the mission variables), Dialog (a dialog of the
// mission's bank), Count (units), Hp (hit points), DistanceM (whole metres: the game evaluates
// param << 16), Seconds, SpeedKph, Bool, Team (0 neutral, 1 good, 2 evil), SubGoal (1..8), Bit (an
// input bit's index), HudTimer (a HUD timer 0..15), LightChannel (0..3), TeleportTarget (the 1..99
// number of a placed type-6088 marker), WpNumber (the wp_number of the type-6088 markers an effect
// plays at). Unused: the game does not read the parameter for this type. Raw: read, named nothing.
enum class ParamKind : uint8_t {
	Unused, Group, Entity, Zone, Event, Path, PathNode, MissionVar, Dialog, Count, Hp, DistanceM, Seconds,
	SpeedKph, Bool, Team, SubGoal, Bit, HudTimer, LightChannel, TeleportTarget, WpNumber, Raw,
};

// A row's sub-type where the row is every sub-type's of its type that has no row of its own.
inline constexpr int32_t kAnySubType = -1;

// What a trigger's or an action's parameter holds where its type does not read it: every shipped
// record holds -1 there (19,596 of the install's 19,596 such slots over its 115 missions, the
// mission_logic ctest's retail leg; the original editor's initializer is unread), so a new record is
// born with it in each of its four, and a retype writes it into a slot the new type does not read.
// (A sub-type its type does not read holds 0: 501 of 501.)
inline constexpr int32_t kUnreadParam = -1;

// One row: a trigger's main type (an action's type) and sub-type, and its four parameters: what each
// is and what the editor calls it (null: the kind's own words, param_kind_label).
struct ParamRow {
	int32_t main;
	int32_t sub;
	ParamKind p[4];
	const char *label[4];
};

// The row of a trigger of this main type and sub-type, and of an action of this type and sub-type:
// the row of that sub-type, else the type's row for any; null for a type no row names (a trigger main
// type outside 1..7, an action type the dispatcher has no case for), whose parameters are Raw.
const ParamRow *trigger_params(int32_t main_type, int32_t sub_type);
const ParamRow *action_params(int32_t action_type, int32_t sub_type);

// What a record's parameter `slot` (0 for param1 .. 3 for param4) is, the record's other words read
// where they decide it: a RedirectGroupTo's or RedirectSingleTo's third parameter is the entity to go
// to where its waypoint list is a command 123..125 (section 8.2), a stop's number otherwise. Raw for
// a record no row names.
ParamKind trigger_param_kind(const bms::Trigger &trigger, int slot);
ParamKind action_param_kind(const bms::Action &action, int slot);
// What the editor calls it: the row's label, else its kind's.
const char *trigger_param_label(const bms::Trigger &trigger, int slot);
const char *action_param_label(const bms::Action &action, int slot);
// A kind's own words ("Group", "Zone", "Waypoint list"; "" for Unused).
const char *param_kind_label(ParamKind kind);
// The values a parameter of the kind takes by name (Team, Bool, SubGoal); none for any other.
MissionChoices param_choices(ParamKind kind);

// A trigger's sub-types by its main type, and an action's by its type, by name: the values the
// evaluator and the dispatcher have a case for [orig: the jump tables of
// EventTrigger_EvaluateCondition @0x453620 and of Entity_ApplyCommand @0x43ab60; the names dfx2med's
// Med_TriggerConditionName @0x446330 and Med_ActionSubTypeName @0x445ee0], none for a type whose
// sub-type selects nothing (its sub-type is 0, "Null").
MissionChoices trigger_sub_types(int32_t main_type);
MissionChoices action_sub_types(int32_t action_type);
// Whether the evaluator reads a trigger's sub-type under its main type: every case but Event's, which
// tests the event's latch whatever the sub-type, and SecondTimeThrough's, which returns the load
// parity; a main type with no case reads nothing [orig: EventTrigger_EvaluateCondition @0x453620;
// docs/mission/bms-event-runtime-re.md 1.4, 7.4].
bool trigger_reads_sub_type(int32_t main_type);

// The waypoint path numbers that name no path: 0 none, and the commands 123..127.
inline constexpr int32_t kFirstPathCommand = 123, kLastPathCommand = 127;
// The commands whose rider names an entity by its SSN (the wp_number beside an entity's waypoint_id,
// a Redirect action's third parameter): 123..125 [orig: Entity_UpdateInfantryAI @0x4ba9ad boards the
// SSN; dfx2med section 8.2].
inline constexpr int32_t kLastEntityPathCommand = 125;
constexpr bool path_command_names_entity(int64_t path) {
	return path >= kFirstPathCommand && path <= kLastEntityPathCommand;
}
// Whether a waypoint list's number names no path: a path is 1 to 122; 0 is none and 123 to 127 are
// commands (go to an SSN, a group, the player) [orig editor: dfx2med Med_ParamWaypointList @0x449c60;
// docs/world/world-wac-ai-re.md section 11].
constexpr bool path_names_none(int64_t path) {
	return path == 0 || (path >= kFirstPathCommand && path <= kLastPathCommand);
}
// The original editor's name of a waypoint list's command 123..127 ("Goto SSN (not driver)"); null
// for any other number [orig editor: dfx2med Med_ParamWaypointList @0x449c60 names them;
// docs/world/world-wac-ai-re.md section 11].
const char *path_command_editor_name(int64_t number);

// Whether a group parameter names no group: group 0, which every witnessed consumer exits on or reads
// false for [orig: Entity_KillAllByNetId @0x43C8F2, Entity_IsTeamInTriggerBounds @0x43c730,
// Entity_HandleAlertCommand @0x43CF10, Entity_TeleportAllByNetId @0x43D5D0].
constexpr bool group_names_none(int64_t group) { return group == 0; }

// The SSN a mission's parameter names the player by: no record of a mission carries it, and the game
// stamps it on the player as its net id [orig: PlayerClass_InitEntity @0x4b1149..0x4b1173, 10000 plus
// the slot byte, the key EntityPool_FindByNetId @0x4f0a20 matches; 04TR's watchdog
// SingleIsWithinArea(10000, zone 6), docs/mission/bms-event-runtime-re.md 7.3].
inline constexpr int32_t kPlayerSsn = 10000;

// The SSNs the sees, targeted and shot relation records and the visited words keyed by an SSN hold:
// rows 0 to 127, the setters skipping any other [orig: the row bound-checks @0x452b60, @0x452bf0;
// docs/mission/bms-event-runtime-re.md 3a].
inline constexpr int32_t kRelationSsnRows = 128;
// Whether a trigger's SSN parameter at `slot` (0 for param1 .. 3 for param4) keys a sees, targeted,
// shot or visited record the game never writes for it: one past the rows those hold (the player's
// 10000 among those never kept). The keyed slots: a Single's subs 1, 2, 13 (its param1) and 7 (the
// visited word, its param1), 15 to 17 (both); a Group's 15 to 17 (the entity, param2).
bool trigger_ssn_unrecorded(const bms::Trigger &trigger, int slot);

// The pools a lookup of an SSN scans, one bit a pool (1 << pool): 0 the organics, 1 the items, 2 the
// buildings, 3 the markers. The lookups by SSN scan all four [orig: EntityPool_FindByNetId @0x4f0a20,
// Entity_KillByNetId @0x43DBD0: pools 0, 1, 2, 3]; some tests and actions fewer.
inline constexpr uint8_t kOrganicPool = 1u << 0, kItemPool = 1u << 1, kBuildingPool = 1u << 2,
                         kMarkerPool = 1u << 3;
inline constexpr uint8_t kAllPools = kOrganicPool | kItemPool | kBuildingPool | kMarkerPool;
// The pools the lookup of a trigger's param1 SSN scans where it scans fewer than the four
// (docs/mission/bms-event-runtime-re.md 3b, 7.2a, 7.4), each false for an SSN it finds no row of:
// the Single alive test (SingleDestroyed, SingleAlive) the organics, items and buildings, a marker's
// SSN reading not alive [orig: cat 2 subs 4, 5 @0x453985 / @0x45399D -> Entity_IsAliveByBmsRef
// @0x43e640, pools 0/1/2]; the alert, health and area tests the organics and items [orig:
// Entity_IsSsnAtAlertLevel @0x43e780, Entity_HasDamageCapacity @0x43e3d0, Entity_HasFullHealth
// @0x43e470, Entity_HasHealthAboveThreshold @0x43e350, Entity_IsBmsRefInTriggerBounds @0x43e510:
// pools 0-1]; the holding test the organics [orig: Entity_IsSsnHoldingItemGroup @0x43e2f0, pool 0].
// 0 for any other trigger.
uint8_t trigger_ssn_pools(const bms::Trigger &trigger);
// The same of an action's param1 (docs/mission/bms-event-runtime-re.md 7.5, 10): ChangeSingleAI,
// ChangeSteam, SingleChangeGroup and SingleTeleport the organics, items and buildings [orig:
// Entity_HandleAlertStateEvent @0x43DEE0, pools 0, 1, 2 @0x43DF20 / @0x43DF41 / @0x43DF69;
// Entity_FindByDCBAndSetFlag @0x43DB30; Entity_SetNetIdByParentRef @0x43D6C0;
// EventAction_TeleportEntityToSpawn @0x43DFC0, @0x43E02D / @0x43E0DD / @0x43E180]; a medevac's or a
// flyover's patient the organics [orig: HeliLift_SpawnPickup @0x4525E0; docs/world/world-wac-ai-re.md
// 33.32]. 0 for any other action.
uint8_t action_ssn_pools(const bms::Action &action);

} // namespace opennova::mission
