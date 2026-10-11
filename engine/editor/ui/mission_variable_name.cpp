#include <editor/ui/mission_variable_name.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include <editor/documents/mission_document.h>
#include <editor/documents/mission_variable_names.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/ui_kit.h>

namespace opennova::editor {

namespace {

const ImVec4 kWarn(0.95f, 0.75f, 0.35f, 1.0f);

// What a field holds while it is typed in, and the refusal its last commit met, by document and variable.
struct FieldState {
	char text[32] = {};
	bool typing = false; // the field was active last frame: its text is the person's
	std::string refusal;
};
std::map<std::pair<uint64_t, int64_t>, FieldState> &field_states() {
	static std::map<std::pair<uint64_t, int64_t>, FieldState> states;
	return states;
}

} // namespace

void draw_variable_name_field(Workspace &workspace, const MissionDocument &mission, int64_t index, bool editable) {
	FieldState &state = field_states()[{mission.identity(), index}];
	const std::string name = mission_variable_name(mission, index);
	const bool layers = mission_variable_table(mission) == mission::LayerTable::LayerNames;
	ImGui::PushID("variable_name");
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted("Name");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
	// The mission's name while the field is not being typed in.
	if (!state.typing) std::snprintf(state.text, sizeof(state.text), "%s", name.c_str());
	ImGui::BeginDisabled(!editable || layers);
	const bool entered = ImGui::InputText("##name", state.text, sizeof(state.text), ImGuiInputTextFlags_EnterReturnsTrue);
	state.typing = ImGui::IsItemActive();
	ImGui::EndDisabled();
	const size_t room = mission::variable_name_room(int32_t(index));
	ui_kit::tooltip(layers ? std::string("The mission's layer table holds the original editor's layer names: no "
	                                     "variable name is written over them.")
	                       : "What the editor calls V" + std::to_string(index) + " in this mission, up to " +
	                                 std::to_string(room) + " characters. The game never reads it: it is kept in the "
	                                 "mission's layer table, which the game reads and drops. Empty takes the name away.");
	if ((entered || ImGui::IsItemDeactivatedAfterEdit()) && editable && !layers && name != state.text) {
		std::vector<Edit> edits;
		state.refusal.clear();
		if (!mission_variable_name_edits(mission, index, state.text, edits, state.refusal)) {
			std::snprintf(state.text, sizeof(state.text), "%s", name.c_str());
		} else if (!edits.empty()) {
			window_requests::edits(workspace, mission, std::move(edits));
		}
	}
	if (!state.refusal.empty()) {
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextColored(kWarn, "%s", state.refusal.c_str());
		ImGui::PopTextWrapPos();
	}
	ImGui::PopID();
}

void draw_variable_names_left(const MissionDocument &mission) {
	if (mission_variable_table(mission) == mission::LayerTable::LayerNames) {
		ImGui::TextDisabled("Variables take no name here: the layer table holds the original editor's layer names.");
		return;
	}
	const size_t left = mission_variable_names_left(mission);
	ImGui::TextDisabled("%zu more variable%s can take a name (%zu at most).", left, left == 1 ? "" : "s",
	                    mission::kMaxVariableNames);
}

} // namespace opennova::editor
