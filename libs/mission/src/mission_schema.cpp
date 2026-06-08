#include "mission/mission_schema.h"

#include <initializer_list>
#include <utility>

#include "mission/bms.h"

namespace opennova::mission {
namespace {

using Kind = MissionParamKind;

MissionParamEnumEntry enum_entry(int value, const char *label) {
	MissionParamEnumEntry entry;
	entry.value = value;
	entry.label = label;
	return entry;
}

const std::vector<MissionParamEnumEntry> &team_enum() {
	static const std::vector<MissionParamEnumEntry> values = {
		enum_entry(0, "Neutral (0)"),
		enum_entry(1, "Good / blue (1)"),
		enum_entry(2, "Evil / red (2)"),
	};
	return values;
}

const std::vector<MissionParamEnumEntry> &subgoal_enum() {
	static const std::vector<MissionParamEnumEntry> values = {
		enum_entry(1, "Subgoal 1"),
		enum_entry(2, "Subgoal 2"),
		enum_entry(3, "Subgoal 3"),
		enum_entry(4, "Subgoal 4"),
		enum_entry(5, "Subgoal 5"),
		enum_entry(6, "Subgoal 6"),
		enum_entry(7, "Subgoal 7"),
		enum_entry(8, "Subgoal 8"),
	};
	return values;
}

// PLAYPARTANIM play type. [orig: dfx2med Med_ParamAnimPlayType @0x449f20, "Fsm" config
// section, table @0x5e64f8 — FSMANIMPLAY=1 / FSMANIMSTOP=0 / FSMANIMREV=-1.]
const std::vector<MissionParamEnumEntry> &anim_play_type_enum() {
	static const std::vector<MissionParamEnumEntry> values = {
		enum_entry(1, "Play"),
		enum_entry(0, "Stop"),
		enum_entry(-1, "Reverse"),
	};
	return values;
}

// AISETSTATE behaviour state. [orig: dfx2med Med_ParamAiState @0x449e40, "Fsm" config
// section, table @0x5e64c8 — raw tokens kept in the label; the engine maps them to
// display text via the "Fsm" config we do not ship.]
const std::vector<MissionParamEnumEntry> &ai_state_enum() {
	static const std::vector<MissionParamEnumEntry> values = {
		enum_entry(1, "Formation (FSMFORMATION)"),
		enum_entry(2, "Return to base (FSMRTB)"),
		enum_entry(3, "Pretty (FSMPRETTY)"),
		enum_entry(4, "Land (FSMLAND)"),
		enum_entry(5, "Follow waypoint (FSMFOLLOWWP)"),
	};
	return values;
}

MissionParamSlot slot(const char *label,
                      Kind kind = Kind::Raw,
                      const char *tip = "",
                      std::vector<MissionParamEnumEntry> enum_values = {}) {
	MissionParamSlot out;
	out.label = label;
	out.kind = kind;
	out.tip = tip;
	out.enum_values = std::move(enum_values);
	return out;
}

// Per-AI-sub-type slots for param2/3/4 of the AI-change action family (CHANGE_GROUP_AI /
// AREA_AI_RED/BLUE / CHANGE_SINGLE_AI). Mirrors the original editor's Med_AiSubTypeParams
// @0x44A920 (slot index 0/1/2 -> param2/3/4); ai_change_spec prepends the target (param1).
// Widget kinds map: Med_ParamIntGeneric(0..999)/Med_ParamIntSmall(0..100) -> Raw,
// Med_ParamBitToggle{0,1} -> Bool, Med_ParamPickSingle -> Entity, Med_ParamAnimPlayType /
// Med_ParamAiState -> Enum, Med_ParamAnimTime -> FixedSeconds, ANIMNUM -> Animation.
// SKILL (Med_ParamSkill @0x449eb0) and HUDITEM (Med_ParamHudItem @0x44a160) are "Fsm"-section
// enums in the original; modelled Raw here (values round-trip exactly) and upgraded to Enum in P5.
std::vector<MissionParamSlot> ai_subtype_slots(int sub_type) {
	switch (static_cast<bms::AIActionSubType>(sub_type)) {
		case bms::AIActionSubType::GuardBit: return { slot("Guard", Kind::Bool) };
		case bms::AIActionSubType::Accuracy: return { slot("Accuracy %", Kind::Raw, "Weapon accuracy, 0-100.") };
		case bms::AIActionSubType::BlindBit: return { slot("Blind", Kind::Bool) };
		case bms::AIActionSubType::BerserkBit: return { slot("Berserk", Kind::Bool) };
		case bms::AIActionSubType::ClimberBit: return { slot("Climber", Kind::Bool) };
		case bms::AIActionSubType::CowardBit: return { slot("Coward", Kind::Bool) };
		case bms::AIActionSubType::DriveSkill: return { slot("Drive skill", Kind::Raw) };
		case bms::AIActionSubType::AimSkill: return { slot("Aim skill", Kind::Raw) };
		case bms::AIActionSubType::AiSetState: return { slot("AI state", Kind::Enum, "AI behaviour state.", ai_state_enum()) };
		case bms::AIActionSubType::CombatSpeed: return { slot("Combat speed (kph)", Kind::Raw) };
		case bms::AIActionSubType::PatrolSpeed: return { slot("Patrol speed (kph)", Kind::Raw) };
		case bms::AIActionSubType::FindAndUse: return { slot("Target unit", Kind::Entity) };
		case bms::AIActionSubType::TargetSsn: return { slot("Target unit", Kind::Entity) };
		case bms::AIActionSubType::PlayPartAnim:
			// ANIMNUM is the model's part-animation CHANNEL, not an animation name: the game
			// (Jointops Entity_ApplyCommand @0x43ab60 case 0x22) acts on channels 1/2 only, playing the
			// part per ANIMPLAYTYPE (-1/0/1) over ANIMTIME. The off_8135F0 walk/idle/weapon table is the
			// separate AI-state-driven infantry set, not this param.
			return { slot("Part #", Kind::Raw, "Part-animation channel on the model (ANIMNUM; the game uses 1 or 2)."),
			         slot("Play type", Kind::Enum, "", anim_play_type_enum()),
			         slot("Time (s)", Kind::FixedSeconds, "Play duration in seconds.") };
		case bms::AIActionSubType::HudItem: return { slot("HUD item", Kind::Raw), slot("Ticks", Kind::Raw) };
		case bms::AIActionSubType::TmateStatus: return { slot("Teammate status", Kind::Bool) };
		case bms::AIActionSubType::AiNodePathBit: return { slot("AI node path", Kind::Bool) };
		case bms::AIActionSubType::AttackDistanceValue: return { slot("Attack distance", Kind::Raw) };
		case bms::AIActionSubType::EngageDistanceMin: return { slot("Engage min", Kind::Raw), slot("Engage max", Kind::Raw) };
		case bms::AIActionSubType::IndestructableBit: return { slot("Indestructible", Kind::Bool) };
		case bms::AIActionSubType::StartFiringBit: return { slot("Start firing", Kind::Bool) };
		case bms::AIActionSubType::FiringAngle: return { slot("Firing angle", Kind::Raw, "Degrees, 0 to -359.") };
		case bms::AIActionSubType::RedAlert:
		case bms::AIActionSubType::GreenAlert:
		case bms::AIActionSubType::YellowAlert:
		case bms::AIActionSubType::AiUseWpz:
		case bms::AIActionSubType::AiClearWpz:
			return {};  // alert / wpz toggles: target only, no value slot (Med_AiSubTypeParams returns NULL)
		default: return { slot("Value") };  // unmodelled sub-types: a single raw value fallback
	}
}

// Build the spec for an AI-change action: the target (param1) followed by the selected
// sub-type's slots (param2/3/4). Mirrors spec()'s used-flag semantics for the trailing slots.
MissionParamSpec ai_change_spec(const char *description, MissionParamSlot target, int sub_type) {
	MissionParamSpec out;
	out.description = description;
	out.known = true;
	std::vector<MissionParamSlot> slots;
	slots.push_back(std::move(target));
	std::vector<MissionParamSlot> sub = ai_subtype_slots(sub_type);
	for (MissionParamSlot &s : sub) {
		slots.push_back(std::move(s));
	}
	for (size_t i = 0; i < out.params.size(); ++i) {
		if (i < slots.size()) {
			out.params[i] = slots[i];
			out.params[i].used = true;
		} else {
			out.params[i].used = false;
		}
	}
	return out;
}

MissionParamSpec spec(const char *description,
                      std::initializer_list<MissionParamSlot> slots,
                      bool variable = false) {
	MissionParamSpec out;
	out.description = description;
	out.known = true;
	out.variable = variable;
	size_t i = 0;
	for (const MissionParamSlot &s : slots) {
		if (i >= out.params.size()) {
			break;
		}
		out.params[i] = s;
		out.params[i].used = true;
		++i;
	}
	for (; i < out.params.size(); ++i) {
		out.params[i].used = variable ? true : false;
	}
	return out;
}

MissionParamSpec unknown_spec() {
	MissionParamSpec out;
	for (MissionParamSlot &param : out.params) {
		param.kind = Kind::Raw;
		param.used = true;
	}
	return out;
}

} // namespace

MissionParamSpec trigger_param_schema(int main_type, int sub_type) {
	switch (static_cast<bms::TriggerMainType>(main_type)) {
		case bms::TriggerMainType::Group:
			switch (sub_type) {
				case 1: return spec("Group {p1} can see group {p2}.", { slot("Group", Kind::Group), slot("Other group", Kind::Group) });
				case 2: return spec("Group {p1} has targeted group {p2}.", { slot("Group", Kind::Group), slot("Other group", Kind::Group) });
				case 3: return spec("Group {p1} is at red alert.", { slot("Group", Kind::Group) });
				case 4: return spec("Group {p1} is destroyed (no units left).", { slot("Group", Kind::Group) });
				case 5: return spec("Group {p1} is alive (at least one unit).", { slot("Group", Kind::Group) });
				case 6: return spec("Group {p1} has lost {p2} or more units.", { slot("Group", Kind::Group), slot("Units lost", Kind::Raw, "Trigger passes once this many of the group's units are gone.") });
				case 7: return spec("Group {p1} reaches waypoint {p3} (type {p2}).", { slot("Group", Kind::Group), slot("Waypoint type"), slot("Waypoint", Kind::Waypoint) });
				case 9: return spec("Group {p1} is intact (no losses).", { slot("Group", Kind::Group) });
				case 10: return spec("Group {p1} is inside zone {p2}.", { slot("Group", Kind::Group), slot("Zone", Kind::Zone) });
				case 11: return spec("Group {p1} is holding group {p2}.", { slot("Group", Kind::Group), slot("Item group", Kind::Group) });
				case 12: return spec("Group {p1} has {p2} or more units.", { slot("Group", Kind::Group), slot("Unit count") });
				case 13: return spec("Group {p1} has shot group {p2}.", { slot("Group", Kind::Group), slot("Other group", Kind::Group) });
				case 14: return spec("Group {p1} is at yellow alert.", { slot("Group", Kind::Group) });
				case 15: return spec("Group {p1} has targeted unit {p2}.", { slot("Group", Kind::Group), slot("Unit", Kind::Entity) });
				case 16: return spec("Group {p1} can see unit {p2}.", { slot("Group", Kind::Group), slot("Unit", Kind::Entity) });
				case 17: return spec("Group {p1} has shot unit {p2}.", { slot("Group", Kind::Group), slot("Unit", Kind::Entity) });
				default: return unknown_spec();
			}
		case bms::TriggerMainType::Single:
			switch (sub_type) {
				case 1: return spec("Unit {p1} can see group {p2}.", { slot("Unit", Kind::Entity), slot("Group", Kind::Group) });
				case 2: return spec("Unit {p1} has targeted group {p2}.", { slot("Unit", Kind::Entity), slot("Group", Kind::Group) });
				case 3: return spec("Unit {p1} is at red alert.", { slot("Unit", Kind::Entity) });
				case 4: return spec("Unit {p1} is destroyed.", { slot("Unit", Kind::Entity) });
				case 5: return spec("Unit {p1} is alive.", { slot("Unit", Kind::Entity) });
				case 6: return spec("Unit {p1} has taken {p2}+ damage.", { slot("Unit", Kind::Entity), slot("Hits / damage") });
				case 7: return spec("Unit {p1} reaches waypoint {p3} (type {p2}).", { slot("Unit", Kind::Entity), slot("Waypoint type"), slot("Waypoint", Kind::Waypoint) });
				case 9: return spec("Unit {p1} is at full health.", { slot("Unit", Kind::Entity) });
				case 10: return spec("Unit {p1} is inside zone {p2}.", { slot("Unit", Kind::Entity), slot("Zone", Kind::Zone) });
				case 11: return spec("Unit {p1} is holding group {p2}.", { slot("Unit", Kind::Entity), slot("Item group", Kind::Group) });
				case 12: return spec("Unit {p1} has {p2}+ hit points.", { slot("Unit", Kind::Entity), slot("Hit points") });
				case 13: return spec("Unit {p1} has shot group {p2}.", { slot("Unit", Kind::Entity), slot("Group", Kind::Group) });
				case 14: return spec("Unit {p1} is at yellow alert.", { slot("Unit", Kind::Entity) });
				case 15: return spec("Unit {p1} has targeted unit {p2}.", { slot("Unit", Kind::Entity), slot("Other unit", Kind::Entity) });
				case 16: return spec("Unit {p1} can see unit {p2}.", { slot("Unit", Kind::Entity), slot("Other unit", Kind::Entity) });
				case 17: return spec("Unit {p1} has shot unit {p2}.", { slot("Unit", Kind::Entity), slot("Other unit", Kind::Entity) });
				case 42: return spec("Unit {p1} is on top of unit {p2}.", { slot("Unit", Kind::Entity), slot("Other unit", Kind::Entity) });
				case 43: return spec("Unit {p1} is within {p3} m of unit {p2}.", { slot("Unit", Kind::Entity), slot("Other unit", Kind::Entity), slot("Distance (m)", Kind::Raw, "Whole metres, stored raw.") });
				case 44: return spec("Unit {p1} has no line of sight to unit {p2} within {p3} m.", { slot("Unit", Kind::Entity), slot("Other unit", Kind::Entity), slot("Distance (m)") });
				case 45: return spec("Unit {p1} does not see unit {p2}, or is farther than {p3} m.", { slot("Unit", Kind::Entity), slot("Other unit", Kind::Entity), slot("Range (m)") });
				default: return unknown_spec();
			}
		case bms::TriggerMainType::Event:
			return spec("Event {p1} has been triggered.", { slot("Event", Kind::Event) });
		case bms::TriggerMainType::MissionVariable:
			return spec("Compare mission variable #{p1} against {p2}.", { slot("Variable #", Kind::Raw, "Mission variable index."), slot("Compare value") });
		case bms::TriggerMainType::Player:
			switch (sub_type) {
				case 34: return spec("Player dialog {p1} is done.", { slot("Dialog #") });
				case 35: return spec("Player dialog {p1} finished.", { slot("Dialog #") });
				case 36: return spec("Player has been outside the mission area for {p1} seconds.", { slot("Seconds") });
				case 37: return spec("Player has placed a satchel in zone {p1}.", { slot("Zone", Kind::Zone) });
				case 38: return spec("Player is attached to vehicle {p1}.", { slot("Vehicle (SSN)", Kind::Entity) });
				case 39: return spec("Player is on vehicle {p1}.", { slot("Vehicle (SSN)", Kind::Entity) });
				case 40: return spec("Player is driving vehicle {p1}.", { slot("Vehicle (SSN)", Kind::Entity) });
				case 41: return spec("Player is on the gun of vehicle {p1}.", { slot("Vehicle (SSN)", Kind::Entity) });
				default: return unknown_spec();
			}
		case bms::TriggerMainType::SecondTimeThrough:
		case bms::TriggerMainType::Teammate:
			return unknown_spec();
	}
	return unknown_spec();
}

MissionParamSpec action_param_schema(int action_type, int action_sub_type) {
	switch (static_cast<bms::ActionType>(action_type)) {
		case bms::ActionType::RedirectGroupTo: return spec("Send group {p1} to a waypoint.", { slot("Group", Kind::Group), slot("Waypoint type"), slot("Waypoint", Kind::Waypoint, "-1 = nearest of that type.") });
		case bms::ActionType::KillGroup: return spec("Kill group {p1}.", { slot("Group", Kind::Group) });
		case bms::ActionType::ChangeGroupAI: return ai_change_spec("Change group {p1} AI (see sub-type).", slot("Group", Kind::Group), action_sub_type);
		// AREA_AI_RED/BLUE apply an AI sub-type to the team's units within a zone. [target kind = Zone: verify in P5]
		case bms::ActionType::AreaAiRed: return ai_change_spec("Change AI of red units in zone {p1} (see sub-type).", slot("Zone", Kind::Zone), action_sub_type);
		case bms::ActionType::AreaAiBlue: return ai_change_spec("Change AI of blue units in zone {p1} (see sub-type).", slot("Zone", Kind::Zone), action_sub_type);
		case bms::ActionType::VaporizeGroup: return spec("Vaporize group {p1}.", { slot("Group", Kind::Group) });
		case bms::ActionType::MisvarChange: return spec("Change mission variable #{p1} (see sub-type) by {p2}.", { slot("Variable #"), slot("Value") });
		case bms::ActionType::OutputText: return spec("Show on-screen text {p1}.", { slot("Text / string id") });
		case bms::ActionType::PlayWavList: return spec("Play dialog / wav {p1}.", { slot("Dialog / wav id"), slot("Always play", Kind::Bool) });
		case bms::ActionType::BlueWin: return spec("Blue team wins the round.", {});
		case bms::ActionType::RedWin: return spec("Red team wins the round.", {});
		case bms::ActionType::GreenWin: return spec("Green team wins the round.", {});
		case bms::ActionType::GroupVelocity: return spec("Set group {p1} move speed to {p2} kph.", { slot("Group", Kind::Group), slot("Speed (kph)") });
		case bms::ActionType::SubGoalWon: return spec("Win subgoal {p1}.", { slot("Subgoal #", Kind::Enum, "", subgoal_enum()) });
		case bms::ActionType::SubGoalLost: return spec("Lose subgoal {p1}.", { slot("Subgoal #", Kind::Enum, "", subgoal_enum()) });
		case bms::ActionType::ChangeGTeamAction: return spec("Change group {p1} team to {p2}.", { slot("Group", Kind::Group), slot("Team", Kind::Enum, "", team_enum()) });
		case bms::ActionType::ChangeGroupAction: return spec("Change group {p1}'s action to group {p2}.", { slot("Group", Kind::Group), slot("Group", Kind::Group) });
		case bms::ActionType::GroupTeleportAction: return spec("Teleport group {p1} to teleport target {p2}.", { slot("Group", Kind::Group), slot("Teleport target") });
		case bms::ActionType::RedirectSingleTo: return spec("Send unit {p1} to a waypoint.", { slot("Unit", Kind::Entity), slot("Waypoint type"), slot("Waypoint", Kind::Waypoint) });
		case bms::ActionType::KillSingle: return spec("Kill unit {p1}.", { slot("Unit", Kind::Entity) });
		case bms::ActionType::ChangeSingleAI: return ai_change_spec("Change unit {p1} AI (see sub-type).", slot("Unit", Kind::Entity), action_sub_type);
		case bms::ActionType::VaporizeSingle: return spec("Vaporize unit {p1}.", { slot("Unit", Kind::Entity) });
		case bms::ActionType::SingleVelocity: return spec("Set unit {p1} move speed to {p2} kph.", { slot("Unit", Kind::Entity), slot("Speed (kph)") });
		case bms::ActionType::ChangeSteamAction: return spec("Change unit {p1} team to {p2}.", { slot("Unit", Kind::Entity), slot("Team", Kind::Enum, "", team_enum()) });
		case bms::ActionType::SingleChangeGroup: return spec("Move unit {p1} into group {p2}.", { slot("Unit", Kind::Entity), slot("Group", Kind::Group) });
		case bms::ActionType::SingleTeleportAction: return spec("Teleport unit {p1} to teleport target {p2}.", { slot("Unit", Kind::Entity), slot("Teleport target") });
		case bms::ActionType::ParticleEffectAction: return spec("Play particle effect at target {p1}.", { slot("Target #") });
		case bms::ActionType::GroupOpenDoorAction: return spec("Open doors for group {p1}.", { slot("Group", Kind::Group) });
		case bms::ActionType::GroupCloseDoorAction: return spec("Close doors for group {p1}.", { slot("Group", Kind::Group) });
		case bms::ActionType::GroupResetHasVisited: return spec("Reset group {p1}'s has-visited flag.", { slot("Group", Kind::Group) });
		case bms::ActionType::SingleResetHasVisited: return spec("Reset unit {p1}'s has-visited flag.", { slot("Unit", Kind::Entity) });
		case bms::ActionType::ResetEvent: return spec("Re-arm event {p1} so it can fire again.", { slot("Event", Kind::Event) });
		case bms::ActionType::ShowWinSubgoal: return spec("Show/hide win subgoal {p1}.", { slot("Subgoal #", Kind::Enum, "", subgoal_enum()), slot("Show", Kind::Bool) });
		case bms::ActionType::ShowLoseSubgoal: return spec("Show/hide lose subgoal {p1}.", { slot("Subgoal #", Kind::Enum, "", subgoal_enum()), slot("Show", Kind::Bool) });
		case bms::ActionType::AttachToEmplaced: return spec("Mount unit {p1} on its emplaced weapon.", { slot("Unit", Kind::Entity) });
		case bms::ActionType::SetLightState: return spec("Set light {p1} state to {p2}.", { slot("Light #"), slot("On", Kind::Bool) });
		case bms::ActionType::ShowWaypoints: return spec("Show or hide waypoints.", { slot("Show", Kind::Bool) });
		case bms::ActionType::SsnTargetSsnPri: return spec("Make unit {p1} prioritise targeting unit {p2}.", { slot("Unit", Kind::Entity), slot("Target unit", Kind::Entity) });
		case bms::ActionType::SsnTargetSsnExc: return spec("Make unit {p1} exclude unit {p2} from targeting.", { slot("Unit", Kind::Entity), slot("Excluded unit", Kind::Entity) });
		case bms::ActionType::SsnTargetGroupPri: return spec("Make unit {p1} prioritise targeting group {p2}.", { slot("Unit", Kind::Entity), slot("Target group", Kind::Group) });
		case bms::ActionType::SsnTargetGroupExc: return spec("Make unit {p1} exclude group {p2} from targeting.", { slot("Unit", Kind::Entity), slot("Excluded group", Kind::Group) });
		case bms::ActionType::GroupTargetSsnPri: return spec("Make group {p1} prioritise targeting unit {p2}.", { slot("Group", Kind::Group), slot("Target unit", Kind::Entity) });
		case bms::ActionType::GroupTargetSsnExc: return spec("Make group {p1} exclude unit {p2} from targeting.", { slot("Group", Kind::Group), slot("Excluded unit", Kind::Entity) });
		case bms::ActionType::GroupTargetGroupPri: return spec("Make group {p1} prioritise targeting group {p2}.", { slot("Group", Kind::Group), slot("Target group", Kind::Group) });
		case bms::ActionType::GroupTargetGroupExc: return spec("Make group {p1} exclude group {p2} from targeting.", { slot("Group", Kind::Group), slot("Excluded group", Kind::Group) });
		default: return unknown_spec();
	}
}

bool mission_param_kind_is_picker(MissionParamKind kind) {
	return kind == Kind::Group || kind == Kind::Entity || kind == Kind::Zone ||
	       kind == Kind::Event || kind == Kind::Waypoint || kind == Kind::Bool ||
	       kind == Kind::Enum;
}

} // namespace opennova::mission
