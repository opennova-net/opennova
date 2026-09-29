#include <editor/ui/welcome_view.h>

#include <algorithm>
#include <cstring>

#include <editor/ui/ui_kit.h>

#include <imgui.h>

namespace opennova::editor {

bool NewProjectForm::draw(EditorHost &host) {
	ImGui::InputText("Name", title_, sizeof(title_));
	// Browse... beside the folder, under it when the view is narrow.
	ui_kit::WrapRow row;
	row.next(ui_kit::field_width(ImGui::CalcItemWidth(), "Folder"));
	ImGui::InputText("Folder", folder_, sizeof(folder_));
	row.next(ui_kit::button_width("Browse...##folder"));
	if (ImGui::Button("Browse...##folder")) {
		EditorRequest pick = make_request(EditorRequestKind::PickDirectory);
		pick.purpose = PickPurpose::NewProjectLocation;
		host.request(pick);
	}
	ImGui::TextDisabled("The folder is created if it does not exist; it must not already hold a project.");
	const bool ready = title_[0] != '\0' && folder_[0] != '\0';
	ImGui::BeginDisabled(!ready);
	const bool create = ImGui::Button("Create project") && ready;
	ImGui::EndDisabled();
	if (create) host.request(make_request(EditorRequestKind::NewProject, folder_, title_));
	return create;
}

void NewProjectForm::set_folder(const std::string &path) {
	const size_t n = std::min(path.size(), sizeof(folder_) - 1);
	std::memcpy(folder_, path.data(), n);
	folder_[n] = '\0';
}

void draw_welcome(EditorHost &host, NewProjectForm &form) {
	const SessionView &v = host.view();
	ImGui::SeparatorText("New project");
	form.draw(host);

	ImGui::SeparatorText("Open project");
	if (ImGui::Button("Open a project folder...")) {
		EditorRequest pick = make_request(EditorRequestKind::PickDirectory);
		pick.purpose = PickPurpose::OpenProject;
		host.request(pick);
	}
	if (!v.recent_projects.empty()) {
		ImGui::TextUnformatted("Recent");
		if (ImGui::BeginTable("recent", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
			ImGui::TableSetupColumn("Project", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("##forget", ImGuiTableColumnFlags_WidthFixed);
			for (const std::string &root : v.recent_projects) {
				ImGui::PushID(root.c_str());
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				// The folder cut to its cell (whole in its tooltip).
				const std::string shown = ui_kit::fit(root, ImGui::GetContentRegionAvail().x);
				if (ImGui::Selectable((shown + "###root").c_str(), false,
				                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick |
				                              ImGuiSelectableFlags_AllowOverlap)) {
					host.request(make_request(EditorRequestKind::OpenProject, root));
				}
				ui_kit::tooltip(shown != root ? "Open " + root : "Open it.");
				ImGui::TableNextColumn();
				if (ImGui::SmallButton("Forget")) host.request(make_request(EditorRequestKind::ForgetRecent, root));
				ui_kit::tooltip("Take it off this list; the project stays where it is.");
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
	}
	if (!v.status.empty()) {
		ImGui::Spacing();
		ImGui::TextWrapped("%s", v.status.c_str());
	}
}

} // namespace opennova::editor
