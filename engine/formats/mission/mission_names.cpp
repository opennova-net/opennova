#include "mission_names.h"

// Display names for the BMS event-logic enums, each read from the rows that hold it: the main types
// and the action types from the field rows (mission_field.cpp), the sub-types from the parameter rows
// (mission_params.cpp). One copy of each name, the original editor's tokens [orig: dfx2med
// Med_TriggerConditionName @0x446330, Med_ActionSubTypeName @0x445EE0].

#include "mission_detail.h"

#include <formats/mission/mission_field.h>
#include <formats/mission/mission_params.h>

namespace opennova::mission::detail {

namespace {

std::string name_in(const MissionChoices &choices, int value) {
	if (const char *name = mission_choice_name(choices.rows, choices.count, value)) return name;
	return unknown_label("Unknown", value);
}

} // namespace

std::string trigger_main_type_name(int value) { return name_in(trigger_main_types(), value); }

std::string trigger_sub_type_name(int main_type, int sub_type) {
	return name_in(trigger_sub_types(main_type), sub_type);
}

std::string action_type_name(int value) { return name_in(action_types(), value); }

std::string action_sub_type_name(int action_type, int sub_type) {
	return name_in(action_sub_types(action_type), sub_type);
}

} // namespace opennova::mission::detail
