#include <editor/ui/editor_windows.h>

#include <memory>
#include <utility>

#include <editor/ui/assets_window.h>
#include <editor/ui/catalog_window.h>
#include <editor/ui/output_window.h>
#include <editor/ui/problems_window.h>
#include <editor/ui/project_window.h>
#include <editor/ui/requirements_window.h>

#include <imgui.h>

namespace opennova::editor {

EditorWindows::EditorWindows() {
	auto project = std::make_unique<ProjectWindow>(*this);
	project_window_ = project.get();
	pass_.register_window(std::move(project));
	pass_.register_window(std::make_unique<AssetsWindow>(*this));
	pass_.register_window(std::make_unique<CatalogWindow>(*this));
	pass_.register_window(std::make_unique<CatalogInspector>(*this));
	pass_.register_window(std::make_unique<RequirementsWindow>(*this));
	pass_.register_window(std::make_unique<ProblemsWindow>(*this));
	pass_.register_window(std::make_unique<OutputWindow>(*this));
	pass_.set_menu_bar_contributor(this);
	pass_.set_open(true); // the editor's workspace has no closed state
}

EditorWindows::~EditorWindows() {
	pass_.set_menu_bar_contributor(nullptr);
}

const SessionView &EditorWindows::view() const {
	return view_ != nullptr ? *view_ : empty_;
}

void EditorWindows::request(EditorRequest request) {
	requests_.push_back(std::move(request));
}

bool EditorWindows::take_request(EditorRequest &out) {
	if (requests_.empty()) return false;
	out = std::move(requests_.front());
	requests_.pop_front();
	return true;
}

void EditorWindows::deliver_pick(PickPurpose purpose, const std::string &path) {
	if (path.empty()) return; // cancelled
	switch (purpose) {
	case PickPurpose::NewProjectLocation: project_window_->set_picked(purpose, path); break;
	case PickPurpose::OpenProject: request(make_request(EditorRequestKind::OpenProject, path)); break;
	case PickPurpose::RuntimeExecutable:
		request(make_request(EditorRequestKind::SetRuntimeExecutable, path));
		break;
	case PickPurpose::RetailDirectory:
		request(make_request(EditorRequestKind::SetRetailDirectory, path));
		break;
	case PickPurpose::ImportFiles: break; // the multi-file result goes directly to the session
	case PickPurpose::None: break;
	}
}

void EditorWindows::draw_menu_bar(devtools::ImGuiPass &) {
	const SessionView &v = view();
	if (ImGui::BeginMenu("Project")) {
		if (ImGui::MenuItem("Open...")) {
			EditorRequest pick = make_request(EditorRequestKind::PickDirectory);
			pick.purpose = PickPurpose::OpenProject;
			request(pick);
		}
		if (ImGui::BeginMenu("Open recent", !v.recent_projects.empty())) {
			for (const std::string &root : v.recent_projects) {
				if (ImGui::MenuItem(root.c_str())) request(make_request(EditorRequestKind::OpenProject, root));
			}
			ImGui::EndMenu();
		}
		if (ImGui::MenuItem("Save", "Ctrl+S", false, !v.active_document.empty())) request(make_request(EditorRequestKind::Save));
		if (ImGui::MenuItem("Save All", "Ctrl+Shift+S", false, !v.documents.empty())) request(make_request(EditorRequestKind::SaveAll));
		if (ImGui::MenuItem("Refresh files", nullptr, false, v.project_open)) {
			request(make_request(EditorRequestKind::Rescan));
		}
		if (ImGui::MenuItem("Show project folder", nullptr, false, v.project_open)) {
			request(make_request(EditorRequestKind::RevealPath, v.project_root));
		}
		if (ImGui::MenuItem("Close project", nullptr, false, v.project_open)) {
			request(make_request(EditorRequestKind::CloseProject));
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Quit")) request(make_request(EditorRequestKind::Quit));
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("Build")) {
		if (ImGui::MenuItem("Build", "Ctrl+B", false, v.project_open && !v.build_running)) {
			request(make_request(EditorRequestKind::Build));
		}
		if (ImGui::MenuItem("Show build folder", nullptr, false, v.has_build && v.last_build.ok)) {
			request(make_request(EditorRequestKind::RevealPath, v.last_build.build_dir));
		}
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("Play")) {
		if (ImGui::MenuItem("Play", "F5", false, v.project_open && v.play_state == PlayState::Stopped)) {
			request(make_request(EditorRequestKind::Play));
		}
		if (ImGui::MenuItem("Stop", "Shift+F5", false, v.play_state == PlayState::Running)) {
			request(make_request(EditorRequestKind::StopPlay));
		}
		ImGui::EndMenu();
	}

	const EditableDocument *document = nullptr;
	for (const auto &d : v.documents) if (d->path() == v.active_document) document = d.get();
	if (ImGui::BeginMenu("Edit")) {
		if (ImGui::MenuItem("Undo", "Ctrl+Z", false, document && document->can_undo())) request(make_request(EditorRequestKind::Undo));
		if (ImGui::MenuItem("Redo", "Ctrl+Y", false, document && document->can_redo())) request(make_request(EditorRequestKind::Redo));
		ImGui::EndMenu();
	}
	if (v.unsaved_prompt) ImGui::OpenPopup("Unsaved changes");
	if (ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::TextUnformatted("Save the edited files before continuing?");
		for (const auto choice : {UnsavedChoice::SaveAll, UnsavedChoice::Discard, UnsavedChoice::Cancel}) {
			if (choice != UnsavedChoice::SaveAll) ImGui::SameLine();
			if (ImGui::Button(choice == UnsavedChoice::SaveAll ? "Save All" : choice == UnsavedChoice::Discard ? "Discard" : "Cancel")) {
				auto action = make_request(EditorRequestKind::ResolveUnsaved);
				action.unsaved_choice = choice; request(std::move(action)); ImGui::CloseCurrentPopup();
			}
		}
		ImGui::EndPopup();
	}

	// The shortcuts the menu labels promise.
	const ImGuiIO &io = ImGui::GetIO();
	if (!io.WantTextInput) {
		if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false))
			request(make_request(io.KeyShift ? EditorRequestKind::SaveAll : EditorRequestKind::Save));
		if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) request(make_request(EditorRequestKind::Undo));
		if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) request(make_request(EditorRequestKind::Redo));
		if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_B, false) && v.project_open && !v.build_running) {
			request(make_request(EditorRequestKind::Build));
		}
		if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) {
			if (io.KeyShift && v.play_state == PlayState::Running) {
				request(make_request(EditorRequestKind::StopPlay));
			} else if (!io.KeyShift && v.project_open && v.play_state == PlayState::Stopped) {
				request(make_request(EditorRequestKind::Play));
			}
		}
	}
}

} // namespace opennova::editor
