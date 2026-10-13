#include "mission_variable_names.h"

#include <editor/documents/mission_document.h>
#include <editor/documents/mission_names_file.h>
#include <formats/mission/mission_mis.h>

namespace opennova::editor {

std::vector<MissionVariableName> mission_variable_names(const MissionDocument &document) {
	std::vector<MissionVariableName> out;
	const auto *state = dynamic_cast<const MissionFileState *>(document.file_state());
	if (!state) return out;
	for (size_t i = 0; i < state->variables.size(); ++i)
		if (!state->variables[i].empty()) out.push_back({int32_t(i), state->variables[i]});
	return out;
}

std::string mission_variable_name(const MissionDocument &document, int64_t index) {
	const auto *state = dynamic_cast<const MissionFileState *>(document.file_state());
	return state && index >= 0 && size_t(index) < state->variables.size() ? state->variables[size_t(index)] : std::string();
}

size_t mission_variable_names_left(const MissionDocument &document) {
	return mission::kVariableNameCount - mission_variable_names(document).size();
}

bool mission_variable_name_edits(const MissionDocument &document, int64_t index, const std::string &name,
                                 std::vector<Edit> &out, std::string &error) {
	out.clear();
	if (index < 0 || index > mission::kVariableNameIndexMax) {
		error = "A variable name names V0 to V" + std::to_string(mission::kVariableNameIndexMax) + ".";
		return false;
	}
	if (!name.empty() && !mission::mis_name_allowed(name, mission::kMisDescriptionChars, error)) return false;
	if (mission_variable_name(document, index) == name) return true;
	Edit edit;
	edit.operation = EditOperation::SetFileValue;
	edit.field = kMisVariableField;
	edit.position = size_t(index);
	edit.value = name;
	out.push_back(std::move(edit));
	return true;
}

} // namespace opennova::editor
