#include "mission_names.h"

// Display names for the BMS event-logic enums. The three sub-tables are file-local:
// each is reached only through the dispatcher above it.

#include "mission_detail.h"

#include <formats/mission/bms.h>
#include <formats/mission/mission_field.h>

namespace opennova::mission::detail {

// The main types are the rows the editor offers them by (mission_field.cpp).
std::string trigger_main_type_name(int value) {
	const MissionChoices types = trigger_main_types();
	if (const char *name = mission_choice_name(types.rows, types.count, value)) return name;
	return unknown_label("Unknown", value);
}

static std::string group_trigger_type_name(int value) {
	switch (static_cast<bms::GroupTriggerType>(value)) {
		case bms::GroupTriggerType::Null: return "Null";
		case bms::GroupTriggerType::GroupSeesGroup: return "GroupSeesGroup";
		case bms::GroupTriggerType::GroupHasTargetedGroup: return "GroupHasTargetedGroup";
		case bms::GroupTriggerType::GroupAtRedAlert: return "GroupAtRedAlert";
		case bms::GroupTriggerType::GroupDestroyed: return "GroupDestroyed";
		case bms::GroupTriggerType::GroupAlive: return "GroupAlive";
		case bms::GroupTriggerType::GroupHasLostMoreUnits: return "GroupHasLostMoreUnits";
		case bms::GroupTriggerType::GroupAtWaypoint: return "GroupAtWaypoint";
		case bms::GroupTriggerType::GroupIntact: return "GroupIntact";
		case bms::GroupTriggerType::GroupIsWithinArea: return "GroupIsWithinArea";
		case bms::GroupTriggerType::GroupHoldingGroup: return "GroupHoldingGroup";
		case bms::GroupTriggerType::GroupHasMoreUnits: return "GroupHasMoreUnits";
		case bms::GroupTriggerType::GroupHasShotGroup: return "GroupHasShotGroup";
		case bms::GroupTriggerType::GroupAtYellowAlert: return "GroupAtYellowAlert";
		case bms::GroupTriggerType::GroupHasTargetedSingle: return "GroupHasTargetedSingle";
		case bms::GroupTriggerType::GroupSeesSingle: return "GroupSeesSingle";
		case bms::GroupTriggerType::GroupHasShotSingle: return "GroupHasShotSingle";
	}
	return unknown_label("Unknown", value);
}

static std::string single_trigger_type_name(int value) {
	switch (static_cast<bms::SingleTriggerType>(value)) {
		case bms::SingleTriggerType::Null: return "Null";
		case bms::SingleTriggerType::SingleSeesGroup: return "SingleSeesGroup";
		case bms::SingleTriggerType::SingleHasTargetedGroup: return "SingleHasTargetedGroup";
		case bms::SingleTriggerType::SingleAtRedAlert: return "SingleAtRedAlert";
		case bms::SingleTriggerType::SingleDestroyed: return "SingleDestroyed";
		case bms::SingleTriggerType::SingleAlive: return "SingleAlive";
		case bms::SingleTriggerType::SingleHasLostMoreUnits: return "SingleHasLostMoreUnits";
		case bms::SingleTriggerType::SingleAtWaypoint: return "SingleAtWaypoint";
		case bms::SingleTriggerType::SingleIntact: return "SingleIntact";
		case bms::SingleTriggerType::SingleIsWithinArea: return "SingleIsWithinArea";
		case bms::SingleTriggerType::SingleHoldingGroup: return "SingleHoldingGroup";
		case bms::SingleTriggerType::SingleHasMoreUnits: return "SingleHasMoreUnits";
		case bms::SingleTriggerType::SingleHasShotGroup: return "SingleHasShotGroup";
		case bms::SingleTriggerType::SingleAtYellowAlert: return "SingleAtYellowAlert";
		case bms::SingleTriggerType::SingleHasTargetedSingle: return "SingleHasTargetedSingle";
		case bms::SingleTriggerType::SingleSeesSingle: return "SingleSeesSingle";
		case bms::SingleTriggerType::SingleHasShotSingle: return "SingleHasShotSingle";
		case bms::SingleTriggerType::SingleOnTopOf: return "SingleOnTopOf";
		case bms::SingleTriggerType::SingleFartherThan: return "SingleFartherThan";
		case bms::SingleTriggerType::SingleHasNoLOS: return "SingleHasNoLOS";
		case bms::SingleTriggerType::SingleDoesNotSeeOrFarther: return "SingleDoesNotSeeOrFarther";
	}
	return unknown_label("Unknown", value);
}

std::string trigger_sub_type_name(int main_type, int sub_type) {
	switch (static_cast<bms::TriggerMainType>(main_type)) {
		case bms::TriggerMainType::Group:
			return group_trigger_type_name(sub_type);
		case bms::TriggerMainType::Single:
			return single_trigger_type_name(sub_type);
		case bms::TriggerMainType::MissionVariable:
			switch (static_cast<bms::MissionVariableTriggerType>(sub_type)) {
				case bms::MissionVariableTriggerType::MissionVariableIsEqual: return "MissionVariableIsEqual";
				case bms::MissionVariableTriggerType::MissionVariableIsLessThan: return "MissionVariableIsLessThan";
				case bms::MissionVariableTriggerType::MissionVariableIsLessThanOrEqual: return "MissionVariableIsLessThanOrEqual";
				case bms::MissionVariableTriggerType::MissionVariableIsGreaterThan: return "MissionVariableIsGreaterThan";
				case bms::MissionVariableTriggerType::MissionVariableIsGreaterThanOrEqual: return "MissionVariableIsGreaterThanOrEqual";
			}
			break;
		case bms::TriggerMainType::Teammate:
			switch (static_cast<bms::TeammateTriggerType>(sub_type)) {
				case bms::TeammateTriggerType::TeammateIsEnabled: return "TeammateIsEnabled";
				case bms::TeammateTriggerType::TeammateMedicAssisting: return "TeammateMedicAssisting";
				case bms::TeammateTriggerType::TeammateEvacuating: return "TeammateEvacuating";
			}
			break;
		case bms::TriggerMainType::Player:
			switch (static_cast<bms::PlayerTriggerType>(sub_type)) {
				case bms::PlayerTriggerType::PlayerBerserk: return "PlayerBerserk";
				case bms::PlayerTriggerType::PlayerFirstPerson: return "PlayerFirstPerson";
				case bms::PlayerTriggerType::PlayerThirdPerson: return "PlayerThirdPerson";
				case bms::PlayerTriggerType::PlayerCockpitView: return "PlayerCockpitView";
				case bms::PlayerTriggerType::PlayerInputBit10: return "PlayerInputBit10";
				case bms::PlayerTriggerType::PlayerInputBit11: return "PlayerInputBit11";
				case bms::PlayerTriggerType::PlayerInputBit12: return "PlayerInputBit12";
				case bms::PlayerTriggerType::PlayerInputBit13: return "PlayerInputBit13";
				case bms::PlayerTriggerType::PlayerLookByteBit0Clear: return "PlayerLookByteBit0Clear";
				case bms::PlayerTriggerType::PlayerLookByteBit0Set: return "PlayerLookByteBit0Set";
				case bms::PlayerTriggerType::PlayerInputBit29: return "PlayerInputBit29";
				case bms::PlayerTriggerType::PlayerInputBit14: return "PlayerInputBit14";
				case bms::PlayerTriggerType::PlayerInputBit15: return "PlayerInputBit15";
				case bms::PlayerTriggerType::PlayerInputBitIndex: return "PlayerInputBitIndex";
				case bms::PlayerTriggerType::PlayerInputBitIndexPlus15: return "PlayerInputBitIndexPlus15";
				case bms::PlayerTriggerType::PlayerDialogDone: return "PlayerDialogDone";
				case bms::PlayerTriggerType::PlayerDialogFinished: return "PlayerDialogFinished";
				case bms::PlayerTriggerType::PlayerAwol: return "PlayerAwol";
				case bms::PlayerTriggerType::PlayerSatchel: return "PlayerSatchel";
				case bms::PlayerTriggerType::PlayerAttachedToSsn: return "PlayerAttachedToSsn";
				case bms::PlayerTriggerType::PlayerOnSsn: return "PlayerOnSsn";
				case bms::PlayerTriggerType::PlayerDrivingSsn: return "PlayerDrivingSsn";
				case bms::PlayerTriggerType::PlayerOnGun: return "PlayerOnGun";
			}
			break;
		case bms::TriggerMainType::Event:
		case bms::TriggerMainType::SecondTimeThrough:
			if (sub_type == 0) {
				return "Null";
			}
			break;
	}
	return unknown_label("Unknown", sub_type);
}

// The action types are the rows the editor offers them by (mission_field.cpp).
std::string action_type_name(int value) {
	const MissionChoices types = action_types();
	if (const char *name = mission_choice_name(types.rows, types.count, value)) return name;
	return unknown_label("Unknown", value);
}

// The AI sub-type display names, ported from the retail mission editor's table.
// [orig: dfx2med Med_ActionSubTypeName @0x445EE0] — the same witness bms.h cites on
// AIActionSubType, whose values this switch enumerates.
static std::string ai_action_sub_type_name(int value) {
	switch (static_cast<bms::AIActionSubType>(value)) {
		case bms::AIActionSubType::GuardBit: return "GuardBit";
		case bms::AIActionSubType::RedAlert: return "RedAlert";
		case bms::AIActionSubType::GreenAlert: return "GreenAlert";
		case bms::AIActionSubType::Accuracy: return "Accuracy";
		case bms::AIActionSubType::BlindBit: return "BlindBit";
		case bms::AIActionSubType::BerserkBit: return "BerserkBit";
		case bms::AIActionSubType::ClimberBit: return "ClimberBit";
		case bms::AIActionSubType::CowardBit: return "CowardBit";
		case bms::AIActionSubType::YellowAlert: return "YellowAlert";
		case bms::AIActionSubType::DriveSkill: return "DriveSkill";
		case bms::AIActionSubType::AimSkill: return "AimSkill";
		case bms::AIActionSubType::AiSetState: return "AiSetState";
		case bms::AIActionSubType::CombatSpeed: return "CombatSpeed";
		case bms::AIActionSubType::PatrolSpeed: return "PatrolSpeed";
		case bms::AIActionSubType::FindAndUse: return "FindAndUse";
		case bms::AIActionSubType::AiUseWpz: return "AiUseWpz";
		case bms::AIActionSubType::AiClearWpz: return "AiClearWpz";
		case bms::AIActionSubType::PlayPartAnim: return "PlayPartAnim";
		case bms::AIActionSubType::HudItem: return "HudItem";
		case bms::AIActionSubType::TmateStatus: return "TmateStatus";
		case bms::AIActionSubType::AiNodePathBit: return "AiNodePathBit";
		case bms::AIActionSubType::AttackDistanceValue: return "AttackDistanceValue";
		case bms::AIActionSubType::EngageDistanceMin: return "EngageDistanceMin";
		case bms::AIActionSubType::IndestructableBit: return "IndestructableBit";
		case bms::AIActionSubType::TargetSsn: return "TargetSsn";
		case bms::AIActionSubType::StartFiringBit: return "StartFiringBit";
		case bms::AIActionSubType::FiringAngle: return "FiringAngle";
	}
	return unknown_label("Unknown", value);
}

std::string action_sub_type_name(int action_type, int sub_type) {
	switch (static_cast<bms::ActionType>(action_type)) {
		case bms::ActionType::ChangeGroupAI:
		case bms::ActionType::ChangeSingleAI:
			return ai_action_sub_type_name(sub_type);
		case bms::ActionType::MisvarChange:
			switch (static_cast<bms::MissionVariableActionSubType>(sub_type)) {
				case bms::MissionVariableActionSubType::Null: return "Null";
				case bms::MissionVariableActionSubType::Set: return "Set";
				case bms::MissionVariableActionSubType::Add: return "Add";
				case bms::MissionVariableActionSubType::Subtract: return "Subtract";
				case bms::MissionVariableActionSubType::Increment: return "Increment";
				case bms::MissionVariableActionSubType::Decrement: return "Decrement";
			}
			break;
		case bms::ActionType::Teammates:
			switch (static_cast<bms::TeammateActionSubType>(sub_type)) {
				case bms::TeammateActionSubType::Null: return "Null";
				case bms::TeammateActionSubType::MedicAssist: return "MedicAssist";
				case bms::TeammateActionSubType::EvacuateTt: return "EvacuateTt";
				case bms::TeammateActionSubType::EvacuateAt: return "EvacuateAt";
			}
			break;
		default:
			if (sub_type == 0) {
				return "Null";
			}
			break;
	}
	return unknown_label("Unknown", sub_type);
}

} // namespace opennova::mission::detail
