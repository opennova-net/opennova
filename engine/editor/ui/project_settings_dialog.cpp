#include <editor/ui/project_settings_dialog.h>

#include <algorithm>
#include <cstring>
#include <optional>

#include <base/gameprofile/gameprofile.h>
#include <editor/project/project_document.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/ui_kit.h>

#include <imgui.h>

namespace opennova::editor {

namespace {

constexpr const char *kTitle = "Project settings";

// A path field's width: a few dozen characters of the font.
float field_width() { return ImGui::GetFontSize() * 28.0f; }

template <size_t N> void copy_into(char (&buffer)[N], const std::string &text) {
	const size_t n = std::min(text.size(), N - 1);
	std::memcpy(buffer, text.data(), n);
	buffer[n] = '\0';
}

const char *game_display_name(const std::string &code) {
	const gameprofile::GameProfile *profile = gameprofile::gameprofile_by_code(code.c_str());
	return profile != nullptr ? profile->display_name : code.c_str();
}

// A path field and its Browse..., the button under the field when the dialog is narrow;
// true when Browse... was pressed.
bool path_field(const char *label, char *buffer, size_t size, const char *browse) {
	ui_kit::WrapRow row;
	row.next(ui_kit::field_width(field_width(), label));
	ImGui::SetNextItemWidth(field_width());
	ImGui::InputText(label, buffer, size);
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

void ProjectSettingsDialog::open(const SessionView &view) {
	fields_ = Fields();
	copy_into(fields_.title, view.project.document->title);
	fields_.mission = view.project.document->features.mission;
	fields_.multiplayer = view.project.document->features.multiplayer;
	expansion_.set(view.project.document->expansion);
	copy_into(fields_.game_install, view.project.retail_directory);
	copy_into(fields_.runtime, view.project.runtime_setting);
	fields_.play_in_install = view.project.play_retail;
	root_ = view.project.root;
	open_ = open_requested_ = true;
	pick_ = PickPurpose::None;
	waiting_ = false;
	error_.clear();
}

// Inside the modal: closes it, dropping what it waited for.
void ProjectSettingsDialog::close() {
	open_ = waiting_ = false;
	pick_ = PickPurpose::None;
	error_.clear();
	ImGui::CloseCurrentPopup();
}

void ProjectSettingsDialog::set_picked(PickPurpose purpose, const std::string &path, const std::string &project_root) {
	if (!open_ || purpose != pick_ || project_root != root_) return;
	pick_ = PickPurpose::None;
	if (purpose == PickPurpose::GameInstall) copy_into(fields_.game_install, path);
	else if (purpose == PickPurpose::RuntimeExecutable) copy_into(fields_.runtime, path);
}

void ProjectSettingsDialog::draw(Workspace &workspace) {
	const SessionView &v = workspace.view();
	// The session's answers since the dialog last drew: its own Apply's is the one carrying its
	// serial (a serial another client's Apply took is never the dialog's next), its flag set when
	// a setting could not be written.
	std::optional<ViewEvent> answer;
	for (const ViewEvent &event : events_.take()) {
		if (event.kind != ViewEventKind::SettingsApplied) continue;
		if (event.tag == serial_) answer = event;
		seen_ = std::max(seen_, event.tag);
	}
	if (open_requested_) {
		open_requested_ = false;
		ImGui::OpenPopup(kTitle);
	}
	if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
	// Its project, and only it: another project open, or none, closes it.
	if (!open_ || !v.project.open || v.project.root != root_) {
		close();
		ImGui::EndPopup();
		return;
	}
	// The session's answer to its Apply, by its flag: nothing failed, it is done; else it stays
	// open saying what failed, the view's settings_result while that is its Apply's (another
	// client's Apply answered after it, before the dialog drew, replaces it: then it says only
	// that a setting failed).
	if (waiting_ && answer) {
		waiting_ = false;
		if (!answer->flag) {
			close();
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
	ImGui::SetNextItemWidth(field_width());
	ImGui::InputText("Name", fields_.title, sizeof(fields_.title));
	ImGui::Checkbox("Missions", &fields_.mission);
	ui_kit::tooltip("The game then needs the files a mission reads: Problems lists the missing ones. A mission is most "
	                "of the game, so File > Import the whole game install... is the way to bring them in.");
	ImGui::Checkbox("Multiplayer", &fields_.multiplayer);
	ImGui::SeparatorText("Expansion");
	const bool expansion_ok = expansion_.draw(v);
	if (expansion_.value().name != v.project.document->expansion.name && !v.project.document->expansion.name.empty() &&
	    !expansion_.value().name.empty())
		ImGui::TextDisabled("Apply renames the project's own expansion files to the new name.");

	ImGui::SeparatorText("This computer");
	if (path_field("Game install folder", fields_.game_install, sizeof(fields_.game_install),
	               "Browse...##install")) {
		pick_ = PickPurpose::GameInstall;
		workspace.request(request::pick_directory(PickPurpose::GameInstall));
	}
	ImGui::TextDisabled("Where the game is installed, kept with the project: its game data is imported from it.");
	if (v.activity.source_run) {
		const std::string runtime = "OpenNova runtime: " + v.activity.runtime_executable;
		const std::string shown = ui_kit::fit(runtime, field_width() * 1.5f);
		ImGui::TextUnformatted(shown.c_str());
		if (shown != runtime) ui_kit::tooltip(runtime);
		ImGui::TextDisabled("A source run: Play runs the Godot binary at the checkout.");
	} else {
		if (path_field("OpenNova runtime", fields_.runtime, sizeof(fields_.runtime), "Browse...##runtime")) {
			pick_ = PickPurpose::RuntimeExecutable;
			workspace.request(request::pick_file(PickPurpose::RuntimeExecutable));
		}
		ImGui::TextDisabled("opennova.exe; left empty, the one packaged beside the editor.");
		ui_kit::tooltip(v.activity.runtime_executable.empty() ? "No runtime is found now." : "Play runs " + v.activity.runtime_executable + ".");
	}
	ImGui::Checkbox("Play in the game install", &fields_.play_in_install);
	ui_kit::tooltip("Play starts the game install on the build instead of the OpenNova runtime.");

	if (!error_.empty()) {
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + field_width() * 1.5f);
		ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1.0f), "%s", error_.c_str());
		ImGui::PopTextWrapPos();
	}
	const bool can_apply = !waiting_ && fields_.title[0] != '\0' && expansion_ok;
	ImGui::BeginDisabled(!can_apply);
	if (ImGui::Button("Apply") && can_apply) apply(workspace);
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::Button("Cancel")) close();
	ImGui::EndPopup();
}

// Every setting as the dialog holds it, in one request: the session writes what differs
// from the settings in effect (the runtime of a source run is not the dialog's to set).
void ProjectSettingsDialog::apply(Workspace &workspace) {
	const SessionView &v = workspace.view();
	EditorRequest request = request::of(EditorRequestKind::ApplyProjectSettings);
	ProjectSettingsChange &settings = request.settings;
	serial_ = std::max(serial_, seen_) + 1;
	settings.serial = serial_;
	settings.title = std::string(fields_.title);
	settings.mission = fields_.mission;
	settings.multiplayer = fields_.multiplayer;
	const ProjectExpansion expansion = expansion_.value();
	settings.expansion = expansion.name;
	settings.builds_on = expansion.builds_on;
	settings.game_install = std::string(fields_.game_install);
	if (!v.activity.source_run) settings.runtime_executable = std::string(fields_.runtime);
	settings.play_in_install = fields_.play_in_install;
	waiting_ = true;
	error_.clear();
	workspace.request(std::move(request));
}

} // namespace opennova::editor
