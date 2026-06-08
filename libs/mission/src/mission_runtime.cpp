#include "mission/mission_runtime.h"

#include <algorithm>
#include <utility>

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

MissionRuntimeCommand command_from_action(const MissionActionRecord &action, size_t event_index) {
	MissionRuntimeCommand command;
	command.event_index = event_index;
	command.action_index = action.index;
	command.action_type = action.action_type;
	command.action_sub_type = action.action_sub_type;
	command.param1 = action.param1;
	command.param2 = action.param2;
	command.param3 = action.param3;
	command.param4 = action.param4;
	command.requires_host = true;
	return command;
}

bool has_reset_after_flag(int flags) {
	return (flags & static_cast<int>(bms::EventFlags::ResetAfter)) != 0;
}

int compare_mission_variable(int left, int op, int right) {
	switch (static_cast<bms::MissionVariableTriggerType>(op)) {
		case bms::MissionVariableTriggerType::MissionVariableIsEqual:
			return left == right;
		case bms::MissionVariableTriggerType::MissionVariableIsLessThan:
			return left < right;
		case bms::MissionVariableTriggerType::MissionVariableIsGreaterThan:
			return left > right;
		case bms::MissionVariableTriggerType::MissionVariableIsLessThanOrEqual:
			return left <= right;
		case bms::MissionVariableTriggerType::MissionVariableIsGreaterThanOrEqual:
			return left >= right;
	}
	return false;
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

bool MissionRuntimeTriggerKey::operator<(const MissionRuntimeTriggerKey &other) const {
	if (main_type != other.main_type) return main_type < other.main_type;
	if (sub_type != other.sub_type) return sub_type < other.sub_type;
	if (param1 != other.param1) return param1 < other.param1;
	if (param2 != other.param2) return param2 < other.param2;
	if (param3 != other.param3) return param3 < other.param3;
	return param4 < other.param4;
}

bool MissionRuntime::load(const MissionDocument &document) {
	chains_.clear();
	states_.clear();
	load_diagnostics_.clear();
	mission_variables_.assign(256, 0);
	host_triggers_.clear();
	if (!document.is_loaded()) {
		return false;
	}
	chains_.reserve(document.event_count());
	states_.resize(document.event_count());
	for (size_t i = 0; i < document.event_count(); ++i) {
		MissionEventChain chain;
		if (!document.get_event_chain(i, chain)) {
			MissionRuntimeDiagnostic diagnostic;
			diagnostic.severity = "error";
			diagnostic.code = "runtime.event_unreadable";
			diagnostic.message = "Mission event could not be read.";
			diagnostic.event_index = i;
			load_diagnostics_.push_back(diagnostic);
			continue;
		}
		for (const MissionLogicDiagnostic &source : chain.diagnostics) {
			MissionRuntimeDiagnostic diagnostic;
			diagnostic.severity = source.severity;
			diagnostic.code = source.code;
			diagnostic.message = source.message;
			diagnostic.event_index = i;
			diagnostic.subject_index = source.subject_index;
			load_diagnostics_.push_back(diagnostic);
		}
		chains_.push_back(std::move(chain));
	}
	return true;
}

void MissionRuntime::reset() {
	for (EventState &state : states_) {
		state = {};
	}
	std::fill(mission_variables_.begin(), mission_variables_.end(), 0);
	// Mirror load(): drop stale host-trigger state so a restart does not re-fire an event off a
	// trigger the host set during the previous run (before the host re-pushes fresh state).
	host_triggers_.clear();
}

MissionRuntimeTickResult MissionRuntime::tick() {
	MissionRuntimeTickResult result;
	for (size_t i = 0; i < chains_.size(); ++i) {
		EventState &state = states_[i];
		const MissionEventRecord &event = chains_[i].event;
		if (state.fired) {
			if (!has_reset_after_flag(event.flags)) {
				continue;
			}
			if (state.reset_remaining > 0) {
				--state.reset_remaining;
				continue;
			}
			state.fired = false;
		}
		if (state.delay_remaining > 0) {
			--state.delay_remaining;
			if (state.delay_remaining > 0) {
				continue;
			}
		} else if (!evaluate_chain(chains_[i], result)) {
			continue;
		} else if (event.delay > 0) {
			state.delay_remaining = event.delay;
			continue;
		}
		for (const MissionActionRecord &action : chains_[i].actions) {
			dispatch_action(action, i, result);
		}
		state.fired = true;
		state.reset_remaining = std::max(0, event.reset_after);
	}
	return result;
}

void MissionRuntime::set_mission_variable(int index, int value) {
	if (index < 0) {
		return;
	}
	const size_t i = static_cast<size_t>(index);
	if (i >= mission_variables_.size()) {
		mission_variables_.resize(i + 1, 0);
	}
	mission_variables_[i] = value;
}

int MissionRuntime::get_mission_variable(int index) const {
	if (index < 0 || static_cast<size_t>(index) >= mission_variables_.size()) {
		return 0;
	}
	return mission_variables_[static_cast<size_t>(index)];
}

void MissionRuntime::set_host_trigger(const MissionRuntimeTriggerKey &key, bool value) {
	host_triggers_[key] = value;
}

void MissionRuntime::clear_host_triggers() {
	host_triggers_.clear();
}

bool MissionRuntime::has_event_fired(size_t index) const {
	return index < states_.size() && states_[index].fired;
}

const std::vector<MissionRuntimeDiagnostic> &MissionRuntime::load_diagnostics() const {
	return load_diagnostics_;
}

size_t MissionRuntime::event_count() const {
	return chains_.size();
}

bool MissionRuntime::evaluate_chain(const MissionEventChain &chain, MissionRuntimeTickResult &result) const {
	if (chain.triggers.empty()) {
		return true;
	}
	bool combined = evaluate_trigger(chain.triggers[0], chain.event.index, result);
	for (size_t i = 1; i < chain.triggers.size(); ++i) {
		const bool current = evaluate_trigger(chain.triggers[i], chain.event.index, result);
		const MissionTriggerRecord &previous = chain.triggers[i - 1];
		if (previous.logic_or) {
			combined = combined || current;
		} else if (previous.logic_xor) {
			combined = combined != current;
		} else {
			combined = combined && current;
		}
	}
	return combined;
}

bool MissionRuntime::evaluate_trigger(const MissionTriggerRecord &trigger, size_t event_index, MissionRuntimeTickResult &result) const {
	bool value = false;
	bool understood = true;
	if (trigger.main_type == static_cast<int>(bms::TriggerMainType::MissionVariable)) {
		value = compare_mission_variable(get_mission_variable(trigger.param1), trigger.sub_type, trigger.param2);
	} else if (trigger.main_type == static_cast<int>(bms::TriggerMainType::Event)) {
		value = trigger.param1 >= 0 && has_event_fired(static_cast<size_t>(trigger.param1));
	} else {
		MissionRuntimeTriggerKey key;
		key.main_type = trigger.main_type;
		key.sub_type = trigger.sub_type;
		key.param1 = trigger.param1;
		key.param2 = trigger.param2;
		key.param3 = trigger.param3;
		key.param4 = trigger.param4;
		const auto it = host_triggers_.find(key);
		if (it != host_triggers_.end()) {
			value = it->second;
		} else {
			understood = false;
		}
	}
	if (!understood) {
		add_runtime_diagnostic(result, "runtime.trigger_requires_host", "Trigger requires host gameplay state.", event_index, static_cast<int>(trigger.index));
	}
	return trigger.negated ? !value : value;
}

void MissionRuntime::dispatch_action(const MissionActionRecord &action, size_t event_index, MissionRuntimeTickResult &result) {
	MissionRuntimeCommand command = command_from_action(action, event_index);
	switch (static_cast<bms::ActionType>(action.action_type)) {
		case bms::ActionType::MisvarChange: {
			const int current = get_mission_variable(action.param1);
			int next = current;
			switch (static_cast<bms::MissionVariableActionSubType>(action.action_sub_type)) {
				case bms::MissionVariableActionSubType::Set: next = action.param2; break;
				case bms::MissionVariableActionSubType::Add: next = current + action.param2; break;
				case bms::MissionVariableActionSubType::Subtract: next = current - action.param2; break;
				case bms::MissionVariableActionSubType::Increment: next = current + 1; break;
				case bms::MissionVariableActionSubType::Decrement: next = current - 1; break;
				default:
					command.kind = MissionRuntimeCommandKind::UnsupportedAction;
					command.supported = false;
					result.commands.push_back(command);
					add_runtime_diagnostic(result, "runtime.unsupported_mission_variable_action", "Mission variable action subtype is not supported.", event_index, static_cast<int>(action.index));
					return;
			}
			set_mission_variable(action.param1, next);
			command.kind = MissionRuntimeCommandKind::MissionVariableChanged;
			command.param2 = next;
			command.requires_host = false;
			result.commands.push_back(command);
			return;
		}
		case bms::ActionType::ResetEvent:
			command.kind = MissionRuntimeCommandKind::ResetEvent;
			command.requires_host = false;
			if (action.param1 >= 0 && static_cast<size_t>(action.param1) < states_.size()) {
				states_[static_cast<size_t>(action.param1)] = {};
			} else {
				command.supported = false;
				add_runtime_diagnostic(result, "runtime.reset_event_out_of_range", "ResetEvent references an event outside the mission table.", event_index, static_cast<int>(action.index));
			}
			result.commands.push_back(command);
			return;
		case bms::ActionType::OutputText:
			command.kind = MissionRuntimeCommandKind::OutputText;
			command.label = "output_text";
			result.commands.push_back(command);
			return;
		case bms::ActionType::PlayWavList:
			command.kind = MissionRuntimeCommandKind::PlayDialog;
			command.label = "play_dialog";
			result.commands.push_back(command);
			return;
		case bms::ActionType::BlueWin:
		case bms::ActionType::RedWin:
		case bms::ActionType::GreenWin:
			command.kind = MissionRuntimeCommandKind::TeamWin;
			command.label = "team_win";
			result.commands.push_back(command);
			return;
		case bms::ActionType::SubGoalWon:
		case bms::ActionType::SubGoalLost:
		case bms::ActionType::ShowWinSubgoal:
		case bms::ActionType::ShowLoseSubgoal:
			command.kind = MissionRuntimeCommandKind::Subgoal;
			command.label = "subgoal";
			result.commands.push_back(command);
			return;
		case bms::ActionType::ShowWaypoints:
			command.kind = MissionRuntimeCommandKind::ShowWaypoints;
			command.label = "show_waypoints";
			result.commands.push_back(command);
			return;
		case bms::ActionType::SetLightState:
			command.kind = MissionRuntimeCommandKind::SetLightState;
			command.label = "set_light_state";
			result.commands.push_back(command);
			return;
		default: {
			const bool known = action_param_schema(action.action_type, action.action_sub_type).known;
			command.kind = known ? MissionRuntimeCommandKind::HostAction : MissionRuntimeCommandKind::UnsupportedAction;
			command.supported = known;
			if (!command.supported) {
				add_runtime_diagnostic(result, "runtime.unsupported_action", "Action type is not supported by the mission runtime.", event_index, static_cast<int>(action.index));
			}
			result.commands.push_back(command);
			return;
		}
	}
}

void MissionRuntime::add_runtime_diagnostic(MissionRuntimeTickResult &result,
                                            const std::string &code,
                                            const std::string &message,
                                            size_t event_index,
                                            int subject_index) const {
	MissionRuntimeDiagnostic diagnostic;
	diagnostic.severity = "warning";
	diagnostic.code = code;
	diagnostic.message = message;
	diagnostic.event_index = event_index;
	diagnostic.subject_index = subject_index;
	result.diagnostics.push_back(diagnostic);
}

} // namespace opennova::mission
