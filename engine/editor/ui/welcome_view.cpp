#include <editor/ui/welcome_view.h>

#include <algorithm>
#include <cstring>

#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/ui_kit.h>

#include <imgui.h>

namespace opennova::editor {

bool NewProjectForm::draw(Workspace &workspace) {
	ImGui::InputText("Name", title_, sizeof(title_));
	// Browse... beside the folder, under it when the view is narrow.
	ui_kit::WrapRow row;
	row.next(ui_kit::field_width(ImGui::CalcItemWidth(), "Folder"));
	ImGui::InputText("Folder", folder_, sizeof(folder_));
	row.next(ui_kit::button_width("Browse...##folder"));
	// A new project is refused while an operation that cannot be cancelled runs: its Browse... and
	// Create wait with it (the busy gate's answer, SessionView::allows).
	const bool allowed = workspace.view().allows(EditorRequestKind::NewProject);
	ImGui::BeginDisabled(!allowed);
	if (ImGui::Button("Browse...##folder") && allowed)
		workspace.request(request::pick_directory(PickPurpose::NewProjectLocation));
	ImGui::EndDisabled();
	ImGui::TextDisabled("The folder is created if it does not exist; it must not already hold a project.");
	const bool expansion_ok = expansion_.draw(workspace.view().project.new_project_expansions);
	const bool ready = title_[0] != '\0' && folder_[0] != '\0' && allowed && expansion_ok;
	ImGui::BeginDisabled(!ready);
	const bool create = ImGui::Button("Create project") && ready;
	ImGui::EndDisabled();
	if (create) {
		const ProjectExpansion expansion = expansion_.value();
		workspace.request(expansion.standalone() ? request::new_project(folder_, title_)
		                                         : request::new_expansion_project(folder_, title_, expansion.name,
		                                                                          expansion.builds_on));
	}
	return create;
}

void NewProjectForm::set_folder(const std::string &path) {
	const size_t n = std::min(path.size(), sizeof(folder_) - 1);
	std::memcpy(folder_, path.data(), n);
	folder_[n] = '\0';
}

void draw_welcome(Workspace &workspace, NewProjectForm &form) {
	const SessionView &v = workspace.view();
	ImGui::SeparatorText("New project");
	form.draw(workspace);

	ImGui::SeparatorText("Open project");
	const bool opens = v.allows(EditorRequestKind::OpenProject);
	ImGui::BeginDisabled(!opens);
	if (ImGui::Button("Open a project folder...") && opens)
		workspace.request(request::pick_directory(PickPurpose::OpenProject));
	ImGui::EndDisabled();
	if (!v.project.recent_projects.empty()) {
		ImGui::TextUnformatted("Recent");
		if (ImGui::BeginTable("recent", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
			ImGui::TableSetupColumn("Project", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("##forget", ImGuiTableColumnFlags_WidthFixed);
			for (const std::string &root : v.project.recent_projects) {
				ImGui::PushID(root.c_str());
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				// The folder cut to its cell (whole in its tooltip).
				const std::string shown = ui_kit::fit(root, ImGui::GetContentRegionAvail().x);
				ImGuiSelectableFlags flags = ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick |
				                             ImGuiSelectableFlags_AllowOverlap;
				if (!opens) flags |= ImGuiSelectableFlags_Disabled;
				if (ImGui::Selectable((shown + "###root").c_str(), false, flags) && opens)
					workspace.request(request::open_project(root));
				ui_kit::tooltip(shown != root ? "Open " + root : "Open it.");
				ImGui::TableNextColumn();
				const bool forgets = v.allows(EditorRequestKind::ForgetRecent);
				ImGui::BeginDisabled(!forgets);
				if (ImGui::SmallButton("Forget") && forgets) workspace.request(request::forget_recent(root));
				ImGui::EndDisabled();
				ui_kit::tooltip("Take it off this list; the project stays where it is.");
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
	}
	if (!v.activity.status.empty()) {
		ImGui::Spacing();
		ImGui::TextWrapped("%s", v.activity.status.c_str());
	}
}

} // namespace opennova::editor
