#include <editor/ui/editor_windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>

#include <base/io/json.h>
#include <editor/graph/jump_queries.h>
#include <editor/project/project_files.h>
#include <editor/project_build/build_run.h>
#include <editor/session/build_result.h>
#include <editor/session/navigation_controller.h>
#include <editor/session/rename_controller.h>
#include <editor/project_build/export_build.h>
#include <editor/session/play_controller.h>
#include <editor/session/problem_query.h>
#include <editor/session/request_factories.h>
#include <editor/session/request_kinds.h>
#include <editor/session/view/session_view.h>
#include <editor/session/workspace_parts.h>
#include <editor/ui/document_window.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/inspector_window.h>
#include <editor/ui/output_window.h>
#include <editor/ui/preview_window.h>
#include <editor/ui/problems_window.h>
#include <editor/ui/ui_kit.h>

#include <imgui.h>
#include <imgui_internal.h>

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
	// The first of the dialogs that take the whole editor (shown_modal): it shows whenever it is asked.
	const bool shows = prompt.open && modal_may_show(workspace.view(), HeldModal::Unsaved);
	if (shows && !ImGui::IsPopupOpen("Unsaved changes")) ImGui::OpenPopup("Unsaved changes");
	if (!ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
	if (!shows) { // answered another way (the editor MCP)
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
		auto files = std::make_unique<FilesWindow>(*this);
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
	// A texture's Replace with image... (S18): the texture it is for kept until the Shell's pick comes back.
	if (request.kind == EditorRequestKind::PickFile && request.purpose == PickPurpose::TextureImage) {
		replace_target_ = request.path;
		request.path.clear();
	}
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
	pointer_hidden_ = false;
	// What the windows offered F12 and Shift+F12 last frame is what the keys act on this one (the menu bar's
	// shortcuts read it before the windows draw).
	subject_ = std::move(offered_);
	offered_ = JumpSubject();
	dispatch_events();
}

void EditorWindows::offer_jump(const JumpSubject &subject) {
	if (!subject.any() || (offered_.pointer && !subject.pointer)) return;
	offered_ = subject;
}

JumpSubject EditorWindows::jump_subject() const {
	// Each key takes what was offered where the offer has it (a Problems row offers its uses alone), else the
	// selection's.
	if (!subject_.definition.empty() && !subject_.usages_file.empty()) return subject_;
	const SessionView &v = view();
	JumpSubject out;
	const DocumentBase *base = active_document(v);
	if (base) {
		out.usages_file = base->path();
		const Document *document = records_of(*base);
		const NodeAddress &record = v.documents.selection.primary;
		if (document && record.row && v.documents.selection.document == base->path()) {
			out.usages_locator = document->locator(record);
			if (v.findings.graph && v.project.scan)
				record_definition(*v.findings.graph, *v.project.scan, *document, record, out.definition);
		}
	}
	if (!subject_.definition.empty()) out.definition = subject_.definition;
	if (!subject_.usages_file.empty()) {
		out.usages_file = subject_.usages_file;
		out.usages_locator = subject_.usages_locator;
	}
	out.pointer = subject_.pointer;
	return out;
}

bool EditorWindows::go_to_definition(const JumpSubject &subject) {
	if (subject.definition.empty() || !view().allows(EditorRequestKind::OpenDocument)) return false;
	window_requests::go_to(*this, subject.definition.front());
	return true;
}

bool EditorWindows::find_usages(const SessionView &v, const JumpSubject &subject) {
	if (subject.usages_file.empty() || !v.project.open || !v.findings.graph) return false;
	ProjectFind::open_usages(*this, subject.usages_file, subject.usages_locator);
	return true;
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
		// Rename everywhere and Rename back open over the workspace's (the session opens them with the ask).
		case ViewEventKind::AskRename: break;
		case ViewEventKind::SettingsApplied: settings_.receive(event); break;
		// The import dialog takes a plan's checks from the workspace (the session takes them anew).
		case ViewEventKind::ImportPlanned: break;
		// The build panel's opening is the workspace's (the session opens it as a build ends).
		case ViewEventKind::BuildEnded: break;
		case ViewEventKind::OpenExternally: break; // the Shell's (EditorApp opens the file)
		// A use shown on its model or in its menu (S18): the Preview window that draws it comes forward.
		case ViewEventKind::RevealPreview:
			if (preview_window_) preview_window_->request_focus();
			break;
		// A Back or a Forward to a document or a page: the Document window comes forward with its tab.
		case ViewEventKind::ShowDocument:
			if (document_window_) document_window_->show_document(event);
			break;
		// A set_workspace's focus (the MCP gaps lane): the window it names comes forward, opened when the person
		// had closed it (the Windows menu's).
		case ViewEventKind::FocusWindow:
			for (size_t i = 0; const char *token = workspace_window_token(i); ++i) {
				if (event.path != token) continue;
				for (int w = 0; w < pass_.window_count(); ++w)
					if (std::strcmp(pass_.window(w).title(), workspace_window_title(i)) == 0) {
						pass_.window(w).open = true;
						pass_.window(w).request_focus();
					}
			}
			break;
		case ViewEventKind::kCount: break;
		}
	}
}

void EditorWindows::drop_files(std::vector<std::string> paths, float x, float y) {
	dropped_ = {std::move(paths), x, y, 2};
}

bool EditorWindows::take_dropped_files(float min_x, float min_y, float max_x, float max_y, std::vector<std::string> &paths) {
	if (dropped_.paths.empty() || dropped_.x < min_x || dropped_.x >= max_x || dropped_.y < min_y || dropped_.y >= max_y)
		return false;
	// The window under the drop is the item's own (or a child of it), never one drawn over it; with a modal
	// open, only the modal's items take one.
	const ImGuiWindow *current = ImGui::GetCurrentWindowRead();
	ImGuiWindow *under = nullptr, *under_moving = nullptr;
	ImGui::FindHoveredWindowEx(ImVec2(dropped_.x, dropped_.y), true, &under, &under_moving);
	if (!current || !under || under->RootWindow != current->RootWindow) return false;
	if (const ImGuiWindow *modal = ImGui::GetTopMostAndVisiblePopupModal())
		if (modal->RootWindow != current->RootWindow) return false;
	paths = std::move(dropped_.paths);
	dropped_ = Dropped();
	return true;
}

void EditorWindows::end_frame() {
	// A viewport's canvas not drawn this frame (another shown, the window closed, collapsed or its
	// tab hidden): its gesture ends, before what waits on the files as saved.
	if (preview_window_) preview_window_->end_frame();
	if (document_window_) document_window_->end_frame();
	// A drop no item took within two frames is none's (the frame it came in may have drawn before it).
	if (!dropped_.paths.empty() && --dropped_.frames <= 0) dropped_ = Dropped();
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

std::vector<EditorRequest> EditorWindows::take_requests_of(EditorRequestKind kind) {
	std::vector<EditorRequest> out;
	for (auto request = requests_.begin(); request != requests_.end();) {
		if (request->kind != kind) {
			++request;
			continue;
		}
		out.push_back(std::move(*request));
		request = requests_.erase(request);
	}
	return out;
}

void EditorWindows::deliver_pick(PickPurpose purpose, const std::string &path) {
	if (path.empty()) return; // cancelled
	switch (purpose) {
	// The new-project form's fields are the workspace's.
	case PickPurpose::NewProjectLocation:
		window_requests::set_workspace(*this, "new_project", "dir", io::JsonValue::make_string(path));
		break;
	case PickPurpose::NewProjectInstall:
		window_requests::set_workspace(*this, "new_project", "game_install", io::JsonValue::make_string(path));
		break;
	case PickPurpose::OpenProject: request(request::open_project(path)); break;
	case PickPurpose::RuntimeExecutable:
	case PickPurpose::GameInstall: settings_.set_picked(*this, purpose, path, view().project.root); break;
	case PickPurpose::ImportFiles: deliver_picks(purpose, {path}); break;
	case PickPurpose::TextureImage:
		// Asked before it is done: the dialog shows what the Replace would change (S18).
		if (!replace_target_.empty()) request(request::preview_texture_source(replace_target_, path));
		replace_target_.clear();
		break;
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
	draw_navigation(v);
	draw_file_menu(v);
	draw_edit_menu(v, document);
	draw_go_menu(v);
	draw_build_menu(v);
	// The modals, every frame, whichever window or menu opened them: the one the session's order shows
	// (shown_modal) opens, the others held wait closed.
	bring_modal_forward(v);
	draw_unsaved_prompt(*this, v.dialogs.unsaved_prompt);
	import_.draw(*this);
	settings_.draw(*this);
	draw_new_project();
	new_file_.draw(*this);
	find_.draw(*this);
	rename_.draw(*this);
	texture_source_.draw(*this);
	draw_build_panel(v);
	if (files_window_) files_window_->draw_card_window();
	if (document_window_) document_window_->draw_modals();
	shortcuts(v, document);
}

// Back and Forward (the navigation history, session/navigation_controller.h), before the menus as a
// browser's are: each enabled while it goes somewhere (navigation_offered), its tooltip naming where and
// its keys, a right-click listing its places nearest first, a click on one going there (navigate_back's
// steps). Alt+Left and Alt+Right and the mouse's back and forward buttons are the same requests
// (shortcuts()).
void EditorWindows::draw_navigation(const SessionView &v) {
	for (const bool back : {true, false}) {
		const std::vector<NavigationPlace> &places = back ? v.navigation.back : v.navigation.forward;
		const bool offered = navigation_offered(v, back);
		const auto go = [&](size_t steps) {
			request(back ? request::navigate_back(uint32_t(steps)) : request::navigate_forward(uint32_t(steps)));
		};
		ImGui::PushID(back ? "navigate_back" : "navigate_forward");
		ImGui::BeginDisabled(!offered);
		const bool pressed = ImGui::ArrowButton("##go", back ? ImGuiDir_Left : ImGuiDir_Right);
		ImGui::EndDisabled();
		if (pressed && offered) go(1);
		if (offered && ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("places");
		const std::string keys = back ? " (Alt+Left, or the mouse's back button)" : " (Alt+Right, or the mouse's forward button)";
		ui_kit::tooltip(places.empty() ? std::string(back ? "Back" : "Forward") + keys +
		                                         ": nowhere yet. A Go to, a Problems row, a find's hit or another file opened "
		                                         "is a place it takes you back to."
		                               : std::string(back ? "Back to " : "Forward to ") + places.front().label + keys +
		                                         ".\nA right-click lists every place " + (back ? "back." : "forward."));
		if (ImGui::BeginPopup("places")) {
			for (size_t i = 0; i < places.size(); ++i) {
				const std::string label = ui_kit::fit(places[i].label, ImGui::GetFontSize() * 24.0f) + "###" + std::to_string(i);
				if (menu_item(label.c_str(), nullptr, navigation_offered(v, back))) go(i + 1);
				ui_kit::tooltip(places[i].path);
			}
			ImGui::EndPopup();
		}
		ImGui::PopID();
	}
}

void EditorWindows::draw_file_menu(const SessionView &v) {
	if (!ImGui::BeginMenu("File")) return;
	// A project switch and Quit, like every item that raises a request, are enabled while the
	// busy gate takes their request (an operation that cannot be cancelled refuses them).
	if (menu_item("New project...", nullptr, v.allows(EditorRequestKind::NewProject)))
		window_requests::set_workspace(*this, "new_project", "open", io::JsonValue::make_bool(true));
	if (menu_item("Open project...", nullptr, v.allows(EditorRequestKind::OpenProject)))
		request(request::pick_directory(PickPurpose::OpenProject));
	if (ImGui::BeginMenu("Open recent", !v.project.recent_projects.empty())) {
		// Each by its title, its folder beside it, muted (the UX round's project lane).
		for (size_t i = 0; i < v.project.recent_projects.size(); ++i) {
			const std::string &root = v.project.recent_projects[i];
			const ProjectView::RecentProject *details =
					i < v.project.recent_details.size() && v.project.recent_details[i].root == root ? &v.project.recent_details[i]
					                                                                               : nullptr;
			const bool found = !details || details->found;
			const std::string title = details && !details->title.empty() ? details->title : root;
			const std::string where = ui_kit::fit_middle(root, ImGui::GetFontSize() * 22.0f);
			// Known by its folder ("###<root>"), whatever its title says.
			if (menu_item((title + "###" + root).c_str(), title != root ? where.c_str() : nullptr,
			              found && v.allows(EditorRequestKind::OpenProject)))
				request(request::open_project(root));
			ui_kit::tooltip(found ? root : root + " holds no project now.");
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
		window_requests::set_workspace(*this, "settings", "open", io::JsonValue::make_bool(true));
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
		// A move's (DI-03): the file to the folder it left.
		const std::string label = last.move ? "Move " + basename_of(last.path) + " back to " +
		                                              (last.from.empty() ? std::string("the top level") : last.from + "/") + "..."
		                                    : "Rename " + last.to + " back to " + last.from + "...";
		if (menu_item(label.c_str(), nullptr, v.allows(EditorRequestKind::PreviewRenameBack)))
			request(request::preview_rename_back(true));
		ui_kit::tooltip("Undo does not take a rename back: it rewrote files. This shows what renaming it back rewrites "
		                "(only what the rename wrote), then does it.");
	}
	ImGui::Separator();
	if (menu_item("Find...", "Ctrl+F", document != nullptr) && document_window_) document_window_->open_find();
	if (menu_item("Find in project...", "Ctrl+Shift+F", v.project.open && v.findings.graph))
		ProjectFind::open(*this);
	ImGui::EndMenu();
}

// Go (DI-18): the jumps, each with its keys. Back and Forward as the arrows go; Go to file, Go to name and Find in
// project open the project's finder in that scope; Go to definition and Find usages act on what jump_subject says
// (from the menu, the selection: the pointer is on the menu), each naming it in its tooltip.
void EditorWindows::draw_go_menu(const SessionView &v) {
	if (!ImGui::BeginMenu("Go")) return;
	if (menu_item("Back", "Alt+Left", navigation_offered(v, true))) request(request::navigate_back());
	ui_kit::tooltip(v.navigation.back.empty() ? std::string("Nowhere yet.") : "Back to " + v.navigation.back.front().label + ".");
	if (menu_item("Forward", "Alt+Right", navigation_offered(v, false))) request(request::navigate_forward());
	ui_kit::tooltip(v.navigation.forward.empty() ? std::string("Nowhere yet.")
	                                             : "Forward to " + v.navigation.forward.front().label + ".");
	ImGui::Separator();
	const bool finds = v.project.open && v.findings.graph;
	if (menu_item("Go to file...", "Ctrl+P", finds)) ProjectFind::open(*this, ProjectFind::Scope::Files);
	ui_kit::tooltip("Open a file of the project by its name: type part of it, Enter opens the first.");
	if (menu_item("Go to name...", "Ctrl+T", finds)) ProjectFind::open(*this, ProjectFind::Scope::Names);
	ui_kit::tooltip("Go to a record of any file by the name it defines: an item, a weapon, a string, a screen, a "
	                "style variable.");
	if (menu_item("Find in project...", "Ctrl+Shift+F", finds)) ProjectFind::open(*this);
	ui_kit::tooltip("Every file and name the project's files define whose name holds the text, each with its uses.");
	ImGui::Separator();
	const JumpSubject subject = jump_subject();
	if (menu_item("Go to definition", "F12", !subject.definition.empty() && v.allows(EditorRequestKind::OpenDocument)))
		go_to_definition(subject);
	ui_kit::tooltip(subject.definition.empty()
	                        ? std::string("What a reference names: F12 with the pointer on a reference field (or Ctrl+click "
	                                      "its value), or with a record selected that names something (a mission's "
	                                      "entity: its item).")
	                        : "Go to " + subject.definition.front().label + " (F12; Ctrl+click a reference's value).");
	const bool usages = finds && !subject.usages_file.empty();
	if (menu_item("Find usages", "Shift+F12", usages)) find_usages(v, subject);
	ui_kit::tooltip_lazy([&] {
		return usages ? "Who names " + usages_subject_words(*v.findings.graph, subject.usages_file, subject.usages_locator) +
		                        ", each a Go to (Shift+F12)."
		              : std::string("Who names a file or what a record defines: open a file or select a record.");
	});
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
	// DI-26: the active mission's view plans it, its player on the ground under the camera.
	const bool from_here = plays && !play_mission_at(v, v.documents.active).empty();
	if (menu_item("Play from here", "Alt+F5", from_here)) request(request::play_from_here(v.documents.active));
	ui_kit::tooltip(from_here ? std::string("Build, then start the game in this mission with the player on the ground under "
	                                        "the mission view's camera, facing the way it looks. The build's copy of the "
	                                        "mission gets the start; the mission's own file is left as it is.")
	                          : std::string("Open a mission to start the game in it where its view's camera is."));
	// A first run: the run directory emptied of what the runs before wrote there (run/run_directory.h), which
	// Play keeps otherwise.
	if (menu_item("Play fresh", nullptr, plays)) request(request::play(std::string(), false, true));
	ui_kit::tooltip("Build, then start the game in an emptied run folder, as on its first run: without the game.cfg, "
	                "saves and scores earlier runs wrote there (the game install asks for the display adapter again). "
	                "Play keeps them.");
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
	// How this project plays (its own, kept in its .opennova/local.json: never another project's, nor another
	// editor's), never refused (the busy gate takes it, as it takes the settings' Apply); the next Play reads
	// it, so it waits while a game runs.
	const PlayMode mode = v.project.play_mode;
	bool in_install = plays_in_install(mode);
	const bool settable = v.project.open && v.activity.play_state == PlayState::Stopped &&
	                      v.allows(EditorRequestKind::ApplyProjectSettings);
	if (ImGui::MenuItem("Play in the game install", nullptr, &in_install, settable) && settable) {
		ProjectSettingsChange change;
		change.play_mode = in_install ? PlayMode::Install : PlayMode::Runtime;
		request(request::apply_project_settings(change));
	}
	ui_kit::tooltip("This project's Play starts the game install on the build instead of the OpenNova runtime. Kept "
	                "with the project on this computer; another project plays as it is set.");
	// Strict Play, under it: what Play in the game install stages and how it launches the game.
	bool strict = mode == PlayMode::Strict;
	ImGui::Indent();
	const bool strict_settable = settable && in_install;
	if (ImGui::MenuItem("Strict: as a player's install", nullptr, &strict, strict_settable) && strict_settable) {
		ProjectSettingsChange change;
		change.play_mode = strict ? PlayMode::Strict : PlayMode::Install;
		request(request::apply_project_settings(change));
	}
	ImGui::Unindent();
	ui_kit::tooltip("The game runs on the build alone, as a player who dropped Jointops.exe into the build's folder "
	                "runs it: nothing of the install but its program and Bink DLL, no /d.");
	// DI-26: Play saves first instead of asking; the project's own, as the two above.
	bool save_first = v.project.save_before_play;
	const bool save_settable = v.project.open && v.allows(EditorRequestKind::ApplyProjectSettings);
	if (ImGui::MenuItem("Save all before Play", nullptr, &save_first, save_settable) && save_settable) {
		ProjectSettingsChange change;
		change.save_before_play = save_first;
		request(request::apply_project_settings(change));
	}
	ui_kit::tooltip("Play writes every file with unsaved edits first, as Save all does, and starts at once. Off, Play "
	                "asks first, as Build does.");
	if (menu_item("Show build folder", nullptr, v.activity.has_build && v.activity.last_build->ok && v.allows(EditorRequestKind::RevealPath)))
		request(request::reveal_path(v.activity.last_build->build_dir));
	ImGui::EndMenu();
}

// What the last build came to (the UX round's problems lane): a floating window that comes forward when a
// build ends (the workspace's build_result, which the session opens then), its words the portable model's
// (session/build_result.h): how long, where, each file with its size, a folder build's line on how players
// install it; a refused build's refusals, a click from their rows in Problems; a failed one's why. Closed (a
// set_workspace) until the next build ends.
void EditorWindows::draw_build_panel(const SessionView &v) {
	if (!v.workspace.build_result.open || !v.project.open || !v.activity.has_build) return;
	const auto close = [this] { window_requests::set_workspace(*this, "build_result", "open", io::JsonValue::make_bool(false)); };
	const BuildReport &report = *v.activity.last_build;
	const std::string own = v.project.root + "/.opennova/";
	const bool in_project = report.build_dir.rfind(own, 0) == 0;
	const BuildResult result = build_result(report, in_project);
	const ImGuiViewport *viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	// Kept in the editor's own window: it comes forward on its own, so it never pops out as a window
	// of its own over the desktop (a floating window that cannot merge into a minimized editor would).
	ImGui::SetNextWindowViewport(viewport->ID);
	bool open = true;
	if (!ImGui::Begin("Build result", &open,
	                  ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
	                          ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		if (!open) close();
		return;
	}
	if (!open) close();
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
		close();
	}
	if (!result.where.empty()) {
		if (enabled_button("Show folder", v.allows(EditorRequestKind::RevealPath))) request(request::reveal_path(report.build_dir));
		ImGui::SameLine();
		if (enabled_button("Play", v.activity.play_state == PlayState::Stopped && v.allows(EditorRequestKind::Play))) {
			request(request::play());
			close();
		}
		ImGui::SameLine();
		if (enabled_button("Build to folder...", v.allows(EditorRequestKind::Build)))
			request(request::pick_directory(PickPurpose::BuildFolder));
		ImGui::SameLine();
	}
	if (ImGui::Button("Close")) close();
	ImGui::End();
}

void EditorWindows::bring_modal_forward(const SessionView &v) {
	const HeldModal shown = shown_modal(v).modal;
	if (int(shown) == brought_) return;
	brought_ = int(shown);
	// One that shows already (a person's own ask, drawn at once) is where the person is: focusing its window again
	// would close it.
	devtools::Window *owner = nullptr;
	if (shown == HeldModal::FileRename && files_window_ && !files_window_->rename_shown()) owner = files_window_;
	if (shown == HeldModal::Confirm && problems_window_ && !problems_window_->confirm_shown()) owner = problems_window_;
	if (!owner) return;
	owner->open = true;
	owner->request_focus();
}

// File > New project...: the welcome view's form in a modal, open while the workspace's new_project says
// and no dialog before it in the session's order is held (shown_modal: it waits, held, while the unsaved
// prompt or another shows); the session closes it as it takes the new_project its Create raises; Cancel
// closes it.
void EditorWindows::draw_new_project() {
	const SessionView &v = view();
	const bool wanted = v.workspace.new_project.open && modal_may_show(v, HeldModal::NewProject);
	if (wanted && !ImGui::IsPopupOpen("New project")) ImGui::OpenPopup("New project");
	if (!ImGui::BeginPopupModal("New project", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
	if (!wanted) { // closed another way (made, the editor MCP), or another dialog shows in its place
		ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		return;
	}
	new_project_.draw(*this);
	ImGui::SameLine();
	if (ImGui::Button("Cancel")) window_requests::set_workspace(*this, "new_project", "open", io::JsonValue::make_bool(false));
	// Why the last New project was refused, here as on the welcome page.
	if (!v.project.refused.empty()) {
		ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
		ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1.0f), "%s", v.project.refused.c_str());
		ImGui::PopTextWrapPos();
	}
	ImGui::EndPopup();
}

// The shortcuts the menu labels promise (Ctrl+F is the Document window's own, before its views'
// filters), and Back's and Forward's. Saving, closing a file, finding in the project, going to a file or a
// name, going to a definition, finding usages, building and
// playing work while a text field has the keyboard (what it typed this frame is raised first:
// request()); Undo and Redo, and Alt+Left and Alt+Right, are the field's own then. Ctrl+Shift+Z is
// Redo, as Ctrl+Y is. None of them while the unsaved prompt is open: an Undo behind it would make a
// file it does not list unsaved. No two share keys: Ctrl+S, Ctrl+Shift+S, Ctrl+W, Ctrl+B, Ctrl+P, Ctrl+T,
// Ctrl+Shift+F, Ctrl+Z, Ctrl+Shift+Z, Ctrl+Y, F5, Ctrl+F5, Alt+F5, Shift+F5, F12, Shift+F12, Alt+Left and
// Alt+Right here; Ctrl+F, Ctrl+C, Ctrl+X, Ctrl+V, Ctrl+D, F2, Delete, F and the arrows the windows' own.
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
	if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_F, false) && v.project.open && v.findings.graph)
		ProjectFind::open(*this);
	// The jumps (DI-18): Go to file and Go to name, wherever the keyboard is (another of the finder's scopes while it
	// shows), and Go to definition and Find usages over what jump_subject says, while no dialog that takes the whole
	// editor shows.
	const HeldModal modal = shown_modal(v).modal;
	const bool finds = v.project.open && v.findings.graph && (modal == HeldModal::None || modal == HeldModal::ProjectFind);
	const bool plain_ctrl = io.KeyCtrl && !io.KeyShift && !io.KeyAlt;
	if (plain_ctrl && ImGui::IsKeyPressed(ImGuiKey_P, false) && finds) ProjectFind::open(*this, ProjectFind::Scope::Files);
	if (plain_ctrl && ImGui::IsKeyPressed(ImGuiKey_T, false) && finds) ProjectFind::open(*this, ProjectFind::Scope::Names);
	if (ImGui::IsKeyPressed(ImGuiKey_F12, false) && !io.KeyCtrl && !io.KeyAlt && modal == HeldModal::None) {
		if (io.KeyShift) find_usages(v, jump_subject());
		else go_to_definition(jump_subject());
	}
	if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_B, false) && v.project.open && v.allows(EditorRequestKind::Build)) {
		request(request::build());
	}
	if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) {
		const bool plays = v.project.open && v.activity.play_state == PlayState::Stopped && v.allows(EditorRequestKind::Play);
		if (io.KeyShift && v.activity.play_state == PlayState::Running && v.allows(EditorRequestKind::StopPlay)) {
			request(request::stop_play());
		} else if (io.KeyAlt && !io.KeyCtrl && !io.KeyShift && plays) {
			// Play from here (DI-26): the active mission's view's camera, nothing where it is no mission.
			if (!play_mission_at(v, v.documents.active).empty()) request(request::play_from_here(v.documents.active));
		} else if (io.KeyCtrl && !io.KeyShift && plays) {
			// Play mission: the active document's mission, nothing where it has none.
			const std::string mission = play_mission_for(v);
			if (!mission.empty()) request(request::play(mission));
		} else if (!io.KeyShift && !io.KeyCtrl && plays) {
			request(request::play());
		}
	}
	// Back and Forward: Alt+Left and Alt+Right (a text field with the keyboard keeps its arrows, its caret's),
	// and the mouse's back and forward buttons, Dear ImGui's buttons 3 and 4 as the bridge maps them, wherever
	// the pointer is over the pass (an undocked window, a viewport's picture, a text field). In the editor's
	// own window the Shell takes those buttons first, before any control (EditorApp::_input), so a press is
	// raised once.
	for (const bool back : {true, false}) {
		const bool key = io.KeyAlt && !io.KeyCtrl && !io.KeyShift && !io.WantTextInput &&
		                 ImGui::IsKeyPressed(back ? ImGuiKey_LeftArrow : ImGuiKey_RightArrow, false);
		const bool button = ImGui::IsMouseClicked(back ? 3 : 4);
		if ((key || button) && navigation_offered(v, back))
			request(back ? request::navigate_back() : request::navigate_forward());
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
		// What runs is the running game's mode, the one its Play was started in.
		PlayMode ran = PlayMode::Runtime;
		play_mode_from_token(v.activity.play_run_mode, ran);
		state = plays_in_install(ran) ? "Game install running" : "Game running";
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
		if (clickable("##problems", widths[1], tip + " A click shows Problems.")) window_requests::focus(*this, "problems");
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
				else window_requests::set_workspace(*this, "build_result", "open", io::JsonValue::make_bool(true));
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
