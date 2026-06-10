// Editor parameter-schema tests (mission_schema.h): the trigger/action -> widget metadata the
// ONED inspector renders. Split out of the old mission_runtime_test when the duplicate MissionRuntime
// evaluator was retired in favour of the shared-world BmsEventSystem (see event_runtime_test).
#include <string>

#include "common/test_expect.h"
#include "mission/bms.h"
#include "mission/mission.h"
#include "mission/mission_schema.h"

namespace {

opennova::mission::MissionEventRecord make_event(int flags = 0) {
	opennova::mission::MissionEventRecord event;
	event.flags = flags;
	return event;
}

} // namespace

int main() {
	using namespace opennova::mission;

	const MissionParamSpec reset_schema = action_param_schema(static_cast<int>(opennova::bms::ActionType::ResetEvent));
	TEST_EXPECT(reset_schema.known);
	TEST_EXPECT(reset_schema.params[0].kind == MissionParamKind::Event);
	TEST_EXPECT(reset_schema.params[0].used);
	TEST_EXPECT(!reset_schema.params[1].used);
	TEST_EXPECT(reset_schema.description.find("event") != std::string::npos);

	const MissionParamSpec unknown_action = action_param_schema(9999);
	TEST_EXPECT(!unknown_action.known);
	for (const MissionParamSlot &slot : unknown_action.params) {
		TEST_EXPECT(slot.used);
		TEST_EXPECT(slot.kind == MissionParamKind::Raw);
	}

	// AI-change actions are sub-type-aware: PLAYPARTANIM exposes target / ANIMNUM / ANIMPLAYTYPE / ANIMTIME.
	const MissionParamSpec play_anim = action_param_schema(
		static_cast<int>(opennova::bms::ActionType::ChangeSingleAI),
		static_cast<int>(opennova::bms::AIActionSubType::PlayPartAnim));
	TEST_EXPECT(play_anim.known);
	TEST_EXPECT(play_anim.params[0].kind == MissionParamKind::Entity);       // target unit (param1)
	TEST_EXPECT(play_anim.params[1].kind == MissionParamKind::Raw);          // ANIMNUM = part channel (1/2)
	TEST_EXPECT(play_anim.params[2].kind == MissionParamKind::Enum);         // ANIMPLAYTYPE
	TEST_EXPECT(play_anim.params[2].enum_values.size() == 3);                // Play / Stop / Reverse
	TEST_EXPECT(play_anim.params[3].kind == MissionParamKind::FixedSeconds); // ANIMTIME
	for (const MissionParamSlot &slot : play_anim.params) {
		TEST_EXPECT(slot.used);
	}

	// A single-slot sub-type (ACCURACY) leaves param3/4 unused; the target stays a Group.
	const MissionParamSpec group_accuracy = action_param_schema(
		static_cast<int>(opennova::bms::ActionType::ChangeGroupAI),
		static_cast<int>(opennova::bms::AIActionSubType::Accuracy));
	TEST_EXPECT(group_accuracy.known);
	TEST_EXPECT(group_accuracy.params[0].kind == MissionParamKind::Group);
	TEST_EXPECT(group_accuracy.params[1].used);
	TEST_EXPECT(!group_accuracy.params[2].used);

	// AISETSTATE is an enum of the five FSM states.
	const MissionParamSpec set_state = action_param_schema(
		static_cast<int>(opennova::bms::ActionType::ChangeSingleAI),
		static_cast<int>(opennova::bms::AIActionSubType::AiSetState));
	TEST_EXPECT(set_state.params[1].kind == MissionParamKind::Enum);
	TEST_EXPECT(set_state.params[1].enum_values.size() == 5);

	// AREA_AI_RED resolves to a Zone target + the sub-type's slots.
	const MissionParamSpec area_ai = action_param_schema(
		static_cast<int>(opennova::bms::ActionType::AreaAiRed),
		static_cast<int>(opennova::bms::AIActionSubType::BlindBit));
	TEST_EXPECT(area_ai.known);
	TEST_EXPECT(area_ai.params[0].kind == MissionParamKind::Zone);
	TEST_EXPECT(area_ai.params[1].kind == MissionParamKind::Bool);

	const MissionParamSpec event_trigger = trigger_param_schema(static_cast<int>(opennova::bms::TriggerMainType::Event), 123);
	TEST_EXPECT(event_trigger.known);
	TEST_EXPECT(event_trigger.params[0].kind == MissionParamKind::Event);

	// MissionDocument integrity: a malformed trigger_index must block remove_event without corrupting counts.
	MissionDocument malformed;
	malformed.create_default();
	TEST_EXPECT(malformed.add_event(make_event()));
	malformed.bms_file().events[0].trigger_index = 999;
	malformed.bms_file().events[0].trigger_count = 1;
	TEST_EXPECT(!malformed.remove_event(0));
	TEST_EXPECT(malformed.event_count() == 1);
	TEST_EXPECT(malformed.trigger_count() == 0);

	return 0;
}
