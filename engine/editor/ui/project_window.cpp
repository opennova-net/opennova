#include <editor/ui/project_window.h>

#include <cstring>

#include <base/gameprofile/gameprofile.h>

#include <imgui.h>

namespace opennova::editor {

namespace {

void copy_into(char *buffer, size_t size, const std::string &text) {
	const size_t n = text.size() < size - 1 ? text.size() : size - 1;
	std::memcpy(buffer, text.data(), n);
	buffer[n] = '\0';
}

const char *game_display_name(const std::string &code) {
	const gameprofile::GameProfile *profile = gameprofile::gameprofile_by_code(code.c_str());
	return profile != nullptr ? profile->display_name : code.c_str();
}

} // namespace

void ProjectWindow::set_picked(PickPurpose purpose, const std::string &path) {
	if (purpose == PickPurpose::NewProjectLocation) copy_into(location_, sizeof(location_), path);
}

void ProjectWindow::draw(devtools::ImGuiPass &, uint64_t) {
	if (host_.view().project_open) {
		draw_open_project();
	} else {
		draw_home();
	}
}

// No project: make one or open one.
void ProjectWindow::draw_home() {
	const SessionView &v = host_.view();
	ImGui::SeparatorText("New project");
	ImGui::InputText("Name", title_, sizeof(title_));
	ImGui::InputText("Folder", location_, sizeof(location_));
	ImGui::SameLine();
	if (ImGui::Button("Browse...##location")) {
		EditorRequest pick = make_request(EditorRequestKind::PickDirectory);
		pick.purpose = PickPurpose::NewProjectLocation;
		host_.request(pick);
	}
	ImGui::TextDisabled("The folder is created if it does not exist; it must not already hold a project.");
	ImGui::BeginDisabled(title_[0] == '\0' || location_[0] == '\0');
	if (ImGui::Button("Create project")) {
		host_.request(make_request(EditorRequestKind::NewProject, location_, title_));
	}
	ImGui::EndDisabled();

	ImGui::SeparatorText("Open project");
	if (ImGui::Button("Open a project folder...")) {
		EditorRequest pick = make_request(EditorRequestKind::PickDirectory);
		pick.purpose = PickPurpose::OpenProject;
		host_.request(pick);
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
				if (ImGui::Selectable(root.c_str(), false, ImGuiSelectableFlags_SpanAllColumns |
				                                                    ImGuiSelectableFlags_AllowDoubleClick)) {
					host_.request(make_request(EditorRequestKind::OpenProject, root));
				}
				ImGui::TableNextColumn();
				if (ImGui::SmallButton("Forget")) host_.request(make_request(EditorRequestKind::ForgetRecent, root));
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

void ProjectWindow::draw_open_project() {
	const SessionView &v = host_.view();
	if (edited_title_revision_ != v.revision && std::string(edited_title_) != v.document.title) {
		// The field follows the document until the user types; a session change while
		// typing would otherwise wipe the edit.
		if (!ImGui::IsAnyItemActive()) copy_into(edited_title_, sizeof(edited_title_), v.document.title);
		edited_title_revision_ = v.revision;
	}
	if (runtime_revision_ != v.revision && !ImGui::IsAnyItemActive()) {
		copy_into(runtime_, sizeof(runtime_), v.runtime_executable);
		copy_into(retail_, sizeof(retail_), v.retail_directory);
		runtime_revision_ = v.revision;
	}

	ImGui::SeparatorText(v.document.title.c_str());
	ImGui::TextDisabled("%s", v.project_root.c_str());
	ImGui::Text("Game: %s", game_display_name(v.document.target_game));
	ImGui::Text("%zu file(s); %d of %d required file(s) missing", v.scan.entries.size(),
	            v.requirements.required_missing + v.requirements.required_wrong_kind,
	            v.requirements.required_total);

	ImGui::SeparatorText("Build and play");
	ImGui::BeginDisabled(v.build_running);
	if (ImGui::Button("Build")) host_.request(make_request(EditorRequestKind::Build));
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(v.play_state != PlayState::Stopped);
	if (ImGui::Button("Play")) host_.request(make_request(EditorRequestKind::Play));
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(v.play_state != PlayState::Running);
	if (ImGui::Button("Stop")) host_.request(make_request(EditorRequestKind::StopPlay));
	ImGui::EndDisabled();
	ImGui::BeginDisabled(v.build_running || v.play_state != PlayState::Stopped);
	bool retail = v.play_retail;
	if (ImGui::Checkbox("Play in retail", &retail)) {
		EditorRequest set = make_request(EditorRequestKind::SetPlayRetail);
		set.flag = retail;
		host_.request(set);
	}
	ImGui::EndDisabled();
	if (v.build_running) {
		const float fraction = v.build_total == 0 ? 0.0f : static_cast<float>(v.build_done) / static_cast<float>(v.build_total);
		ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f), v.build_step.empty() ? "Preparing..." : v.build_step.c_str());
	} else if (v.has_build && v.last_build.ok) {
		ImGui::TextWrapped("Last build: %s", v.last_build.build_dir.c_str());
	}
	if (v.play_state != PlayState::Stopped) {
		ImGui::Text("Game %s (process %lld)", play_state_label(v.play_state),
		            static_cast<long long>(v.play_pid));
	}
	if (!v.status.empty()) ImGui::TextWrapped("%s", v.status.c_str());

	ImGui::SeparatorText("Settings");
	if (ImGui::InputText("Name##title", edited_title_, sizeof(edited_title_),
	                     ImGuiInputTextFlags_EnterReturnsTrue) && edited_title_[0] != '\0') {
		host_.request(make_request(EditorRequestKind::SetTitle, std::string(), edited_title_));
	}
	bool mission = v.document.features.mission;
	if (ImGui::Checkbox("Missions (adds the files a mission needs to the checklist)", &mission)) {
		EditorRequest set = make_request(EditorRequestKind::SetFeature, std::string(), "mission");
		set.flag = mission;
		host_.request(set);
	}
	bool multiplayer = v.document.features.multiplayer;
	if (ImGui::Checkbox("Multiplayer", &multiplayer)) {
		EditorRequest set = make_request(EditorRequestKind::SetFeature, std::string(), "multiplayer");
		set.flag = multiplayer;
		host_.request(set);
	}
	if (ImGui::InputText("Retail install", retail_, sizeof(retail_), ImGuiInputTextFlags_EnterReturnsTrue)) {
		host_.request(make_request(EditorRequestKind::SetRetailDirectory, retail_));
	}
	ImGui::SameLine();
	if (ImGui::Button("Browse...##retail")) {
		EditorRequest pick = make_request(EditorRequestKind::PickDirectory);
		pick.purpose = PickPurpose::RetailDirectory;
		host_.request(pick);
	}
	ImGui::TextDisabled("Joint Operations installation folder.");
	if (v.source_run) {
		ImGui::TextWrapped("OpenNova runtime (source checkout): %s", v.runtime_executable.c_str());
	} else {
		if (ImGui::InputText("OpenNova runtime", runtime_, sizeof(runtime_), ImGuiInputTextFlags_EnterReturnsTrue)) {
			host_.request(make_request(EditorRequestKind::SetRuntimeExecutable, runtime_));
		}
		ImGui::SameLine();
		if (ImGui::Button("Browse...##runtime")) {
			EditorRequest pick = make_request(EditorRequestKind::PickFile);
			pick.purpose = PickPurpose::RuntimeExecutable;
			host_.request(pick);
		}
		ImGui::TextDisabled("opennova.exe; leave empty for the one packaged beside the editor.");
	}
}

} // namespace opennova::editor
