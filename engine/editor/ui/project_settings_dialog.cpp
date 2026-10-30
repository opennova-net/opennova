#include <editor/ui/project_settings_dialog.h>

#include <algorithm>
#include <cstring>
#include <optional>

#include <base/io/json.h>
#include <base/title/title.h>
#include <base/version/one_build.h>
#include <base/version/version_line.h>
#include <editor/novaworld/novaworld_account.h>
#include <editor/project/base_project.h>
#include <editor/project/project_document.h>
#include <editor/session/novaworld_standing.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/session/workspace_parts.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/ui_kit.h>
#include <editor/ui/welcome_view.h>
#include <net/protocol/protocol.h>

#include <imgui.h>

namespace opennova::editor {

namespace {

constexpr const char *kTitle = "Project settings";

// A path field's width: a few dozen characters of the font.
float field_width() { return ImGui::GetFontSize() * 28.0f; }

// Where a read-only row's value starts, after its label ("What it is", "Base game", "Play with").
float label_column() { return ImGui::GetFontSize() * 7.0f; }

std::string game_display_name(const std::string &code) {
	const Title *title = title_by_code(code);
	return title != nullptr && title->display_name ? title->display_name : code;
}

// What the project is, in a line, set when it was made (ADR 0074): a game of its own of its game, or a mod of its
// base game by its short name. A mod's base game is named as NovaWorld names it where the mod's project there says
// it, else by its project's folder; a mod of the install's base game, or of an installed expansion, says so.
std::string what_it_is(const SessionView &v) {
	const ProjectDocument &doc = *v.project.document;
	const std::string game = game_display_name(doc.target_game);
	if (doc.expansion.standalone()) return "A game of its own for " + game;
	std::string base = game;
	if (doc.expansion.on_base_project()) {
		const NovaWorldView &novaworld = v.novaworld;
		std::string named;
		if (novaworld.remote_known && !novaworld.remote.base.empty()) {
			for (const RemoteGame &each : novaworld.games)
				if (each.key == novaworld.remote.base) named = each.title;
			for (const RemoteProject &each : novaworld.projects)
				if (named.empty() && each.key == novaworld.remote.base) named = each.title;
			if (named.empty()) named = novaworld.remote.base;
		}
		if (named.empty()) {
			std::string path = doc.expansion.base_project;
			while (!path.empty() && (path.back() == '/' || path.back() == '\\')) path.pop_back();
			const size_t cut = path.find_last_of("/\\");
			named = cut == std::string::npos ? path : path.substr(cut + 1);
		}
		base = named;
	} else if (!doc.expansion.builds_on.empty()) {
		base = game + " with " + doc.expansion.builds_on;
	}
	return "A mod of " + base + ", short name " + doc.expansion.name;
}

// A read-only row: its label quiet, its value after the labels' column.
void row_label(const char *label) {
	ImGui::AlignTextToFramePadding();
	ImGui::TextDisabled("%s", label);
	ImGui::SameLine(label_column());
}

// The versions the project is made for and with (base/version/version_line.h): the title it runs
// as, the protocol that title speaks ("JO protocol 16"; none for a title whose protocol is
// unwitnessed) and the ONE this editor is. A project has no release of its own: a release's
// game version is typed at Build > Release.
void version_rows(const ProjectDocument &doc) {
	Versions versions;
	title_versions(doc.target_game, versions);
	row_label("Title");
	ui_kit::clipped_text(versions.game.empty() ? doc.target_game : versions.game,
			"The retail game the project runs as: its rules and the protocol it speaks.");
	row_label("Protocol");
	ui_kit::clipped_text(versions.protocol.empty()
					? std::string("None witnessed")
					: versions.protocol + " protocol " + versions.protocol_version,
			"The in-match protocol the title speaks and its version (PV2), the revision a host "
			"checks a joiner against.");
	row_label("Engine");
	ui_kit::clipped_text(one_build().label(),
			"ONE, the OpenNova Engine this editor is: its release, commit and build.");
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
	const ProjectDocument &doc = *v.project.document;
	const NovaWorldView &novaworld = v.novaworld;
	title_.follow(held.title);
	game_install_.follow(held.game_install);
	runtime_.follow(held.runtime);

	// The project: its name, and what it is, set when it was made (a project under another name is a new one).
	ImGui::SeparatorText("Project");
	ImGui::SetNextItemWidth(field_width());
	if (ImGui::InputText("Name", title_.text, sizeof(title_.text))) set("title", io::JsonValue::make_string(title_.sent()));
	ui_kit::tooltip(v.project.root); // its folder, whole
	row_label("What it is");
	ui_kit::clipped_text(what_it_is(v), "Set when the project was made. For another short name or another game, make a "
	                                    "new project (File > New project...) and copy this one's files into its folder.");
	version_rows(doc);
	// A mod's base game's project: where its project file names it, found or missing, and Open.
	if (doc.expansion.on_base_project()) {
		const bool found = novaworld.missing_base.empty();
		row_label("Base game");
		ui_kit::clipped_text(doc.expansion.base_project + (found ? "  found" : "  missing"),
		                     found ? "The base game's project the mod builds and plays over."
		                           : "No project is there: the NovaWorld window's Make the base game's project makes it.");
		ImGui::SameLine();
		const bool opens = found && v.allows(EditorRequestKind::OpenProject);
		ImGui::BeginDisabled(!opens);
		if (ImGui::SmallButton("Open##base") && opens)
			workspace.request(request::open_project(base_project_root(v.project.root, doc.expansion)));
		ImGui::EndDisabled();
		ui_kit::tooltip("Opens the base game's project in place of this one.");
	}

	// Its NovaWorld project (ADR 0069): named by the project file's key, which no setting here changes.
	ImGui::SeparatorText("NovaWorld");
	if (novaworld.key.empty()) {
		ui_kit::quiet_text("Not on NovaWorld.");
		ImGui::SameLine();
		const bool puts = novaworld.available && novaworld.signed_in && !novaworld.calling &&
		                  v.allows(EditorRequestKind::NovaWorldPut);
		ImGui::BeginDisabled(!puts);
		if (ImGui::Button("Put on NovaWorld...") && puts) put_.open();
		ImGui::EndDisabled();
		ui_kit::tooltip(!novaworld.available ? "This editor reaches no NovaWorld service."
		                : novaworld.signed_in ? "Makes it a project on NovaWorld: a new one of yours, or an empty one "
		                                        "you may push."
		                                      : "Sign in to NovaWorld (its window) to put the project there.");
	} else {
		const NovaWorldStanding standing = novaworld_standing(novaworld);
		std::string line = novaworld_host(novaworld.server) + ": " + standing.line;
		if (novaworld.remote_known) line += novaworld.remote.is_public ? ", Public" : ", Private";
		ui_kit::clipped_text(line, standing.tip);
		if (novaworld.remote_known && novaworld.remote.lobby) {
			ui_kit::clipped_text("Lobby: " + novaworld.remote.lobby->display_name);
			ui_kit::quiet_text(novaworld.remote.lobby_locked
							? "Lobby fixed by the first release build."
							: "Manage the lobby on the site before the first release build.");
		}
		const bool site = novaworld.available && novaworld.signed_in && v.allows(EditorRequestKind::NovaWorldOpenSite);
		ImGui::BeginDisabled(!site);
		if (ImGui::Button("Open on the site") && site) workspace.request(request::novaworld_open_site("project"));
		ImGui::EndDisabled();
		ui_kit::tooltip(site ? "Its page on the NovaWorld website: its releases and who gets it."
		                     : "Sign in to NovaWorld (its window) to open its page.");
	}
	put_.draw(workspace);

	// This computer's: the game install where the game needs one (a mod made on a base game's project takes its
	// game from that project), and how the project plays here, its own (.opennova/local.json: never the editor's,
	// so it never reaches another project).
	ImGui::SeparatorText("On this computer");
	const auto set_mode = [&set](PlayMode mode) { set("play_mode", io::JsonValue::make_string(play_mode_token(mode))); };
	const bool strict = plays_in_install(held.play_mode);
	bool own_program = strict; // the game's own program offered where the install has it (or it is chosen already)
	if (!doc.expansion.on_base_project()) {
		const ui_kit::PathField install =
		        ui_kit::path_field("Game install", game_install_.text, sizeof(game_install_.text), "Browse...##install", field_width());
		if (install.browse) {
			pick_ = PickPurpose::GameInstall;
			workspace.request(request::pick_directory(PickPurpose::GameInstall));
		}
		if (install.edited) set("game_install", io::JsonValue::make_string(game_install_.sent()));
		ui_kit::tooltip(std::string(game_install_.text) + (game_install_.text[0] ? "\n" : "") +
		                "Where the game is installed, kept with the project: File > Import... brings its files in.");
		// What the folder holds, checked once it is not being typed (the UX round's project lane).
		bool found = false;
		draw_install_line(check_.words(workspace, game_install_.text, install.typing, std::string(), found), found,
		                  game_install_.text[0] != '\0');
		const InstallCheck *check = found ? check_.answer(game_install_.text) : nullptr;
		if (check && check->executable) own_program = true;
	}
	row_label("Play with");
	if (ImGui::RadioButton("OpenNova", !strict) && strict) set_mode(PlayMode::Runtime);
	ui_kit::tooltip("Play runs the OpenNova runtime on the build.");
	if (own_program) {
		ImGui::SameLine();
		if (ImGui::RadioButton("the game's own program", strict) && !strict) set_mode(PlayMode::Strict);
		ui_kit::tooltip("Play starts the game install's own program on the build, not the OpenNova runtime, as a player "
		                "who dropped it in the build's folder runs it (its program and Bink DLL alone, no /d). It starts "
		                "at its menu, where the mission is chosen.");
	}
	// DI-26: Play saves first instead of asking (S26 K6: kept here alone, the Build menu's copy gone).
	bool save_first = held.save_before_play;
	if (ImGui::Checkbox("Save all before Play", &save_first)) set("save_before_play", io::JsonValue::make_bool(save_first));
	ui_kit::tooltip("Play writes every file with unsaved edits first, as Save all does, and starts at once. Off, Play "
	                "asks first, as Build does.");

	// Advanced, collapsed: what the checklist weighs (the project's features), and the runtime Play runs.
	if (ImGui::CollapsingHeader("Advanced")) {
		bool mission = held.mission;
		if (ImGui::Checkbox("Check for the files a mission needs", &mission)) set("mission", io::JsonValue::make_bool(mission));
		ui_kit::tooltip("Problems then lists the files a mission reads that the project lacks. A mission is most of the "
		                "game: File > Import..., the whole game install, brings them in.");
		bool multiplayer = held.multiplayer;
		if (ImGui::Checkbox("Check for the files multiplayer needs", &multiplayer))
			set("multiplayer", io::JsonValue::make_bool(multiplayer));
		// What it does, in the game's terms: the files the game reads for multiplayer alone join the checklist
		// (requirement_multiplayer_only).
		ui_kit::tooltip("Problems then lists the files the game reads for multiplayer alone that the project lacks: the "
		                "NovaWorld screens (mp.mnu).");
		if (v.activity.source_run) {
			const std::string runtime = "OpenNova runtime: " + v.activity.runtime_executable;
			ImGui::TextUnformatted(ui_kit::fit(runtime, field_width() * 1.5f).c_str());
			ui_kit::tooltip(runtime + "\nA source run: Play runs the Godot binary at the checkout.");
		} else {
			const ui_kit::PathField runtime =
			        ui_kit::path_field("OpenNova runtime", runtime_.text, sizeof(runtime_.text), "Browse...##runtime", field_width());
			if (runtime.browse) {
				pick_ = PickPurpose::RuntimeExecutable;
				workspace.request(request::pick_file(PickPurpose::RuntimeExecutable));
			}
			if (runtime.edited) set("runtime", io::JsonValue::make_string(runtime_.sent()));
			ui_kit::tooltip("A game runtime (opennova.exe); left empty, the onplay.exe packaged beside the editor. " +
			                (v.activity.runtime_executable.empty() ? std::string("No runtime is found now.")
			                                                       : "Play runs " + v.activity.runtime_executable + "."));
		}
	}

	if (!error_.empty()) ui_kit::refusal_text(error_);
	const bool can_apply = !waiting_ && !held.title.empty();
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
	settings.game_install = held.game_install;
	if (!v.activity.source_run) settings.runtime_executable = held.runtime;
	settings.play_mode = held.play_mode;
	settings.save_before_play = held.save_before_play;
	waiting_ = true;
	error_.clear();
	workspace.request(std::move(request));
}

} // namespace opennova::editor
