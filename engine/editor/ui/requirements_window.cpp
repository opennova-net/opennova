#include <editor/ui/requirements_window.h>

#include <editor/assets/asset_kind.h>
#include <editor/blank/blank_factory.h>

#include <imgui.h>

namespace opennova::editor {

namespace {

const char *state_label(RequirementState state) {
	switch (state) {
	case RequirementState::Present: return "Present";
	case RequirementState::Missing: return "Missing";
	case RequirementState::WrongKind: return "Wrong kind of file";
	}
	return "";
}

ImVec4 state_color(RequirementState state) {
	switch (state) {
	case RequirementState::Present: return ImVec4(0.55f, 0.85f, 0.55f, 1.0f);
	case RequirementState::Missing: return ImVec4(0.95f, 0.60f, 0.45f, 1.0f);
	case RequirementState::WrongKind: return ImVec4(0.95f, 0.80f, 0.40f, 1.0f);
	}
	return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
}

} // namespace

void RequirementsWindow::draw(devtools::ImGuiPass &, uint64_t) {
	const SessionView &v = host_.view();
	if (!v.project_open) {
		ImGui::TextDisabled("Open a project to see the files it needs.");
		return;
	}
	const RequirementReport &r = v.requirements;
	const int unmet = r.required_missing + r.required_wrong_kind;
	if (unmet == 0) {
		ImGui::TextColored(state_color(RequirementState::Present), "Every required file is present.");
	} else {
		ImGui::TextColored(state_color(RequirementState::Missing), "%d of %d required file(s) missing.",
		                   unmet, r.required_total);
	}
	ImGui::BeginDisabled(r.required_missing == 0);
	if (ImGui::Button("Create all missing")) host_.request(make_request(EditorRequestKind::CreateMissing));
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::Button("Refresh")) host_.request(make_request(EditorRequestKind::Rescan));
	ImGui::TextDisabled("A file of the wrong kind is never replaced: rename or remove it first.");

	ImGui::SeparatorText("Required");
	draw_rows(true);
	if (ImGui::CollapsingHeader("Optional")) draw_rows(false);
}

void RequirementsWindow::draw_rows(bool required) {
	const SessionView &v = host_.view();
	const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
	                              ImGuiTableFlags_SizingStretchProp;
	if (!ImGui::BeginTable(required ? "required" : "optional", 4, flags)) return;
	ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch, 3.0f);
	ImGui::TableSetupColumn("Needed for", ImGuiTableColumnFlags_WidthStretch, 2.0f);
	ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 2.0f);
	ImGui::TableSetupColumn("##action", ImGuiTableColumnFlags_WidthFixed);
	ImGui::TableHeadersRow();
	for (const RequirementRow &row : v.requirements.rows) {
		if (row.required != required) continue;
		ImGui::PushID(row.role.c_str());
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::TextUnformatted(row.name.c_str());
		if (ImGui::IsItemHovered() && row.resource != nullptr && row.resource->failure != nullptr) {
			ImGui::SetTooltip("Without it: %s\nKind: %s", row.resource->failure, asset_kind_label(row.expected_kind));
		}
		ImGui::TableNextColumn();
		ImGui::TextUnformatted(requirement_phase_label(row.phase));
		ImGui::TableNextColumn();
		ImGui::TextColored(state_color(row.state), "%s", state_label(row.state));
		if (row.state == RequirementState::WrongKind && ImGui::IsItemHovered()) {
			ImGui::SetTooltip("%s is a %s, not a %s", row.asset_path.c_str(), asset_kind_label(row.found_kind),
			                  asset_kind_label(row.expected_kind));
		}
		ImGui::TableNextColumn();
		if (row.state == RequirementState::Missing) {
			const BlankFactory *factory = find_blank_factory_for_role(row.role);
			if (factory != nullptr) {
				if (ImGui::SmallButton("Create")) {
					host_.request(make_request(EditorRequestKind::CreateMissing, std::string(), row.role));
				}
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("Creates %s", factory->summary);
			} else {
				ImGui::TextDisabled("no writer yet");
			}
		}
		ImGui::PopID();
	}
	ImGui::EndTable();
}

} // namespace opennova::editor
