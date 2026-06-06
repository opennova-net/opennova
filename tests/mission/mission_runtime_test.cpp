#include <string>
#include <vector>

#include "common/test_expect.h"
#include "mission/bms.h"
#include "mission/mission.h"
#include "mission/mission_runtime.h"

namespace {

opennova::mission::MissionEventRecord make_event(int flags = 0) {
	opennova::mission::MissionEventRecord event;
	event.flags = flags;
	return event;
}

opennova::mission::MissionTriggerRecord make_mission_variable_trigger(int variable, int compare_value) {
	opennova::mission::MissionTriggerRecord trigger;
	trigger.main_type = static_cast<int>(opennova::bms::TriggerMainType::MissionVariable);
	trigger.sub_type = static_cast<int>(opennova::bms::MissionVariableTriggerType::MissionVariableIsGreaterThan);
	trigger.param1 = variable;
	trigger.param2 = compare_value;
	return trigger;
}

opennova::mission::MissionActionRecord make_action(opennova::bms::ActionType type, int p1 = 0, int p2 = 0) {
	opennova::mission::MissionActionRecord action;
	action.action_type = static_cast<int>(type);
	action.param1 = p1;
	action.param2 = p2;
	return action;
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

	const MissionParamSpec event_trigger = trigger_param_schema(static_cast<int>(opennova::bms::TriggerMainType::Event), 123);
	TEST_EXPECT(event_trigger.known);
	TEST_EXPECT(event_trigger.params[0].kind == MissionParamKind::Event);

	MissionDocument doc;
	doc.create_default();
	MissionEventRecord event0;
	TEST_EXPECT(doc.add_event(make_event(), &event0));
	TEST_EXPECT(event0.index == 0);
	TEST_EXPECT(doc.insert_event_action(0, 0, make_action(opennova::bms::ActionType::OutputText, 123)));

	MissionRuntime runtime;
	TEST_EXPECT(runtime.load(doc));
	MissionRuntimeTickResult result = runtime.tick();
	TEST_EXPECT(result.commands.size() == 1);
	TEST_EXPECT(result.commands[0].kind == MissionRuntimeCommandKind::OutputText);
	TEST_EXPECT(result.commands[0].event_index == 0);
	TEST_EXPECT(result.commands[0].action_index == 0);
	TEST_EXPECT(result.commands[0].param1 == 123);
	TEST_EXPECT(runtime.has_event_fired(0));
	TEST_EXPECT(runtime.tick().commands.empty());

	MissionDocument vars_doc;
	vars_doc.create_default();
	TEST_EXPECT(vars_doc.add_event(make_event()));
	TEST_EXPECT(vars_doc.insert_event_trigger(0, 0, make_mission_variable_trigger(2, 4)));
	MissionActionRecord add_var = make_action(opennova::bms::ActionType::MisvarChange, 2, 3);
	add_var.action_sub_type = static_cast<int>(opennova::bms::MissionVariableActionSubType::Add);
	TEST_EXPECT(vars_doc.insert_event_action(0, 0, add_var));

	MissionRuntime vars_runtime;
	TEST_EXPECT(vars_runtime.load(vars_doc));
	vars_runtime.set_mission_variable(2, 5);
	result = vars_runtime.tick();
	TEST_EXPECT(vars_runtime.get_mission_variable(2) == 8);
	TEST_EXPECT(result.commands.size() == 1);
	TEST_EXPECT(result.commands[0].kind == MissionRuntimeCommandKind::MissionVariableChanged);
	TEST_EXPECT(result.commands[0].param1 == 2);
	TEST_EXPECT(result.commands[0].param2 == 8);

	MissionDocument reset_doc;
	reset_doc.create_default();
	TEST_EXPECT(reset_doc.add_event(make_event()));
	TEST_EXPECT(reset_doc.insert_event_action(0, 0, make_action(opennova::bms::ActionType::OutputText, 7)));
	TEST_EXPECT(reset_doc.add_event(make_event()));
	TEST_EXPECT(reset_doc.insert_event_action(1, 0, make_action(opennova::bms::ActionType::ResetEvent, 0)));

	MissionRuntime reset_runtime;
	TEST_EXPECT(reset_runtime.load(reset_doc));
	result = reset_runtime.tick();
	TEST_EXPECT(result.commands.size() == 2);
	TEST_EXPECT(!reset_runtime.has_event_fired(0));
	TEST_EXPECT(reset_runtime.has_event_fired(1));
	result = reset_runtime.tick();
	TEST_EXPECT(result.commands.size() == 1);
	TEST_EXPECT(result.commands[0].kind == MissionRuntimeCommandKind::OutputText);
	TEST_EXPECT(result.commands[0].param1 == 7);

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
