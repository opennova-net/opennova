#include <editor/ui/project_settings_dialog.h>

#include <algorithm>
#include <cstring>
#include <optional>

#include <base/gameprofile/gameprofile.h>
#include <base/io/json.h>
#include <editor/project/project_document.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/session/workspace_parts.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/ui_kit.h>
#include <editor/ui/welcome_view.h>

#include <imgui.h>

namespace opennova::editor {

namespace {

constexpr const char *kTitle = "Project settings";

// A path field's width: a few dozen characters of the font.
float field_width() { return ImGui::GetFontSize() * 28.0f; }

const char *game_display_name(const std::string &code) {
	const gameprofile::GameProfile *profile = gameprofile::gameprofile_by_code(code.c_str());
	return profile != nullptr ? profile->display_name : code.c_str();
}

// A path field and its Browse..., the button under the field when the dialog is narrow;
// true when Browse... was pressed; `typing`, when given, whether the field is being typed in, and
// `edited` whether its text changed this frame.
bool path_field(const char *label, char *buffer, size_t size, const char *browse, bool *typing = nullptr,
                bool *edited = nullptr) {
	ui_kit::WrapRow row;
	row.next(ui_kit::field_width(field_width(), label));
	ImGui::SetNextItemWidth(field_width());
	const bool changed = ImGui::InputText(label, buffer, size);
	if (edited) *edited = changed;
	if (typing) *typing = ImGui::IsItemActive();
	row.next(ui_kit::button_width(browse));
	return ImGui::Button(browse);
}

// Whether no Apply was answered after the one the SettingsApplied event at `seq` answers: the
// view's settings_result is then that Apply's (each answer replaces it). An answer the view no
// longer holds is not known to be the last.
bool last_settings_answer(const SessionView &view, uint64_t seq) {
	if (seq < view.events.first_seq()) return false;
	for (const ViewEvent &event : view.events.held())
		if (event.kind == ViewEventKind::SettingsApplied && event.seq > seq) return false;
	return true;
}

} // namespace

// Closed (Cancel, its Apply answered with nothing failed): the workspace's settings closed.
void ProjectSettingsDialog::close(Workspace &workspace) {
	window_requests::set_workspace(workspace, "settings", "open", io::JsonValue::make_bool(false));
}

void ProjectSettingsDialog::set_picked(Workspace &workspace, PickPurpose purpose, const std::string &path,
                                       const std::string &project_root) {
	if (!workspace.view().workspace.settings.open || purpose != pick_ || project_root != root_) return;
	pick_ = PickPurpose::None;
	if (purpose == PickPurpose::GameInstall)
		window_requests::set_workspace(workspace, "settings", "game_install", io::JsonValue::make_string(path));
	else if (purpose == PickPurpose::RuntimeExecutable)
		window_requests::set_workspace(workspace, "settings", "runtime", io::JsonValue::make_string(path));
}

void ProjectSettingsDialog::draw(Workspace &workspace) {
	const SessionView &v = workspace.view();
	const WorkspaceView::Settings &held = v.workspace.settings;
	const auto set = [&workspace](const char *member, io::JsonValue value) {
		window_requests::set_workspace(workspace, "settings", member, std::move(value));
	};
	// The session's answers since the dialog last drew: its own Apply's is the one carrying its
	// serial (a serial another client's Apply took is never the dialog's next), its flag set when
	// a setting could not be written.
	std::optional<ViewEvent> answer;
	for (const ViewEvent &event : events_.take()) {
		if (event.kind != ViewEventKind::SettingsApplied) continue;
		if (event.tag == serial_) answer = event;
		seen_ = std::max(seen_, event.tag);
	}
	// Each opening starts afresh: nothing waited for, nothing failed, the install checked again.
	if (held.open && !shown_) {
		root_ = v.project.root;
		pick_ = PickPurpose::None;
		waiting_ = false;
		error_.clear();
		check_.forget();
	}
	shown_ = held.open;
	// Held open, it shows when no dialog before it in the session's order is held (shown_modal), and waits
	// closed while one is.
	const bool shows = held.open && v.project.open && modal_may_show(v, HeldModal::Settings);
	if (shows && !ImGui::IsPopupOpen(kTitle)) ImGui::OpenPopup(kTitle);
	if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
	// Closed another way (the session closes it with its project, the editor MCP), or another dialog shows.
	if (!shows) {
		ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		return;
	}
	// The session's answer to its Apply, by its flag: nothing failed, it is done (the session closed it);
	// else it stays open saying what failed, the view's settings_result while that is its Apply's (another
	// client's Apply answered after it, before the dialog drew, replaces it: then it says only that a
	// setting failed).
	if (waiting_ && answer) {
		waiting_ = false;
		if (!answer->flag) {
			ImGui::EndPopup();
			return;
		}
		if (last_settings_answer(v, answer->seq))
			for (const Diagnostic &failure : v.project.settings_result.failures)
				error_ += (error_.empty() ? "" : "\n") + failure.message;
		if (error_.empty()) error_ = "A setting could not be saved: see Problems.";
	}
	// The project's folder, a long one cut (whole in its tooltip): the dialog fits its fields.
	const std::string root = ui_kit::fit(v.project.root, field_width() * 1.5f);
	ImGui::TextDisabled("%s", root.c_str());
	if (root != v.project.root) ui_kit::tooltip(v.project.root);
	ImGui::Text("Game: %s", game_display_name(v.project.document->target_game));
	title_.follow(held.title);
	game_install_.follow(held.game_install);
	runtime_.follow(held.runtime);
	ImGui::SetNextItemWidth(field_width());
	if (ImGui::InputText("Name", title_.text, sizeof(title_.text))) set("title", io::JsonValue::make_string(title_.sent()));
	bool mission = held.mission;
	if (ImGui::Checkbox("Missions", &mission)) set("mission", io::JsonValue::make_bool(mission));
	ui_kit::tooltip("The game then needs the files a mission reads: Problems lists the missing ones. A mission is most "
	                "of the game, so File > Import the whole game install... is the way to bring them in.");
	bool multiplayer = held.multiplayer;
	if (ImGui::Checkbox("Multiplayer", &multiplayer)) set("multiplayer", io::JsonValue::make_bool(multiplayer));
	// What it does today, in the game's terms (the UX round's project lane): the requirements have no
	// multiplayer phase (requirement_phase_enabled), so it changes no check.
	ui_kit::tooltip("Kept with the project; no check reads it yet. A multiplayer game reads the menus' files and a "
	                "mission's: Missions brings the files a mission needs into Problems.");
	ImGui::SeparatorText("Expansion");
	ExpansionChoice choice{ held.builds_on, held.as_expansion, held.expansion };
	bool changed = false;
	const bool expansion_ok = expansion_.draw(choice, v.project.install_expansions, v.project.document->expansion, changed);
	if (changed) {
		io::JsonValue members = io::JsonValue::make_object();
		members.set("builds_on", io::JsonValue::make_string(choice.builds_on));
		members.set("as_expansion", io::JsonValue::make_bool(choice.as_expansion));
		members.set("expansion", io::JsonValue::make_string(choice.name));
		window_requests::set_workspace(workspace, "settings", std::move(members));
	}
	if (choice.value().name != v.project.document->expansion.name && !v.project.document->expansion.name.empty() &&
	    !choice.value().name.empty())
		ImGui::TextDisabled("Apply renames the project's own expansion files to the new name.");

	ImGui::SeparatorText("This computer");
	bool typing = false, edited = false;
	if (path_field("Game install folder", game_install_.text, sizeof(game_install_.text), "Browse...##install", &typing,
	               &edited)) {
		pick_ = PickPurpose::GameInstall;
		workspace.request(request::pick_directory(PickPurpose::GameInstall));
	}
	if (edited) set("game_install", io::JsonValue::make_string(game_install_.sent()));
	ui_kit::tooltip(game_install_.text);
	// What the folder holds, checked once it is not being typed (the UX round's project lane).
	bool found = false;
	const std::string words = check_.words(workspace, game_install_.text, typing, std::string(), found);
	ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + field_width() * 1.5f);
	ImGui::PushStyleColor(ImGuiCol_Text, words.empty() || found ? ImVec4(0.55f, 0.85f, 0.55f, 1.0f) : ImVec4(0.95f, 0.55f, 0.45f, 1.0f));
	ImGui::TextWrapped("%s", words.empty() ? "Checking the folder..." : words.c_str());
	ImGui::PopStyleColor();
	ImGui::PopTextWrapPos();
	ImGui::TextDisabled("Where the game is installed, kept with the project: its game data is imported from it.");
	if (v.activity.source_run) {
		const std::string runtime = "OpenNova runtime: " + v.activity.runtime_executable;
		const std::string shown = ui_kit::fit(runtime, field_width() * 1.5f);
		ImGui::TextUnformatted(shown.c_str());
		if (shown != runtime) ui_kit::tooltip(runtime);
		ImGui::TextDisabled("A source run: Play runs the Godot binary at the checkout.");
	} else {
		bool runtime_edited = false;
		if (path_field("OpenNova runtime", runtime_.text, sizeof(runtime_.text), "Browse...##runtime", nullptr,
		               &runtime_edited)) {
			pick_ = PickPurpose::RuntimeExecutable;
			workspace.request(request::pick_file(PickPurpose::RuntimeExecutable));
		}
		if (runtime_edited) set("runtime", io::JsonValue::make_string(runtime_.sent()));
		ImGui::TextDisabled("opennova.exe; left empty, the one packaged beside the editor.");
		ui_kit::tooltip(v.activity.runtime_executable.empty() ? "No runtime is found now." : "Play runs " + v.activity.runtime_executable + ".");
	}
	// How the project plays: its own, kept in its local settings on this computer (never the editor's, so it
	// never reaches another project, nor another editor on the machine).
	ImGui::SeparatorText("Play");
	const auto set_mode = [&set](PlayMode mode) { set("play_mode", io::JsonValue::make_string(play_mode_token(mode))); };
	bool in_install = plays_in_install(held.play_mode);
	if (ImGui::Checkbox("Play in the game install", &in_install)) set_mode(in_install ? PlayMode::Install : PlayMode::Runtime);
	ui_kit::tooltip("Play starts the game install on the build instead of the OpenNova runtime.");
	ImGui::Indent();
	ImGui::BeginDisabled(!in_install);
	bool strict = held.play_mode == PlayMode::Strict;
	if (ImGui::Checkbox("Strict: as a player's install", &strict)) set_mode(strict ? PlayMode::Strict : PlayMode::Install);
	ImGui::EndDisabled();
	ImGui::Unindent();
	ui_kit::tooltip("The game runs on the build alone, as a player who dropped Jointops.exe into the build's folder "
	                "runs it: nothing of the install but its program and Bink DLL, no /d. Off, the install's "
	                "configuration and saves are beside it and loose files win (/d).");
	ImGui::TextDisabled("This project's, on this computer (.opennova/local.json): other projects play as each is set.");

	if (!error_.empty()) {
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + field_width() * 1.5f);
		ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1.0f), "%s", error_.c_str());
		ImGui::PopTextWrapPos();
	}
	const bool can_apply = !waiting_ && !held.title.empty() && expansion_ok;
	ImGui::BeginDisabled(!can_apply);
	if (ImGui::Button("Apply") && can_apply) apply(workspace);
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::Button("Cancel")) close(workspace);
	ImGui::EndPopup();
}

// Every setting as the dialog holds it (the workspace's, whole: what a client set too, never the fields'
// copies), in one request: the session writes what differs from the settings in effect (the runtime of a
// source run is not the dialog's to set).
void ProjectSettingsDialog::apply(Workspace &workspace) {
	const SessionView &v = workspace.view();
	const WorkspaceView::Settings &held = v.workspace.settings;
	EditorRequest request = request::of(EditorRequestKind::ApplyProjectSettings);
	ProjectSettingsChange &settings = request.settings;
	serial_ = std::max(serial_, seen_) + 1;
	settings.serial = serial_;
	settings.title = held.title;
	settings.mission = held.mission;
	settings.multiplayer = held.multiplayer;
	const ProjectExpansion expansion = ExpansionChoice{ held.builds_on, held.as_expansion, held.expansion }.value();
	settings.expansion = expansion.name;
	settings.builds_on = expansion.builds_on;
	settings.game_install = held.game_install;
	if (!v.activity.source_run) settings.runtime_executable = held.runtime;
	settings.play_mode = held.play_mode;
	waiting_ = true;
	error_.clear();
	workspace.request(std::move(request));
}

} // namespace opennova::editor
