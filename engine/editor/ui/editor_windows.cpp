#include <editor/ui/editor_windows.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <utility>

#include <editor/project_build/build_run.h>
#include <editor/session/build_result.h>
#include <editor/session/rename_controller.h>
#include <editor/project_build/export_build.h>
#include <editor/session/play_controller.h>
#include <editor/session/problem_query.h>
#include <editor/session/request_factories.h>
#include <editor/session/request_kinds.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/document_window.h>
#include <editor/ui/inspector_window.h>
#include <editor/ui/output_window.h>
#include <editor/ui/preview_window.h>
#include <editor/ui/problems_window.h>
#include <editor/ui/ui_kit.h>

#include <imgui.h>

namespace opennova::editor {
namespace {

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

// The session's unsaved-changes prompt: what waits (its row's words, waiting_words), the files it
// lists and the answers that fit it (its row's Save label, and Discard where its row offers it).
// One file (a Close or a Reload): Save / Don't save / Cancel; a project switch or Quit: Save all /
// Discard / Cancel; Build and Play, which pack the files on disk, and an import or a rename, which
// write over them: Save all and build (play, import, rename) / Cancel.
void draw_unsaved_prompt(Workspace &workspace, const DialogsView::UnsavedPrompt &prompt) {
	if (prompt.open) ImGui::OpenPopup("Unsaved changes");
	if (!ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
	if (!prompt.open) { // answered another way (the editor MCP)
		ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		return;
	}
	const RequestKindRow &row = request_kind_row(prompt.action);
	// One sentence (the UX round's problems lane): what waits and on what. An action that reads the files as
	// saved (Build, Play, an import, a rename: its Save is "Save all and ...") needs them saved first; any
	// other only asks what to do with them.
	const std::string waiting = waiting_words(prompt.action, prompt.target);
	const bool saves_first = row.save_label && std::string(row.save_label).rfind("Save all and ", 0) == 0;
	const char *files = prompt.files.size() == 1 ? "this file" : "these files";
	const std::string sentence = waiting.empty() ? std::string(prompt.files.size() == 1 ? "This file has unsaved changes:"
	                                                                                    : "These files have unsaved changes:")
	                             : saves_first ? waiting + " needs " + files + " saved first:"
	                                           : waiting + ": " + files + (prompt.files.size() == 1 ? " has" : " have") +
	                                                     " unsaved changes:";
	ImGui::TextUnformatted(sentence.c_str());
	for (const std::string &file : prompt.files) ImGui::BulletText("%s", file.c_str());
	const bool one_file = row.guard == GuardScope::Document;
	const char *save = row.save_label ? row.save_label : "Save all";
	const auto answer = [&](UnsavedChoice choice) {
		workspace.request(request::resolve_unsaved(choice));
		ImGui::CloseCurrentPopup();
	};
	// An answer the session would refuse while an operation runs (busy_refuses_answer) is
	// disabled, the prompt kept: the running operation named, as the refusal names it.
	const OperationStatus &running = workspace.view().activity.operation;
	const std::string wait = running.running() ? std::string("Wait for ") + operation_kind_row(running.kind).noun +
	                                                     " to finish" + (running.cancellable ? ", or cancel it." : ".")
	                                           : std::string();
	const bool save_refused = busy_refuses_answer(prompt.action, UnsavedChoice::Save, running);
	if (enabled_button(save, !save_refused)) answer(UnsavedChoice::Save);
	if (save_refused) ui_kit::tooltip(wait);
	if (prompt.can_discard) {
		ImGui::SameLine();
		const bool discard_refused = busy_refuses_answer(prompt.action, UnsavedChoice::Discard, running);
		if (enabled_button(one_file ? "Don't save" : "Discard", !discard_refused)) answer(UnsavedChoice::Discard);
		if (discard_refused) ui_kit::tooltip(wait);
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
	layout.center_right = 1.0f - kDocumentShare;
	layout.focus = {"Document", "Problems"};
	return layout;
}

const DocumentBase *active_document(const SessionView &v) {
	for (const auto &document : v.documents.open)
		if (document->path() == v.documents.active) return document.get();
	return nullptr;
}

// A number of bytes as the bar's tooltip says it: "512 bytes", "3.4 KB", "12.0 MB".
std::string size_text(uint64_t bytes) {
	char text[32];
	if (bytes < 1024) std::snprintf(text, sizeof(text), "%llu bytes", static_cast<unsigned long long>(bytes));
	else if (bytes < (uint64_t(1) << 20)) std::snprintf(text, sizeof(text), "%.1f KB", double(bytes) / 1024.0);
	else std::snprintf(text, sizeof(text), "%.1f MB", double(bytes) / double(uint64_t(1) << 20));
	return text;
}

// The running operation on the bar ("Building 45%", "Refreshing 3/12") and in its tooltip (what
// it works on, how far it is).
std::string operation_text(const OperationStatus &operation) {
	const std::string verb = operation_kind_row(operation.kind).verb;
	if (operation.total == 0) return verb;
	if (operation.unit == OperationUnit::Bytes)
		return verb + " " + std::to_string(std::min<uint64_t>(operation.done * 100 / operation.total, 100)) + "%";
	return verb + " " + std::to_string(operation.done) + "/" + std::to_string(operation.total);
}

std::string operation_tip(const OperationStatus &operation) {
	std::string tip = operation.label;
	if (operation.total > 0 && operation.unit == OperationUnit::Bytes)
		tip += (tip.empty() ? "" : "\n") + size_text(operation.done) + " of " + size_text(operation.total);
	return tip;
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
	{
		auto inspector = std::make_unique<InspectorWindow>(*this);
		inspector_window_ = inspector.get();
		pass_.register_window(std::move(inspector));
	}
	{
		auto problems = std::make_unique<ProblemsWindow>(*this);
		problems_window_ = problems.get();
		pass_.register_window(std::move(problems));
	}
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

void EditorWindows::set_view(const SessionView *view) {
	if (view == view_) return;
	view_ = view;
	dispatched_ = this->view().events.next_seq() - 1;
}

// A request whose empty path names the active document (its row's names_active) names the one
// active when it was raised, so a Save that waits for the frame's other requests still saves it
// when a click in the same frame makes another document active first. One that acts on the files
// as saved (its row's acts_on_saved), raised while a frame draws, waits for the frame's other
// requests: the menu bar draws before the windows, so a Ctrl+S pressed as the inspector sets a
// field would otherwise save the document without it.
void EditorWindows::request(EditorRequest request) {
	const RequestKindRow &row = request_kind_row(request.kind);
	if (request.path.empty() && row.names_active) request.path = view().documents.active;
	if (in_frame_ && row.acts_on_saved) deferred_.push_back(std::move(request));
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
	dispatch_events();
}

// Each view event posted since the last frame to the window it is for, oldest first: the
// window holds it until it draws (a Files' RevealFile brings Files forward now, whether it draws
// this frame or not).
void EditorWindows::dispatch_events() {
	for (const ViewEvent &event : view().events.held()) {
		if (event.seq <= dispatched_) continue;
		dispatched_ = event.seq;
		switch (event.kind) {
		case ViewEventKind::RevealRecord:
			if (document_window_) document_window_->receive(event);
			if (inspector_window_) inspector_window_->receive(event);
			break;
		case ViewEventKind::RevealText:
			if (document_window_) document_window_->receive(event);
			break;
		case ViewEventKind::RevealFile:
			if (files_window_) files_window_->receive(event);
			break;
		case ViewEventKind::AskRename: rename_.receive(event); break;
		case ViewEventKind::SettingsApplied: settings_.receive(event); break;
		case ViewEventKind::ImportPlanned: import_.receive(event); break;
		// The build panel comes forward with what a build came to, unless the game follows it (a Play's
		// build that landed: the game is what the modder waits on).
		case ViewEventKind::BuildEnded:
			if (!(event.flag && event.tag == 1)) build_panel_open_ = true;
			break;
		case ViewEventKind::kCount: break;
		}
	}
}

void EditorWindows::end_frame() {
	// A viewport's canvas not drawn this frame (another shown, the window closed, collapsed or its
	// tab hidden): its gesture ends, before what waits on the files as saved.
	if (preview_window_) preview_window_->end_frame();
	if (document_window_) document_window_->end_frame();
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

void EditorWindows::deliver_pick(PickPurpose purpose, const std::string &path) {
	if (path.empty()) return; // cancelled
	switch (purpose) {
	case PickPurpose::NewProjectLocation: new_project_.set_folder(path); break;
	case PickPurpose::OpenProject: request(request::open_project(path)); break;
	case PickPurpose::RuntimeExecutable:
	case PickPurpose::GameInstall: settings_.set_picked(purpose, path, view().project.root); break;
	case PickPurpose::ImportFiles: deliver_picks(purpose, {path}); break;
	case PickPurpose::BuildFolder: {
		// The modder's pick, kept with the project's local settings (Build > Build to <it>), then built into.
		ProjectSettingsChange keep;
		keep.build_folder = path;
		request(request::apply_project_settings(keep));
		request(request::build(path));
		break;
	}
	case PickPurpose::None: break;
	}
}

void EditorWindows::deliver_picks(PickPurpose purpose, const std::vector<std::string> &paths) {
	if (paths.empty() || purpose != PickPurpose::ImportFiles) return; // cancelled, or not a file list
	// The editor's setting: with the files they need.
	request(request::preview_import(paths, view().project.import_dependencies));
}

void EditorWindows::draw_menu_bar(devtools::ImGuiPass &) {
	const SessionView &v = view();
	const DocumentBase *document = active_document(v);
	draw_file_menu(v);
	draw_edit_menu(v, document);
	draw_build_menu(v);
	// The modals, every frame, whichever window or menu opened them.
	draw_unsaved_prompt(*this, v.dialogs.unsaved_prompt);
	import_.draw(*this);
	settings_.draw(*this);
	draw_new_project();
	new_file_.draw(*this);
	find_.draw(*this);
	rename_.draw(*this);
	draw_build_panel(v);
	if (document_window_) document_window_->draw_modals();
	shortcuts(v, document);
}

void EditorWindows::draw_file_menu(const SessionView &v) {
	if (!ImGui::BeginMenu("File")) return;
	// A project switch and Quit, like every item that raises a request, are enabled while the
	// busy gate takes their request (an operation that cannot be cancelled refuses them).
	if (menu_item("New project...", nullptr, v.allows(EditorRequestKind::NewProject))) open_new_project_ = true;
	if (menu_item("Open project...", nullptr, v.allows(EditorRequestKind::OpenProject)))
		request(request::pick_directory(PickPurpose::OpenProject));
	if (ImGui::BeginMenu("Open recent", !v.project.recent_projects.empty())) {
		for (const std::string &root : v.project.recent_projects) {
			if (menu_item(root.c_str(), nullptr, v.allows(EditorRequestKind::OpenProject)))
				request(request::open_project(root));
		}
		ImGui::EndMenu();
	}
	ImGui::Separator();
	if (menu_item("Save", "Ctrl+S", !v.documents.active.empty() && v.allows(EditorRequestKind::Save)))
		request(request::save());
	if (menu_item("Save All", "Ctrl+Shift+S", !v.documents.open.empty() && v.allows(EditorRequestKind::SaveAll)))
		request(request::save_all());
	if (menu_item("Close file", "Ctrl+W", !v.documents.active.empty() && v.allows(EditorRequestKind::CloseDocument)))
		request(request::close_document());
	ImGui::Separator();
	const bool imports = v.project.open && v.allows(EditorRequestKind::PreviewImport);
	if (menu_item("Import files...", nullptr, imports)) request(request::pick_file(PickPurpose::ImportFiles));
	const bool lists = v.project.open && !v.project.retail_directory.empty() &&
	                   v.allows(EditorRequestKind::PreviewInstallImport);
	// A project that holds missions is offered the whole game install first (ADR 0046 S14: a
	// mission's closure is most of the game); any other the files it chooses, with what they need.
	const bool missions = v.project.open && v.project.document && v.project.document->features.mission;
	const auto whole_install = [&] {
		if (menu_item("Import the whole game install...", nullptr, lists)) request(request::import_whole_install());
		ui_kit::tooltip(v.project.open && v.project.retail_directory.empty()
		                        ? "Choose the game install folder in File > Project settings... first."
		                        : "Every file of the game install, copied into the project: what a mission project needs to "
		                          "play, build and resolve every name.");
	};
	if (missions) whole_install();
	if (menu_item("Import from the game data...", nullptr, lists))
		request(request::preview_install_import({}, v.project.import_dependencies));
	if (v.project.open && v.project.retail_directory.empty())
		ui_kit::tooltip("Choose the game install folder in File > Project settings... first.");
	if (!missions) whole_install();
	ImGui::Separator();
	if (menu_item("Project settings...", nullptr, v.project.open && v.allows(EditorRequestKind::ApplyProjectSettings)))
		settings_.open(v);
	if (menu_item("Show project folder", nullptr, v.project.open && v.allows(EditorRequestKind::RevealPath)))
		request(request::reveal_path(v.project.root));
	if (menu_item("Close project", nullptr, v.project.open && v.allows(EditorRequestKind::CloseProject)))
		request(request::close_project());
	ImGui::Separator();
	if (menu_item("Quit", nullptr, v.allows(EditorRequestKind::Quit))) request(request::quit());
	ImGui::EndMenu();
}

void EditorWindows::draw_edit_menu(const SessionView &v, const DocumentBase *document) {
	if (!ImGui::BeginMenu("Edit")) return;
	// Each names the step it would take (the UX round's problems lane): its words under it, muted.
	const auto step_words = [](const std::string &words) {
		if (words.empty()) return;
		ImGui::Indent();
		ImGui::TextDisabled("%s", ui_kit::fit(words, ImGui::GetFontSize() * 24.0f).c_str());
		ui_kit::tooltip(words);
		ImGui::Unindent();
	};
	if (menu_item("Undo", "Ctrl+Z", document && document->can_undo() && v.allows(EditorRequestKind::Undo)))
		request(request::undo());
	if (document) step_words(document->undo_words());
	if (menu_item("Redo", "Ctrl+Y", document && document->can_redo() && v.allows(EditorRequestKind::Redo)))
		request(request::redo());
	if (document) step_words(document->redo_words());
	// A rename is no step of a document's history: it rewrites files. Its way back is its true inverse (only
	// the sites it rewrote), shown before it commits, offered while its name is still where it put it.
	if (v.activity.last_rename.made && rename_back_offered(v)) {
		const ActivityView::LastRename &last = v.activity.last_rename;
		const std::string label = "Rename " + last.to + " back to " + last.from + "...";
		if (menu_item(label.c_str(), nullptr, v.allows(EditorRequestKind::PreviewRenameBack)))
			request(request::preview_rename_back(true));
		ui_kit::tooltip("Undo does not take a rename back: it rewrote files. This shows what renaming it back rewrites "
		                "(only what the rename wrote), then does it.");
	}
	ImGui::Separator();
	if (menu_item("Find...", "Ctrl+F", document != nullptr) && document_window_) document_window_->open_find();
	if (menu_item("Find in project...", "Ctrl+Shift+F", v.project.open && v.findings.graph))
		find_.open();
	ImGui::EndMenu();
}

void EditorWindows::draw_build_menu(const SessionView &v) {
	if (!ImGui::BeginMenu("Build")) return;
	if (menu_item("Build", "Ctrl+B", v.project.open && v.allows(EditorRequestKind::Build)))
		request(request::build());
	// A build for players, in a folder of the modder's choosing (the UX round's problems lane): the last
	// one again, or another.
	if (!v.project.build_folder.empty()) {
		// Its end tells two folders apart: the middle gives way.
		const std::string again = "Build to " + ui_kit::fit_middle(v.project.build_folder, ImGui::GetFontSize() * 20.0f);
		if (menu_item(again.c_str(), nullptr, v.project.open && v.allows(EditorRequestKind::Build)))
			request(request::build(v.project.build_folder));
		ui_kit::tooltip("Builds the game's files again into a new folder inside " + v.project.build_folder +
		                ", for players to copy into their game.");
	}
	if (menu_item("Build to folder...", nullptr, v.project.open && v.allows(EditorRequestKind::Build)))
		request(request::pick_directory(PickPurpose::BuildFolder));
	ui_kit::tooltip("Builds the game's files into a new folder inside a folder you choose, outside the project: what "
	                "players copy into their game to play the mod.");
	const bool plays = v.project.open && v.activity.play_state == PlayState::Stopped && v.allows(EditorRequestKind::Play);
	if (menu_item("Play", "F5", plays)) request(request::play());
	// S14: the game started in the active document's mission (its own file, or the mission the game
	// finds its file by: play_mission_for); F5 stays the game at its menu.
	const std::string mission = play_mission_for(v);
	if (menu_item("Play mission", "Ctrl+F5", plays && !mission.empty())) request(request::play(mission));
	ui_kit::tooltip(mission.empty() ? std::string("Open a mission, or a file the game finds by its name (its script, its text), to "
	                                              "start the game in it.")
	                                : "Build, then start the game in " + mission + ".");
	if (menu_item("Stop", "Shift+F5", v.activity.play_state == PlayState::Running && v.allows(EditorRequestKind::StopPlay)))
		request(request::stop_play());
	ImGui::Separator();
	// ADR 0046 S16: what ships, the build copied into the project's export folder.
	if (menu_item("Export", nullptr, v.project.open && v.allows(EditorRequestKind::Export)))
		request(request::export_project());
	ui_kit::tooltip("Build, then copy the build into the project's export folder as what ships.");
	if (menu_item("Show export folder", nullptr,
	              v.activity.has_export && v.activity.last_export->ok && v.allows(EditorRequestKind::RevealPath)))
		request(request::reveal_path(v.activity.last_export->export_dir));
	ImGui::Separator();
	// An editor setting, never refused (the busy gate takes it, as it takes the settings'
	// Apply); the next Play reads it, so it waits while a game runs.
	bool in_install = v.project.play_retail;
	const bool settable = v.activity.play_state == PlayState::Stopped && v.allows(EditorRequestKind::ApplyProjectSettings);
	if (ImGui::MenuItem("Play in the game install", nullptr, &in_install, settable) && settable) {
		ProjectSettingsChange change;
		change.play_in_install = in_install;
		request(request::apply_project_settings(change));
	}
	ui_kit::tooltip("Play starts the game install on the build instead of the OpenNova runtime.");
	if (menu_item("Show build folder", nullptr, v.activity.has_build && v.activity.last_build->ok && v.allows(EditorRequestKind::RevealPath)))
		request(request::reveal_path(v.activity.last_build->build_dir));
	ImGui::EndMenu();
}

// What the last build came to (the UX round's problems lane): a floating window that comes forward when a
// build ends (BuildEnded), its words the portable model's (session/build_result.h): how long, where, each
// file with its size, a folder build's line on how players install it; a refused build's refusals, a
// click from their rows in Problems; a failed one's why. Closed until the next build ends.
void EditorWindows::draw_build_panel(const SessionView &v) {
	if (!build_panel_open_ || !v.project.open || !v.activity.has_build) return;
	const BuildReport &report = *v.activity.last_build;
	const std::string own = v.project.root + "/.opennova/";
	const bool in_project = report.build_dir.rfind(own, 0) == 0;
	const BuildResult result = build_result(report, in_project);
	const ImGuiViewport *viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	// Kept in the editor's own window: it comes forward on its own, so it never pops out as a window
	// of its own over the desktop (a floating window that cannot merge into a minimized editor would).
	ImGui::SetNextWindowViewport(viewport->ID);
	if (!ImGui::Begin("Build result", &build_panel_open_,
	                  ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
	                          ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	ImGui::PushTextWrapPos(ImGui::GetFontSize() * 36.0f);
	const bool refused = result.outcome == BuildResult::Outcome::Refused;
	const bool failed = result.outcome == BuildResult::Outcome::Failed;
	if (refused || failed) {
		ui_kit::severity_marker(DiagnosticSeverity::Error);
		ImGui::SameLine();
	}
	ImGui::TextWrapped("%s", result.headline.c_str());
	for (const std::string &refusal : result.refusals) ImGui::BulletText("%s", refusal.c_str());
	if (!result.failure.empty()) ImGui::TextWrapped("%s", result.failure.c_str());
	if (!result.where.empty()) {
		ImGui::Spacing();
		ImGui::TextDisabled("In");
		ImGui::SameLine();
		ImGui::TextWrapped("%s", result.where.c_str());
	}
	if (!result.files.empty() && ImGui::BeginTable("built", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
		for (const BuildResult::File &file : result.files) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(file.name.c_str());
			ImGui::TableNextColumn();
			ImGui::TextDisabled("%s", file.words.c_str());
		}
		ImGui::EndTable();
	}
	if (!result.players.empty()) {
		ImGui::Spacing();
		ImGui::TextWrapped("%s", result.players.c_str());
	}
	if (!result.others.empty()) {
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		ImGui::TextWrapped("%s", result.others.c_str());
		ImGui::PopStyleColor();
	}
	ImGui::PopTextWrapPos();
	ImGui::Spacing();
	if (refused && problems_window_ && ImGui::Button("Show them in Problems")) {
		problems_window_->show_blocking();
		build_panel_open_ = false;
	}
	if (!result.where.empty()) {
		if (enabled_button("Show folder", v.allows(EditorRequestKind::RevealPath))) request(request::reveal_path(report.build_dir));
		ImGui::SameLine();
		if (enabled_button("Play", v.activity.play_state == PlayState::Stopped && v.allows(EditorRequestKind::Play))) {
			request(request::play());
			build_panel_open_ = false;
		}
		ImGui::SameLine();
		if (enabled_button("Build to folder...", v.allows(EditorRequestKind::Build)))
			request(request::pick_directory(PickPurpose::BuildFolder));
		ImGui::SameLine();
	}
	if (ImGui::Button("Close")) build_panel_open_ = false;
	ImGui::End();
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
void EditorWindows::shortcuts(const SessionView &v, const DocumentBase *document) {
	if (v.dialogs.unsaved_prompt.open) return;
	const ImGuiIO &io = ImGui::GetIO();
	if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
		if (io.KeyShift && !v.documents.open.empty() && v.allows(EditorRequestKind::SaveAll))
			request(request::save_all());
		else if (!io.KeyShift && !v.documents.active.empty() && v.allows(EditorRequestKind::Save))
			request(request::save());
	}
	if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_W, false) && !v.documents.active.empty() &&
	    v.allows(EditorRequestKind::CloseDocument))
		request(request::close_document());
	if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_F, false) && v.project.open && v.findings.graph) find_.open();
	if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_B, false) && v.project.open && v.allows(EditorRequestKind::Build)) {
		request(request::build());
	}
	if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) {
		const bool plays = v.project.open && v.activity.play_state == PlayState::Stopped && v.allows(EditorRequestKind::Play);
		if (io.KeyShift && v.activity.play_state == PlayState::Running && v.allows(EditorRequestKind::StopPlay)) {
			request(request::stop_play());
		} else if (io.KeyCtrl && !io.KeyShift && plays) {
			// Play mission: the active document's mission, nothing where it has none.
			const std::string mission = play_mission_for(v);
			if (!mission.empty()) request(request::play(mission));
		} else if (!io.KeyShift && !io.KeyCtrl && plays) {
			request(request::play());
		}
	}
	if (!io.WantTextInput && document) {
		const EditorRequestKind z = io.KeyShift ? EditorRequestKind::Redo : EditorRequestKind::Undo;
		if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false) && v.allows(z)) request(request::of(z));
		if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false) && v.allows(EditorRequestKind::Redo))
			request(request::redo());
	}
}

// The bar's right end, right-aligned: what the session last said (cut to the room left),
// the files with unsaved changes (a click lists them: a click on one makes it the active
// document; Save all), the errors and warnings (a click shows Problems), the running
// operation with its Cancel ("Building 45%"), else the game or the last build ("Game
// running", "Built"), then Build, Play and Stop, each disabled while the busy gate would
// refuse it. A bar too narrow for all of it leaves parts out from the left rather than run
// over the menus.
void EditorWindows::draw_menu_bar_trailing(devtools::ImGuiPass &) {
	const SessionView &v = view();
	if (!v.project.open) return;
	std::vector<const DocumentBase *> unsaved;
	for (const auto &document : v.documents.open)
		if (document->dirty()) unsaved.push_back(document.get());
	// The modder's findings: those about the game's own data (S15) are counted apart, in the tooltip, as
	// Problems and the view's problem_counts count them.
	const ProblemCounts counted = count_problems(v);
	const size_t errors = counted.errors, warnings = counted.warnings, infos = counted.infos;
	const size_t original = counted.original_errors + counted.original_warnings + counted.original_infos;
	const std::string unsaved_text = std::to_string(unsaved.size()) + " unsaved";
	std::string state, state_tip;
	const bool cancel = v.activity.operation.running() && v.activity.operation.cancellable;
	if (v.activity.operation.running()) {
		state = operation_text(v.activity.operation);
		state_tip = operation_tip(v.activity.operation);
	} else if (v.activity.validation.running) {
		// The validation the polls step (the first one of a large project above all).
		const ValidationStatus &validation = v.activity.validation;
		state = "Validating" + (validation.total ? " " + std::to_string(validation.done) + "/" +
				std::to_string(validation.total) : std::string());
		state_tip = "Problems lists what was found before until it ends.";
	} else if (v.activity.play_state == PlayState::Running) {
		state = v.project.play_retail ? "Game install running" : "Game running";
		state_tip = "Process " + std::to_string(v.activity.play_pid) + ". Stop ends it.";
	} else if (v.activity.play_state != PlayState::Stopped) {
		state = "Stopping the game";
	} else if (v.activity.has_build) {
		const BuildReport &last = *v.activity.last_build;
		state = last.ok ? "Built" : last.refused ? "Build refused" : "Build failed";
		// What refused it, from its own report (the status line has moved on since).
		state_tip = last.ok ? "The last build: " + last.build_dir
		            : last.refused ? refused_words(last) + "\nA click shows what refuses it in Problems."
		                           : "See Problems.";
	}

	// Each part's width, left to right: the unsaved files, the problem counts, the state and
	// the buttons (the bar's layout puts an item spacing between items), from the end of the
	// menus to the end of the bar's content.
	const float gap = ImGui::GetStyle().ItemSpacing.x;
	float widths[4] = {};
	if (!unsaved.empty()) widths[0] = ui_kit::unsaved_dot_width() + gap + ui_kit::text_width(unsaved_text.c_str());
	widths[1] = ui_kit::severity_count_width(errors) + gap + ui_kit::severity_count_width(warnings);
	if (!state.empty()) widths[2] = ui_kit::text_width(state.c_str());
	if (cancel) widths[2] += gap + ui_kit::button_width("Cancel");
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
	const std::string status = first == 0 && !v.activity.status.empty() && room >= ImGui::GetFontSize() * 4.0f
	                                   ? ui_kit::fit(v.activity.status, room)
	                                   : std::string();
	const float status_width = status.empty() ? 0.0f : ui_kit::text_width(status.c_str()) + gap;
	ImGui::SetCursorPosX(right - total - status_width);
	ImGui::PushID("status");
	if (!status.empty()) {
		ImGui::TextDisabled("%s", status.c_str());
		ui_kit::tooltip(v.activity.status + "\n" + v.project.root);
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
			for (const DocumentBase *document : unsaved)
				if (menu_item(document->path().c_str(), nullptr, v.allows(EditorRequestKind::OpenDocument)))
					request(request::open_document(document->path()));
			ImGui::Separator();
			if (menu_item("Save all", nullptr, v.allows(EditorRequestKind::SaveAll)))
				request(request::save_all());
			ImGui::EndPopup();
		}
	}
	if (first <= 1) {
		std::string tip = std::to_string(errors) + (errors == 1 ? " error, " : " errors, ") + std::to_string(warnings) +
		                  (warnings == 1 ? " warning, " : " warnings, ") + std::to_string(infos) + " info.";
		if (original) tip += " " + std::to_string(original) + " more in the game's own data (also in the original).";
		if (counted.blocking)
			tip += " " + std::to_string(counted.blocking) + (counted.blocking == 1 ? " blocks" : " block") + " the build.";
		if (const int missing = v.project.requirements->required_missing + v.project.requirements->required_wrong_kind)
			tip += " " + std::to_string(missing) + " of " + std::to_string(v.project.requirements->required_total) +
			       " required files " + (missing == 1 ? "is" : "are") + " missing.";
		if (clickable("##problems", widths[1], tip + " A click shows Problems.") && problems_window_)
			problems_window_->request_focus();
		ui_kit::severity_count(DiagnosticSeverity::Error, errors, height);
		ui_kit::severity_count(DiagnosticSeverity::Warning, warnings, height);
	}
	if (first <= 2 && !state.empty()) {
		// The last build's state: a click shows what it came to (the build panel); a refused build's, Problems
		// with only what refuses it.
		const bool built = !v.activity.operation.running() && !v.activity.validation.running &&
		                   v.activity.play_state == PlayState::Stopped && v.activity.has_build;
		const bool refused = built && v.activity.last_build->refused;
		if (built) {
			if (clickable("##built", ui_kit::text_width(state.c_str()),
			              state_tip + (refused ? std::string() : std::string("\nA click shows what it built.")))) {
				if (refused && problems_window_) problems_window_->show_blocking();
				else build_panel_open_ = true;
			}
			if (refused) ImGui::PushStyleColor(ImGuiCol_Text, ui_kit::severity_color(DiagnosticSeverity::Error));
		}
		ImGui::TextUnformatted(state.c_str());
		if (refused) ImGui::PopStyleColor();
		if (!built) ui_kit::tooltip(state_tip);
		if (cancel) {
			if (enabled_button("Cancel", v.allows(EditorRequestKind::CancelOperation)))
				request(request::cancel_operation());
			ui_kit::tooltip(std::string("Stops ") + operation_kind_row(v.activity.operation.kind).noun + ": nothing it made is kept.");
		}
	}
	if (enabled_button("Build", v.allows(EditorRequestKind::Build))) request(request::build());
	ui_kit::tooltip("Build the project (Ctrl+B).");
	if (enabled_button("Play", v.activity.play_state == PlayState::Stopped && v.allows(EditorRequestKind::Play)))
		request(request::play());
	ui_kit::tooltip("Build, then run the game on the build (F5).");
	if (enabled_button("Stop", v.activity.play_state == PlayState::Running && v.allows(EditorRequestKind::StopPlay)))
		request(request::stop_play());
	ui_kit::tooltip("Stop the game (Shift+F5).");
	ImGui::PopID();
}

} // namespace opennova::editor
