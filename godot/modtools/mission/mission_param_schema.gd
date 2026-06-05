class_name MissionParamSchema
extends RefCounted

## Designer-facing meaning of each trigger/action param1..4, reverse-engineered from the Jointops.exe
## runtime (EventTrigger_EvaluateCondition @0x453620, EventAction_Dispatch @0x4542e0) and cross-checked
## against the C# format reference. Full table + IDA evidence: notes/mission/param-semantics.md.
##
## This is human-authored RE truth (the engine carries params raw), so it lives here as a code table — NOT
## engine-probed like the type/sub-type enum reflectors. It degrades gracefully: any trigger/action type or
## param slot not described below renders as today's raw int spinbox and round-trips the value untouched.

# Widget kind for one param slot. Picker kinds (GROUP..ENUM) render a dropdown populated from a mission
# collection; everything else is a labelled raw int spinbox (RAW covers counts, distances, ids, etc.).
enum Kind { RAW, GROUP, ENTITY, ZONE, EVENT, WAYPOINT, BOOL, ENUM }

const _DEFAULT_SLOT := { "label": "", "kind": Kind.RAW, "tip": "", "enum": [] }

# Fixed team table (dfx2med Med_ParamTeam @0x449A90: the {id,name-key} array at unk_5E63B0, NULL-terminated;
# names from config section "Teams"). Only three teams exist; ChangeGroupTeam/ChangeSingleTeam param2 picks one.
const TEAM_ENUM := [
	{ "value": 0, "label": "Neutral (0)" },
	{ "value": 1, "label": "Good / blue (1)" },
	{ "value": 2, "label": "Evil / red (2)" },
]

# Win/lose subgoals are a fixed 8-slot table (dfx2med Med_ParamSubGoalWon/Lost @0x449710/@0x449830 loop i=1..8;
# each slot maps to a STRWINCOND%.3d / STRLOSECOND%.3d condition string in the mission config). The .bms carries
# only the slot index 1..8; the display text lives in the companion mission config, which our model does not load.
const SUBGOAL_ENUM := [
	{ "value": 1, "label": "Subgoal 1" }, { "value": 2, "label": "Subgoal 2" },
	{ "value": 3, "label": "Subgoal 3" }, { "value": 4, "label": "Subgoal 4" },
	{ "value": 5, "label": "Subgoal 5" }, { "value": 6, "label": "Subgoal 6" },
	{ "value": 7, "label": "Subgoal 7" }, { "value": 8, "label": "Subgoal 8" },
]

# Trigger main types (TriggerMainType): 1=Group 2=Single 3=Event 4=MissionVariable 5=SecondTimeThrough
# 6=Teammate 7=Player. Keyed [main_type][sub_type] -> { desc, p:[slot,...] }. A "_default" entry under a
# main type applies to any sub-type without its own row (used where every sub shares the same param shape).
const TRIGGERS := {
	1: {  # Group — param1 is always a group id (0..63)
		1:  { "desc": "Group {p1} can see group {p2}.", "p": [ {"label":"Group","kind":Kind.GROUP}, {"label":"Other group","kind":Kind.GROUP} ] },
		2:  { "desc": "Group {p1} has targeted group {p2}.", "p": [ {"label":"Group","kind":Kind.GROUP}, {"label":"Other group","kind":Kind.GROUP} ] },
		3:  { "desc": "Group {p1} is at red alert.", "p": [ {"label":"Group","kind":Kind.GROUP} ] },
		4:  { "desc": "Group {p1} is destroyed (no units left).", "p": [ {"label":"Group","kind":Kind.GROUP} ] },
		5:  { "desc": "Group {p1} is alive (at least one unit).", "p": [ {"label":"Group","kind":Kind.GROUP} ] },
		6:  { "desc": "Group {p1} has lost {p2} or more units.", "p": [ {"label":"Group","kind":Kind.GROUP}, {"label":"Units lost","tip":"Trigger passes once this many of the group's units are gone."} ] },
		7:  { "desc": "Group {p1} reaches waypoint {p3} (type {p2}).", "p": [ {"label":"Group","kind":Kind.GROUP}, {"label":"Waypoint type"}, {"label":"Waypoint","kind":Kind.WAYPOINT} ] },
		9:  { "desc": "Group {p1} is intact (no losses).", "p": [ {"label":"Group","kind":Kind.GROUP} ] },
		10: { "desc": "Group {p1} is inside zone {p2}.", "p": [ {"label":"Group","kind":Kind.GROUP}, {"label":"Zone","kind":Kind.ZONE} ] },
		11: { "desc": "Group {p1} is holding group {p2}.", "p": [ {"label":"Group","kind":Kind.GROUP}, {"label":"Item group","kind":Kind.GROUP} ] },
		12: { "desc": "Group {p1} has {p2} or more units.", "p": [ {"label":"Group","kind":Kind.GROUP}, {"label":"Unit count"} ] },
		13: { "desc": "Group {p1} has shot group {p2}.", "p": [ {"label":"Group","kind":Kind.GROUP}, {"label":"Other group","kind":Kind.GROUP} ] },
		14: { "desc": "Group {p1} is at yellow alert.", "p": [ {"label":"Group","kind":Kind.GROUP} ] },
		15: { "desc": "Group {p1} has targeted unit {p2}.", "p": [ {"label":"Group","kind":Kind.GROUP}, {"label":"Unit","kind":Kind.ENTITY} ] },
		16: { "desc": "Group {p1} can see unit {p2}.", "p": [ {"label":"Group","kind":Kind.GROUP}, {"label":"Unit","kind":Kind.ENTITY} ] },
		17: { "desc": "Group {p1} has shot unit {p2}.", "p": [ {"label":"Group","kind":Kind.GROUP}, {"label":"Unit","kind":Kind.ENTITY} ] },
	},
	2: {  # Single — param1 is an entity (SSN / BMS id)
		1:  { "desc": "Unit {p1} can see group {p2}.", "p": [ {"label":"Unit","kind":Kind.ENTITY}, {"label":"Group","kind":Kind.GROUP} ] },
		2:  { "desc": "Unit {p1} has targeted group {p2}.", "p": [ {"label":"Unit","kind":Kind.ENTITY}, {"label":"Group","kind":Kind.GROUP} ] },
		3:  { "desc": "Unit {p1} is at red alert.", "p": [ {"label":"Unit","kind":Kind.ENTITY} ] },
		4:  { "desc": "Unit {p1} is destroyed.", "p": [ {"label":"Unit","kind":Kind.ENTITY} ] },
		5:  { "desc": "Unit {p1} is alive.", "p": [ {"label":"Unit","kind":Kind.ENTITY} ] },
		6:  { "desc": "Unit {p1} has taken {p2}+ damage.", "p": [ {"label":"Unit","kind":Kind.ENTITY}, {"label":"Hits / damage"} ] },
		7:  { "desc": "Unit {p1} reaches waypoint {p3} (type {p2}).", "p": [ {"label":"Unit","kind":Kind.ENTITY}, {"label":"Waypoint type"}, {"label":"Waypoint","kind":Kind.WAYPOINT} ] },
		9:  { "desc": "Unit {p1} is at full health.", "p": [ {"label":"Unit","kind":Kind.ENTITY} ] },
		10: { "desc": "Unit {p1} is inside zone {p2}.", "p": [ {"label":"Unit","kind":Kind.ENTITY}, {"label":"Zone","kind":Kind.ZONE} ] },
		11: { "desc": "Unit {p1} is holding group {p2}.", "p": [ {"label":"Unit","kind":Kind.ENTITY}, {"label":"Item group","kind":Kind.GROUP} ] },
		12: { "desc": "Unit {p1} has {p2}+ hit points.", "p": [ {"label":"Unit","kind":Kind.ENTITY}, {"label":"Hit points"} ] },
		13: { "desc": "Unit {p1} has shot group {p2}.", "p": [ {"label":"Unit","kind":Kind.ENTITY}, {"label":"Group","kind":Kind.GROUP} ] },
		14: { "desc": "Unit {p1} is at yellow alert.", "p": [ {"label":"Unit","kind":Kind.ENTITY} ] },
		15: { "desc": "Unit {p1} has targeted unit {p2}.", "p": [ {"label":"Unit","kind":Kind.ENTITY}, {"label":"Other unit","kind":Kind.ENTITY} ] },
		16: { "desc": "Unit {p1} can see unit {p2}.", "p": [ {"label":"Unit","kind":Kind.ENTITY}, {"label":"Other unit","kind":Kind.ENTITY} ] },
		17: { "desc": "Unit {p1} has shot unit {p2}.", "p": [ {"label":"Unit","kind":Kind.ENTITY}, {"label":"Other unit","kind":Kind.ENTITY} ] },
		42: { "desc": "Unit {p1} is on top of unit {p2}.", "p": [ {"label":"Unit","kind":Kind.ENTITY}, {"label":"Other unit","kind":Kind.ENTITY} ] },
		43: { "desc": "Unit {p1} is within {p3} m of unit {p2}.", "p": [ {"label":"Unit","kind":Kind.ENTITY}, {"label":"Other unit","kind":Kind.ENTITY}, {"label":"Distance (m)","tip":"Whole metres, stored raw (the engine shifts <<16 when evaluating)."} ] },
		44: { "desc": "Unit {p1} has no line of sight to unit {p2} within {p3} m.", "p": [ {"label":"Unit","kind":Kind.ENTITY}, {"label":"Other unit","kind":Kind.ENTITY}, {"label":"Distance (m)"} ] },
		45: { "desc": "Unit {p1} does not see unit {p2}, or is farther than {p3} m.", "p": [ {"label":"Unit","kind":Kind.ENTITY}, {"label":"Other unit","kind":Kind.ENTITY}, {"label":"Range (m)"} ] },
	},
	3: {  # Event reference
		"_default": { "desc": "Event {p1} has been triggered.", "p": [ {"label":"Event","kind":Kind.EVENT} ] },
	},
	4: {  # Mission variable comparison (sub-type name carries the operator: ==, <, >, <=, >=)
		"_default": { "desc": "Compare mission variable #{p1} against {p2}.", "p": [ {"label":"Variable #","tip":"Mission variable index (V0..)."}, {"label":"Compare value"} ] },
	},
	7: {  # Player
		34: { "desc": "Player dialog {p1} is done.", "p": [ {"label":"Dialog #"} ] },
		35: { "desc": "Player dialog {p1} finished.", "p": [ {"label":"Dialog #"} ] },
		36: { "desc": "Player has been outside the mission area for {p1} seconds.", "p": [ {"label":"Seconds"} ] },
		37: { "desc": "Player has placed a satchel in zone {p1}.", "p": [ {"label":"Zone","kind":Kind.ZONE} ] },
		38: { "desc": "Player is attached to vehicle {p1}.", "p": [ {"label":"Vehicle (SSN)","kind":Kind.ENTITY} ] },
		39: { "desc": "Player is on vehicle {p1}.", "p": [ {"label":"Vehicle (SSN)","kind":Kind.ENTITY} ] },
		40: { "desc": "Player is driving vehicle {p1}.", "p": [ {"label":"Vehicle (SSN)","kind":Kind.ENTITY} ] },
		41: { "desc": "Player is on the gun of vehicle {p1}.", "p": [ {"label":"Vehicle (SSN)","kind":Kind.ENTITY} ] },
	},
}

# Action types (ActionType). Keyed [action_type] -> { desc, p:[slot,...] }.
const ACTIONS := {
	1:  { "desc": "Send group {p1} to a waypoint.", "p": [ {"label":"Group","kind":Kind.GROUP}, {"label":"Waypoint type"}, {"label":"Waypoint","kind":Kind.WAYPOINT,"tip":"-1 = nearest of that type."} ] },
	2:  { "desc": "Kill group {p1}.", "p": [ {"label":"Group","kind":Kind.GROUP} ] },
	3:  { "desc": "Change group {p1} AI (see sub-type).", "variable": true, "p": [ {"label":"Group","kind":Kind.GROUP}, {"label":"Value"} ] },
	4:  { "desc": "Vaporize group {p1}.", "p": [ {"label":"Group","kind":Kind.GROUP} ] },
	5:  { "desc": "Change mission variable #{p1} (see sub-type) by {p2}.", "p": [ {"label":"Variable #"}, {"label":"Value"} ] },
	6:  { "desc": "Show on-screen text {p1}.", "p": [ {"label":"Text / string id"} ] },
	7:  { "desc": "Play dialog / wav {p1}.", "p": [ {"label":"Dialog / wav id"}, {"label":"Always play","kind":Kind.BOOL} ] },
	8:  { "desc": "Blue team wins the round.", "p": [] },
	9:  { "desc": "Red team wins the round.", "p": [] },
	10: { "desc": "Green team wins the round.", "p": [] },
	11: { "desc": "Set group {p1} move speed to {p2} kph.", "p": [ {"label":"Group","kind":Kind.GROUP}, {"label":"Speed (kph)"} ] },
	14: { "desc": "Win subgoal {p1}.", "p": [ {"label":"Subgoal #","kind":Kind.ENUM,"enum":SUBGOAL_ENUM} ] },
	15: { "desc": "Lose subgoal {p1}.", "p": [ {"label":"Subgoal #","kind":Kind.ENUM,"enum":SUBGOAL_ENUM} ] },
	16: { "desc": "Change group {p1} team to {p2}.", "p": [ {"label":"Group","kind":Kind.GROUP}, {"label":"Team","kind":Kind.ENUM,"enum":TEAM_ENUM} ] },
	17: { "desc": "Change group {p1}'s action to group {p2}.", "p": [ {"label":"Group","kind":Kind.GROUP}, {"label":"Group","kind":Kind.GROUP} ] },
	18: { "desc": "Teleport group {p1} to teleport target {p2}.", "p": [ {"label":"Group","kind":Kind.GROUP}, {"label":"Teleport target"} ] },
	19: { "desc": "Send unit {p1} to a waypoint.", "p": [ {"label":"Unit","kind":Kind.ENTITY}, {"label":"Waypoint type"}, {"label":"Waypoint","kind":Kind.WAYPOINT} ] },
	20: { "desc": "Kill unit {p1}.", "p": [ {"label":"Unit","kind":Kind.ENTITY} ] },
	21: { "desc": "Change unit {p1} AI (see sub-type).", "variable": true, "p": [ {"label":"Unit","kind":Kind.ENTITY}, {"label":"Value"} ] },
	22: { "desc": "Vaporize unit {p1}.", "p": [ {"label":"Unit","kind":Kind.ENTITY} ] },
	23: { "desc": "Set unit {p1} move speed to {p2} kph.", "p": [ {"label":"Unit","kind":Kind.ENTITY}, {"label":"Speed (kph)"} ] },
	24: { "desc": "Change unit {p1} team to {p2}.", "p": [ {"label":"Unit","kind":Kind.ENTITY}, {"label":"Team","kind":Kind.ENUM,"enum":TEAM_ENUM} ] },
	25: { "desc": "Move unit {p1} into group {p2}.", "p": [ {"label":"Unit","kind":Kind.ENTITY}, {"label":"Group","kind":Kind.GROUP} ] },
	26: { "desc": "Teleport unit {p1} to teleport target {p2}.", "p": [ {"label":"Unit","kind":Kind.ENTITY}, {"label":"Teleport target"} ] },
	27: { "desc": "Play particle effect at target {p1}.", "p": [ {"label":"Target #"} ] },
	30: { "desc": "Open doors for group {p1}.", "p": [ {"label":"Group","kind":Kind.GROUP} ] },
	31: { "desc": "Close doors for group {p1}.", "p": [ {"label":"Group","kind":Kind.GROUP} ] },
	32: { "desc": "Reset group {p1}'s has-visited flag.", "p": [ {"label":"Group","kind":Kind.GROUP} ] },
	33: { "desc": "Reset unit {p1}'s has-visited flag.", "p": [ {"label":"Unit","kind":Kind.ENTITY} ] },
	34: { "desc": "Re-arm event {p1} so it can fire again.", "p": [ {"label":"Event","kind":Kind.EVENT} ] },
	35: { "desc": "Show/hide win subgoal {p1}.", "p": [ {"label":"Subgoal #","kind":Kind.ENUM,"enum":SUBGOAL_ENUM}, {"label":"Show","kind":Kind.BOOL} ] },
	36: { "desc": "Show/hide lose subgoal {p1}.", "p": [ {"label":"Subgoal #","kind":Kind.ENUM,"enum":SUBGOAL_ENUM}, {"label":"Show","kind":Kind.BOOL} ] },
	37: { "desc": "Mount unit {p1} on its emplaced weapon.", "p": [ {"label":"Unit","kind":Kind.ENTITY} ] },
	38: { "desc": "Set light {p1} state to {p2}.", "p": [ {"label":"Light #"}, {"label":"On","kind":Kind.BOOL} ] },
	# 39 (Teammates) param shape depends on the teammate sub-type (medic/evac targets); left raw.
	40: { "desc": "Show or hide waypoints.", "p": [ {"label":"Show","kind":Kind.BOOL} ] },
	42: { "desc": "Make unit {p1} prioritise targeting unit {p2}.", "p": [ {"label":"Unit","kind":Kind.ENTITY}, {"label":"Target unit","kind":Kind.ENTITY} ] },
	43: { "desc": "Make unit {p1} exclude unit {p2} from targeting.", "p": [ {"label":"Unit","kind":Kind.ENTITY}, {"label":"Excluded unit","kind":Kind.ENTITY} ] },
	44: { "desc": "Make unit {p1} prioritise targeting group {p2}.", "p": [ {"label":"Unit","kind":Kind.ENTITY}, {"label":"Target group","kind":Kind.GROUP} ] },
	45: { "desc": "Make unit {p1} exclude group {p2} from targeting.", "p": [ {"label":"Unit","kind":Kind.ENTITY}, {"label":"Excluded group","kind":Kind.GROUP} ] },
	46: { "desc": "Make group {p1} prioritise targeting unit {p2}.", "p": [ {"label":"Group","kind":Kind.GROUP}, {"label":"Target unit","kind":Kind.ENTITY} ] },
	47: { "desc": "Make group {p1} exclude unit {p2} from targeting.", "p": [ {"label":"Group","kind":Kind.GROUP}, {"label":"Excluded unit","kind":Kind.ENTITY} ] },
	48: { "desc": "Make group {p1} prioritise targeting group {p2}.", "p": [ {"label":"Group","kind":Kind.GROUP}, {"label":"Target group","kind":Kind.GROUP} ] },
	49: { "desc": "Make group {p1} exclude group {p2} from targeting.", "p": [ {"label":"Group","kind":Kind.GROUP}, {"label":"Excluded group","kind":Kind.GROUP} ] },
}


static func _normalize(entry: Dictionary) -> Dictionary:
	var raw: Array = entry.get("p", [])
	# A described type carries a row (desc/p); an undescribed type arrives as the {} fallback from
	# .get(type, {}). For described types, a slot beyond `p` is unused -> disabled by the inspector.
	# Undescribed types (count unknown) and `variable` AI types (count depends on sub-type) keep all 4
	# slots usable so a real param is never blocked.
	var known := not entry.is_empty()
	var variable := bool(entry.get("variable", false))
	var slots: Array = []
	for i in 4:
		var slot: Dictionary = _DEFAULT_SLOT.duplicate(true)
		if i < raw.size():
			var src := raw[i] as Dictionary
			for k in src:
				slot[k] = src[k]
		slot["used"] = (not known) or variable or (i < raw.size())
		slots.append(slot)
	return { "desc": String(entry.get("desc", "")), "params": slots }


# Returns { desc:String, params:[4 fully-defaulted slot dicts] } for a trigger. Unknown type => all RAW.
static func trigger_slots(main_type: int, sub_type: int) -> Dictionary:
	var by_main: Dictionary = TRIGGERS.get(main_type, {})
	var entry: Dictionary = by_main.get(sub_type, by_main.get("_default", {}))
	return _normalize(entry)


# Returns { desc:String, params:[4 fully-defaulted slot dicts] } for an action. Unknown type => all RAW.
static func action_slots(action_type: int) -> Dictionary:
	return _normalize(ACTIONS.get(action_type, {}))


static func is_picker(kind: int) -> bool:
	return kind == Kind.GROUP or kind == Kind.ENTITY or kind == Kind.ZONE \
		or kind == Kind.EVENT or kind == Kind.WAYPOINT or kind == Kind.BOOL or kind == Kind.ENUM
