// A mission's names (ADR 0046 S24, Names and Live). S0: a mission variable's base words are the script's name for
// it ("V12"), a trigger and an action reading a variable word it through MissionNames::variable (a name the
// mission gives it overriding them); and the live logic the session holds while Play runs is empty until a game
// reports (documents/mission_live.h), an event found by its index.
#include <cstdio>
#include <string>

#include <editor/documents/mission_live.h>
#include <editor/documents/mission_sentence.h>
#include <editor/session/view/activity_view.h>
#include <formats/mission/bms.h>

#include "common/test_expect.h"

using namespace opennova::editor;
namespace bms = opennova::bms;

namespace {

// A mission's names with V3 named.
class NamedVariables final : public MissionNames {
public:
	std::string variable(int64_t index) const override { return index == 3 ? "alarm_raised" : MissionNames::variable(index); }
};

bms::Trigger variable_trigger(int32_t compare, int32_t variable, int32_t value) {
	bms::Trigger trigger{};
	trigger.main_type = bms::TriggerMainType::MissionVariable;
	trigger.sub_type = compare;
	trigger.param1 = variable;
	trigger.param2 = value;
	return trigger;
}

bms::Action variable_action(int32_t change, int32_t variable, int32_t value) {
	bms::Action action{};
	action.action_type = bms::ActionType::MisvarChange;
	action.action_sub_type = change;
	action.param1 = variable;
	action.param2 = value;
	return action;
}

int test_variable_words() {
	const MissionNames base;
	TEST_EXPECT(base.variable(12) == "V12" && base.variable(0) == "V0");
	TEST_EXPECT(trigger_words(variable_trigger(1, 3, 2), base) == "V3 equals 2");
	TEST_EXPECT(action_words(variable_action(1, 3, 1), base, nullptr) == "set V3 to 1");
	const NamedVariables named;
	TEST_EXPECT(trigger_words(variable_trigger(1, 3, 2), named) == "alarm_raised equals 2");
	TEST_EXPECT(action_words(variable_action(2, 3, 5), named, nullptr) == "add 5 to alarm_raised");
	TEST_EXPECT(action_words(variable_action(2, 4, 5), named, nullptr) == "add 5 to V4");
	return 0;
}

int test_live_logic() {
	const ActivityView activity;
	TEST_EXPECT(activity.live_logic.empty() && activity.live_logic.events.empty() && activity.live_logic.serial == 0);
	LiveLogic live;
	live.mission = "missions/logic.bms";
	LiveEvent event;
	event.index = 4;
	event.truth = {1, 0, -1};
	live.events.push_back(event);
	TEST_EXPECT(!live.empty() && live.event(4) && live.event(4)->truth.size() == 3 && !live.event(0));
	TEST_EXPECT(live.event(4)->last_checked_tick == -1 && !live.event(4)->active && !live.event(4)->fired);
	return 0;
}

} // namespace

int main() {
	if (test_variable_words() != 0) return 1;
	if (test_live_logic() != 0) return 1;
	std::printf("editor_mission_names: all tests passed\n");
	return 0;
}
