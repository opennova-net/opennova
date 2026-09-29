#include <editor/ui/project_settings_dialog.h>

#include <algorithm>
#include <cstring>

#include <base/gameprofile/gameprofile.h>
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

} // namespace

void ProjectSettingsDialog::open(const SessionView &view) {
	fields_ = Fields();
	copy_into(fields_.title, view.document.title);
	fields_.mission = view.document.features.mission;
	fields_.multiplayer = view.document.features.multiplayer;
	copy_into(fields_.retail, view.retail_directory);
	copy_into(fields_.runtime, view.runtime_setting);
	fields_.play_retail = view.play_retail;
	root_ = view.project_root;
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
	if (purpose == PickPurpose::RetailDirectory) copy_into(fields_.retail, path);
	else if (purpose == PickPurpose::RuntimeExecutable) copy_into(fields_.runtime, path);
}

void ProjectSettingsDialog::draw(EditorHost &host) {
	const SessionView &v = host.view();
	if (open_requested_) {
		open_requested_ = false;
		ImGui::OpenPopup(kTitle);
	}
	if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
	// Its project, and only it: another project open, or none, closes it.
	if (!open_ || !v.project_open || v.project_root != root_) {
		close();
		ImGui::EndPopup();
		return;
	}
	// The session's answer to its Apply: nothing failed, it is done; else it says what
	// failed and stays open.
	if (waiting_ && v.settings_result.serial == serial_) {
		waiting_ = false;
		for (const Diagnostic &failure : v.settings_result.failures)
			error_ += (error_.empty() ? "" : "\n") + failure.message;
		if (error_.empty()) {
			close();
			ImGui::EndPopup();
			return;
		}
	}
	// The project's folder, a long one cut (whole in its tooltip): the dialog fits its fields.
	const std::string root = ui_kit::fit(v.project_root, field_width() * 1.5f);
	ImGui::TextDisabled("%s", root.c_str());
	if (root != v.project_root) ui_kit::tooltip(v.project_root);
	ImGui::Text("Game: %s", game_display_name(v.document.target_game));
	ImGui::SetNextItemWidth(field_width());
	ImGui::InputText("Name", fields_.title, sizeof(fields_.title));
	ImGui::Checkbox("Missions", &fields_.mission);
	ui_kit::tooltip("The game then needs the files a mission reads: Problems lists the missing ones.");
	ImGui::Checkbox("Multiplayer", &fields_.multiplayer);

	ImGui::SeparatorText("This computer");
	if (path_field("Game install folder", fields_.retail, sizeof(fields_.retail), "Browse...##retail")) {
		pick_ = PickPurpose::RetailDirectory;
		EditorRequest pick = make_request(EditorRequestKind::PickDirectory);
		pick.purpose = PickPurpose::RetailDirectory;
		host.request(pick);
	}
	ImGui::TextDisabled("Where the game is installed, kept with the project: its game data is imported from it.");
	if (v.source_run) {
		const std::string runtime = "OpenNova runtime: " + v.runtime_executable;
		const std::string shown = ui_kit::fit(runtime, field_width() * 1.5f);
		ImGui::TextUnformatted(shown.c_str());
		if (shown != runtime) ui_kit::tooltip(runtime);
		ImGui::TextDisabled("A source run: Play runs the Godot binary at the checkout.");
	} else {
		if (path_field("OpenNova runtime", fields_.runtime, sizeof(fields_.runtime), "Browse...##runtime")) {
			pick_ = PickPurpose::RuntimeExecutable;
			EditorRequest pick = make_request(EditorRequestKind::PickFile);
			pick.purpose = PickPurpose::RuntimeExecutable;
			host.request(pick);
		}
		ImGui::TextDisabled("opennova.exe; left empty, the one packaged beside the editor.");
		ui_kit::tooltip(v.runtime_executable.empty() ? "No runtime is found now." : "Play runs " + v.runtime_executable + ".");
	}
	ImGui::Checkbox("Play in the game install", &fields_.play_retail);
	ui_kit::tooltip("Play starts the game install on the build instead of the OpenNova runtime.");

	if (!error_.empty()) {
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + field_width() * 1.5f);
		ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1.0f), "%s", error_.c_str());
		ImGui::PopTextWrapPos();
	}
	const bool can_apply = !waiting_ && fields_.title[0] != '\0';
	ImGui::BeginDisabled(!can_apply);
	if (ImGui::Button("Apply") && can_apply) apply(host);
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::Button("Cancel")) close();
	ImGui::EndPopup();
}

// Every setting as the dialog holds it, in one request: the session writes what differs
// from the settings in effect (the runtime of a source run is not the dialog's to set).
void ProjectSettingsDialog::apply(EditorHost &host) {
	const SessionView &v = host.view();
	EditorRequest request = make_request(EditorRequestKind::ApplyProjectSettings);
	ProjectSettingsChange &settings = request.settings;
	serial_ = std::max(serial_, v.settings_result.serial) + 1;
	settings.serial = serial_;
	settings.title = std::string(fields_.title);
	settings.mission = fields_.mission;
	settings.multiplayer = fields_.multiplayer;
	settings.retail_directory = std::string(fields_.retail);
	if (!v.source_run) settings.runtime_executable = std::string(fields_.runtime);
	settings.play_retail = fields_.play_retail;
	waiting_ = true;
	error_.clear();
	host.request(std::move(request));
}

} // namespace opennova::editor
