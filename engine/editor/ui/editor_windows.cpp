#include <editor/ui/editor_windows.h>

#include <algorithm>
#include <memory>
#include <utility>

#include <editor/project/project_files.h>
#include <editor/ui/document_window.h>
#include <editor/ui/inspector_window.h>
#include <editor/ui/output_window.h>
#include <editor/ui/preview_window.h>
#include <editor/ui/problems_window.h>
#include <editor/ui/ui_kit.h>

#include <imgui.h>

namespace opennova::editor {
namespace {

// The requests that act on the files as saved: the ones that write them (Save, Save All),
// pack them (Build, Play) or answer the unsaved prompt, and the ones that ask whether a
// document has unsaved edits (Close, Reload, a project switch, Quit, Import, Rename and
// Assign and a rename everywhere, which the prompt guards; Rescan, which reads again the clean
// documents whose files changed). Raised while a frame draws, they wait for the frame's other requests: the
// menu bar draws before the windows, so a Ctrl+S pressed as the inspector sets a field
// would otherwise save the document without it.
bool acts_on_saved_files(EditorRequestKind kind) {
	switch (kind) {
	case EditorRequestKind::Save:
	case EditorRequestKind::SaveAll:
	case EditorRequestKind::Build:
	case EditorRequestKind::Play:
	case EditorRequestKind::ResolveUnsaved:
	case EditorRequestKind::CloseDocument:
	case EditorRequestKind::ReloadDocument:
	case EditorRequestKind::NewProject:
	case EditorRequestKind::OpenProject:
	case EditorRequestKind::CloseProject:
	case EditorRequestKind::Quit:
	case EditorRequestKind::Rescan:
	case EditorRequestKind::ImportFiles:
	case EditorRequestKind::RenameAsset:
	case EditorRequestKind::AssignRequirement:
	case EditorRequestKind::RenameSymbol: return true;
	default: return false;
	}
}

// The requests whose empty path means the active document (the session's document_for).
// Raised without a path, they name the one active when they were raised (request()), so a
// Save that waits for the frame's other requests still saves it when a click in the same
// frame makes another document active first.
bool names_the_active_document(EditorRequestKind kind) {
	switch (kind) {
	case EditorRequestKind::OpenDocument:
	case EditorRequestKind::ReloadDocument:
	case EditorRequestKind::CloseDocument:
	case EditorRequestKind::SelectRecord:
	case EditorRequestKind::EditRecord:
	case EditorRequestKind::RevertToSaved:
	case EditorRequestKind::EndEdit:
	case EditorRequestKind::Copy:
	case EditorRequestKind::Cut:
	case EditorRequestKind::Paste:
	case EditorRequestKind::Duplicate:
	case EditorRequestKind::Save:
	case EditorRequestKind::Undo:
	case EditorRequestKind::Redo: return true;
	default: return false;
	}
}

// What waits on the unsaved prompt, in the words of the menu that asked for it.
std::string waiting_action(const SessionView::UnsavedPrompt &prompt) {
	const std::string file = basename_of(prompt.target);
	switch (prompt.action) {
	case EditorRequestKind::CloseDocument: return "Close " + file;
	case EditorRequestKind::ReloadDocument: return "Reload " + file;
	case EditorRequestKind::Build: return "Build";
	case EditorRequestKind::Play: return "Play";
	case EditorRequestKind::ImportFiles: return "Import";
	case EditorRequestKind::RenameAsset:
	case EditorRequestKind::AssignRequirement: return "Rename " + file;
	case EditorRequestKind::RenameSymbol: return "Rename everywhere (defined in " + file + ")";
	case EditorRequestKind::NewProject: return "Create a new project";
	case EditorRequestKind::OpenProject: return "Open another project";
	case EditorRequestKind::CloseProject: return "Close the project";
	default: return "Quit";
	}
}

// The session's unsaved-changes prompt: what waits, the files it lists and the answers that
// fit it. One file (a Close or a Reload): Save / Don't save / Cancel; a project switch or
// Quit: Save all / Discard / Cancel; Build and Play, which pack the files on disk, and an
// import or a rename, which write over them: Save all and build (play, import, rename) /
// Cancel.
void draw_unsaved_prompt(EditorHost &host, const SessionView::UnsavedPrompt &prompt) {
	if (prompt.open) ImGui::OpenPopup("Unsaved changes");
	if (!ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
	if (!prompt.open) { // answered another way (the editor MCP)
		ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		return;
	}
	ImGui::TextUnformatted(waiting_action(prompt).c_str());
	ImGui::TextUnformatted(prompt.files.size() == 1 ? "This file has unsaved changes:" : "These files have unsaved changes:");
	for (const std::string &file : prompt.files) ImGui::BulletText("%s", file.c_str());
	const bool one_file = prompt.action == EditorRequestKind::CloseDocument || prompt.action == EditorRequestKind::ReloadDocument;
	const bool renames = prompt.action == EditorRequestKind::RenameAsset || prompt.action == EditorRequestKind::AssignRequirement ||
	                     prompt.action == EditorRequestKind::RenameSymbol;
	const char *save = one_file                                           ? "Save"
	                   : prompt.action == EditorRequestKind::Build       ? "Save all and build"
	                   : prompt.action == EditorRequestKind::Play        ? "Save all and play"
	                   : prompt.action == EditorRequestKind::ImportFiles ? "Save all and import"
	                   : renames                                         ? "Save all and rename"
	                                                                     : "Save all";
	const auto answer = [&](UnsavedChoice choice) {
		auto request = make_request(EditorRequestKind::ResolveUnsaved);
		request.unsaved_choice = choice;
		host.request(std::move(request));
		ImGui::CloseCurrentPopup();
	};
	if (ImGui::Button(save)) answer(UnsavedChoice::Save);
	if (prompt.can_discard) {
		ImGui::SameLine();
		if (ImGui::Button(one_file ? "Don't save" : "Discard")) answer(UnsavedChoice::Discard);
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel")) answer(UnsavedChoice::Cancel);
	ImGui::EndPopup();
}

// The workspace's first layout: the bottom strip under everything, a quarter of the height;
// above it Files 260 pixels of a 1280-wide window and the Inspector 320, kept as shares of
// the width a window has when the layout is built (each ratio of the node it splits: the
// Inspector's of what Files leaves); the centre 37.5% Document, 62.5% Preview. Once
// built, Document has the focus and Problems is the bottom's tab. The dockspace id is the
// editor's own: a layout saved before the workspace had these windows is not found, so this
// one is built once, and the user's docking persists after.
devtools::DockLayout editor_layout() {
	devtools::DockLayout layout;
	layout.dockspace = "OpenNovaEditorWorkspace.v2";
	layout.bottom_full_width = true;
	layout.bottom = 0.25f;
	layout.left = 260.0f / 1280.0f;
	layout.right = 320.0f / (1280.0f - 260.0f);
	layout.center_right = 0.625f;
	layout.focus = {"Document", "Problems"};
	return layout;
}

const Document *active_document(const SessionView &v) {
	for (const auto &document : v.documents)
		if (document->path() == v.active_document) return document.get();
	return nullptr;
}

// A menu item chosen only while it can run: a disabled one is never chosen, however it is
// activated.
bool menu_item(const char *label, const char *shortcut, bool enabled) {
	return ImGui::MenuItem(label, shortcut, false, enabled) && enabled;
}

// A button pressed only while it can run, likewise.
bool enabled_button(const char *label, bool enabled) {
	ImGui::BeginDisabled(!enabled);
	const bool pressed = ImGui::Button(label);
	ImGui::EndDisabled();
	return pressed && enabled;
}

} // namespace

EditorWindows::EditorWindows() {
	{
		auto files = std::make_unique<FilesWindow>(*this, new_file_);
		files_window_ = files.get();
		pass_.register_window(std::move(files));
	}
	{
		auto document = std::make_unique<DocumentWindow>(*this, new_project_);
		document_window_ = document.get();
		pass_.register_window(std::move(document));
	}
	{
		auto preview = std::make_unique<PreviewWindow>(*this);
		preview_window_ = preview.get();
		pass_.register_window(std::move(preview));
	}
	pass_.register_window(std::make_unique<InspectorWindow>(*this));
	problems_window_ = &pass_.register_window(std::make_unique<ProblemsWindow>(*this));
	pass_.register_window(std::make_unique<OutputWindow>(*this));
	pass_.set_dock_layout(editor_layout());
	pass_.set_menu_bar_contributor(this);
	pass_.set_open(true); // the editor's workspace has no closed state
	pass_.set_closeable(false);
}

EditorWindows::~EditorWindows() {
	pass_.set_menu_bar_contributor(nullptr);
}

const SessionView &EditorWindows::view() const {
	return view_ != nullptr ? *view_ : empty_;
}

void EditorWindows::request(EditorRequest request) {
	if (request.path.empty() && names_the_active_document(request.kind)) request.path = view().active_document;
	if (in_frame_ && acts_on_saved_files(request.kind)) deferred_.push_back(std::move(request));
	else requests_.push_back(std::move(request));
}

bool EditorWindows::draw_frame(uint64_t frame_index) {
	begin_frame();
	const bool drew = pass_.draw_frame(frame_index);
	end_frame();
	return drew;
}

void EditorWindows::begin_frame() {
	in_frame_ = true;
	// A ShowInFiles brings Files forward, whether it draws this frame or not; a PreviewRename
	// that asks the new name opens Rename everywhere.
	if (files_window_) files_window_->follow_reveal(view());
	rename_.follow(view());
}

void EditorWindows::end_frame() {
	// Preview not drawn this frame (closed, collapsed, its tab hidden): its gestures end,
	// before what waits on the files as saved.
	if (preview_window_) preview_window_->end_frame();
	in_frame_ = false;
	for (EditorRequest &request : deferred_) requests_.push_back(std::move(request));
	deferred_.clear();
}

bool EditorWindows::take_request(EditorRequest &out) {
	if (requests_.empty()) return false;
	out = std::move(requests_.front());
	requests_.pop_front();
	return true;
}

void EditorWindows::set_menu_preview_viewport(MenuPreviewViewport *viewport) {
	if (preview_window_) preview_window_->set_menu_viewport(viewport);
}

void EditorWindows::set_model_preview_viewport(ModelPreviewViewport *viewport) {
	if (preview_window_) preview_window_->set_model_viewport(viewport);
}

void EditorWindows::deliver_pick(PickPurpose purpose, const std::string &path) {
	if (path.empty()) return; // cancelled
	switch (purpose) {
	case PickPurpose::NewProjectLocation: new_project_.set_folder(path); break;
	case PickPurpose::OpenProject: request(make_request(EditorRequestKind::OpenProject, path)); break;
	case PickPurpose::RuntimeExecutable:
	case PickPurpose::RetailDirectory: settings_.set_picked(purpose, path, view().project_root); break;
	case PickPurpose::ImportFiles: break; // the multi-file result goes directly to the session
	case PickPurpose::None: break;
	}
}

void EditorWindows::draw_menu_bar(devtools::ImGuiPass &) {
	const SessionView &v = view();
	const Document *document = active_document(v);
	draw_file_menu(v);
	draw_edit_menu(v, document);
	draw_build_menu(v);
	// The modals, every frame, whichever window or menu opened them.
	draw_unsaved_prompt(*this, v.unsaved_prompt);
	import_.draw(*this);
	settings_.draw(*this);
	draw_new_project();
	new_file_.draw(*this);
	find_.draw(*this);
	rename_.draw(*this);
	if (document_window_) document_window_->draw_modals();
	shortcuts(v, document);
}

void EditorWindows::draw_file_menu(const SessionView &v) {
	if (!ImGui::BeginMenu("File")) return;
	if (ImGui::MenuItem("New project...")) open_new_project_ = true;
	if (ImGui::MenuItem("Open project...")) {
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
	ImGui::Separator();
	if (menu_item("Save", "Ctrl+S", !v.active_document.empty())) request(make_request(EditorRequestKind::Save));
	if (menu_item("Save All", "Ctrl+Shift+S", !v.documents.empty())) request(make_request(EditorRequestKind::SaveAll));
	if (menu_item("Close file", "Ctrl+W", !v.active_document.empty())) request(make_request(EditorRequestKind::CloseDocument));
	ImGui::Separator();
	if (menu_item("Import files...", nullptr, v.project_open)) {
		EditorRequest pick = make_request(EditorRequestKind::PickFile);
		pick.purpose = PickPurpose::ImportFiles;
		request(pick);
	}
	if (menu_item("Import from the game data...", nullptr, v.project_open && !v.retail_directory.empty())) {
		EditorRequest listed = make_request(EditorRequestKind::PreviewRetailImport);
		listed.flag = v.import_dependencies;
		request(listed);
	}
	if (v.project_open && v.retail_directory.empty())
		ui_kit::tooltip("Choose the game install folder in File > Project settings... first.");
	ImGui::Separator();
	if (menu_item("Project settings...", nullptr, v.project_open)) settings_.open(v);
	if (menu_item("Show project folder", nullptr, v.project_open))
		request(make_request(EditorRequestKind::RevealPath, v.project_root));
	if (menu_item("Close project", nullptr, v.project_open)) request(make_request(EditorRequestKind::CloseProject));
	ImGui::Separator();
	if (ImGui::MenuItem("Quit")) request(make_request(EditorRequestKind::Quit));
	ImGui::EndMenu();
}

void EditorWindows::draw_edit_menu(const SessionView &v, const Document *document) {
	if (!ImGui::BeginMenu("Edit")) return;
	if (menu_item("Undo", "Ctrl+Z", document && document->can_undo())) request(make_request(EditorRequestKind::Undo));
	if (menu_item("Redo", "Ctrl+Y", document && document->can_redo())) request(make_request(EditorRequestKind::Redo));
	ImGui::Separator();
	if (menu_item("Find...", "Ctrl+F", document != nullptr) && document_window_) document_window_->open_find();
	if (menu_item("Find in project...", "Ctrl+Shift+F", v.project_open && v.graph)) find_.open();
	ImGui::EndMenu();
}

void EditorWindows::draw_build_menu(const SessionView &v) {
	if (!ImGui::BeginMenu("Build")) return;
	if (menu_item("Build", "Ctrl+B", v.project_open && !v.build_running)) request(make_request(EditorRequestKind::Build));
	if (menu_item("Play", "F5", v.project_open && v.play_state == PlayState::Stopped))
		request(make_request(EditorRequestKind::Play));
	if (menu_item("Stop", "Shift+F5", v.play_state == PlayState::Running)) request(make_request(EditorRequestKind::StopPlay));
	ImGui::Separator();
	bool retail = v.play_retail;
	const bool stopped = !v.build_running && v.play_state == PlayState::Stopped;
	if (ImGui::MenuItem("Play in the game install", nullptr, &retail, stopped) && stopped) {
		EditorRequest set = make_request(EditorRequestKind::ApplyProjectSettings);
		set.settings.play_retail = retail;
		request(set);
	}
	ui_kit::tooltip("Play starts the game install on the build instead of the OpenNova runtime.");
	if (menu_item("Show build folder", nullptr, v.has_build && v.last_build.ok))
		request(make_request(EditorRequestKind::RevealPath, v.last_build.build_dir));
	ImGui::EndMenu();
}

// File > New project...: the welcome view's form in a modal.
void EditorWindows::draw_new_project() {
	if (open_new_project_) {
		open_new_project_ = false;
		ImGui::OpenPopup("New project");
	}
	if (!ImGui::BeginPopupModal("New project", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
	if (new_project_.draw(*this)) ImGui::CloseCurrentPopup();
	ImGui::SameLine();
	if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}

// The shortcuts the menu labels promise (Ctrl+F is the Document window's own, before its views'
// filters). Saving, closing a file, finding in the project, building and playing work
// while a text field has the keyboard (what it typed this frame is raised first: request());
// Undo and Redo are the field's own then. Ctrl+Shift+Z is Redo, as Ctrl+Y is. None of them
// while the unsaved prompt is open: an Undo behind it would make a file it does not list
// unsaved.
void EditorWindows::shortcuts(const SessionView &v, const Document *document) {
	if (v.unsaved_prompt.open) return;
	const ImGuiIO &io = ImGui::GetIO();
	if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
		if (io.KeyShift && !v.documents.empty()) request(make_request(EditorRequestKind::SaveAll));
		else if (!io.KeyShift && !v.active_document.empty()) request(make_request(EditorRequestKind::Save));
	}
	if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_W, false) && !v.active_document.empty())
		request(make_request(EditorRequestKind::CloseDocument));
	if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_F, false) && v.project_open && v.graph) find_.open();
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
	if (!io.WantTextInput && document) {
		if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false))
			request(make_request(io.KeyShift ? EditorRequestKind::Redo : EditorRequestKind::Undo));
		if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) request(make_request(EditorRequestKind::Redo));
	}
}

// The bar's right end, right-aligned: what the session last said (cut to the room left),
// the files with unsaved changes (a click lists them: a click on one makes it the active
// document; Save all), the errors and warnings (a click shows Problems), the build or the
// game ("Building 3/12", "Built", "Game running"), then Build, Play and Stop. A bar too
// narrow for all of it leaves parts out from the left rather than run over the menus.
void EditorWindows::draw_menu_bar_trailing(devtools::ImGuiPass &) {
	const SessionView &v = view();
	if (!v.project_open) return;
	std::vector<const Document *> unsaved;
	for (const auto &document : v.documents)
		if (document->dirty()) unsaved.push_back(document.get());
	size_t errors = 0, warnings = 0, infos = 0;
	for (const Diagnostic &d : v.diagnostics) {
		if (d.severity == DiagnosticSeverity::Error) ++errors;
		else if (d.severity == DiagnosticSeverity::Warning) ++warnings;
		else ++infos;
	}
	const std::string unsaved_text = std::to_string(unsaved.size()) + " unsaved";
	std::string state, state_tip;
	if (v.build_running) {
		state = "Building " + std::to_string(v.build_done) + "/" + std::to_string(v.build_total);
		state_tip = v.build_step.empty() ? "Preparing..." : v.build_step;
	} else if (v.play_state == PlayState::Running) {
		state = v.play_retail ? "Game install running" : "Game running";
		state_tip = "Process " + std::to_string(v.play_pid) + ". Stop ends it.";
	} else if (v.play_state != PlayState::Stopped) {
		state = "Stopping the game";
	} else if (v.has_build) {
		state = v.last_build.ok ? "Built" : "Build failed";
		state_tip = v.last_build.ok ? "The last build: " + v.last_build.build_dir : "See Problems.";
	}

	// Each part's width, left to right: the unsaved files, the problem counts, the state and
	// the buttons (the bar's layout puts an item spacing between items), from the end of the
	// menus to the end of the bar's content.
	const float gap = ImGui::GetStyle().ItemSpacing.x;
	float widths[4] = {};
	if (!unsaved.empty()) widths[0] = ui_kit::unsaved_dot_width() + gap + ui_kit::text_width(unsaved_text.c_str());
	widths[1] = ui_kit::severity_count_width(errors) + gap + ui_kit::severity_count_width(warnings);
	if (!state.empty()) widths[2] = ui_kit::text_width(state.c_str());
	widths[3] = ui_kit::button_width("Build") + gap + ui_kit::button_width("Play") + gap + ui_kit::button_width("Stop");
	const float left = ImGui::GetCursorPosX();
	const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
	size_t first = 0; // the leftmost part shown
	float total = 0.0f;
	for (; first < 4; ++first) {
		total = 0.0f;
		for (size_t part = first; part < 4; ++part)
			if (widths[part] > 0.0f) total += (total > 0.0f ? gap : 0.0f) + widths[part];
		if (right - total >= left) break;
	}
	if (first == 4) return;
	// Before them, what the session last said, in the room the parts leave (up to a line of
	// a few words, and none in less than a word's room): cut to fit, whole in its tooltip
	// with the project's folder under it (Output names the project's files from its folder).
	const float room = std::min(right - total - gap - left, ImGui::GetFontSize() * 24.0f);
	const std::string status = first == 0 && !v.status.empty() && room >= ImGui::GetFontSize() * 4.0f
	                                   ? ui_kit::fit(v.status, room)
	                                   : std::string();
	const float status_width = status.empty() ? 0.0f : ui_kit::text_width(status.c_str()) + gap;
	ImGui::SetCursorPosX(right - total - status_width);
	ImGui::PushID("status");
	if (!status.empty()) {
		ImGui::TextDisabled("%s", status.c_str());
		ui_kit::tooltip(v.status + "\n" + v.project_root);
	}
	const float height = ImGui::GetFrameHeight();
	// A part a click acts on: a selectable as wide as the part, what it shows drawn over it
	// (back at its start: SameLine(x) would add the offset of the group the bar is).
	const auto clickable = [](const char *id, float width, const std::string &tip) {
		const float x = ImGui::GetCursorPosX();
		const bool clicked = ImGui::Selectable(id, false, ImGuiSelectableFlags_AllowOverlap, ImVec2(width, 0.0f));
		ui_kit::tooltip(tip);
		ImGui::SameLine(0.0f, 0.0f);
		ImGui::SetCursorPosX(x);
		return clicked;
	};
	if (first == 0 && !unsaved.empty()) {
		if (clickable("##unsaved", widths[0], "Files with unsaved changes: a click lists them.")) ImGui::OpenPopup("unsaved");
		ui_kit::unsaved_dot(height);
		ImGui::TextUnformatted(unsaved_text.c_str());
		if (ImGui::BeginPopup("unsaved")) {
			for (const Document *document : unsaved)
				if (ImGui::MenuItem(document->path().c_str())) request(make_request(EditorRequestKind::OpenDocument, document->path()));
			ImGui::Separator();
			if (ImGui::MenuItem("Save all")) request(make_request(EditorRequestKind::SaveAll));
			ImGui::EndPopup();
		}
	}
	if (first <= 1) {
		std::string tip = std::to_string(errors) + (errors == 1 ? " error, " : " errors, ") + std::to_string(warnings) +
		                  (warnings == 1 ? " warning, " : " warnings, ") + std::to_string(infos) + " info.";
		if (const int missing = v.requirements.required_missing + v.requirements.required_wrong_kind)
			tip += " " + std::to_string(missing) + " of " + std::to_string(v.requirements.required_total) +
			       " required files " + (missing == 1 ? "is" : "are") + " missing.";
		if (clickable("##problems", widths[1], tip + " A click shows Problems.") && problems_window_)
			problems_window_->request_focus();
		ui_kit::severity_count(DiagnosticSeverity::Error, errors, height);
		ui_kit::severity_count(DiagnosticSeverity::Warning, warnings, height);
	}
	if (first <= 2 && !state.empty()) {
		ImGui::TextUnformatted(state.c_str());
		ui_kit::tooltip(state_tip);
	}
	if (enabled_button("Build", !v.build_running)) request(make_request(EditorRequestKind::Build));
	ui_kit::tooltip("Build the project (Ctrl+B).");
	if (enabled_button("Play", v.play_state == PlayState::Stopped)) request(make_request(EditorRequestKind::Play));
	ui_kit::tooltip("Build, then run the game on the build (F5).");
	if (enabled_button("Stop", v.play_state == PlayState::Running)) request(make_request(EditorRequestKind::StopPlay));
	ui_kit::tooltip("Stop the game (Shift+F5).");
	ImGui::PopID();
}

} // namespace opennova::editor
