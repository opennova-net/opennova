#include <editor/ui/welcome_view.h>

#include <algorithm>
#include <cstring>
#include <iterator>

#include <base/gameprofile/gameprofile.h>
#include <base/io/json.h>
#include <base/io/strutil.h>
#include <editor/project/expansion_name.h>
#include <editor/project/local_settings.h>
#include <editor/project/project_document.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/session/workspace_parts.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/novaworld_window.h>
#include <editor/ui/ui_kit.h>

#include <imgui.h>

namespace opennova::editor {

namespace {

template <size_t N> void copy_into(char (&buffer)[N], const std::string &text) {
	const size_t n = std::min(text.size(), N - 1);
	std::memcpy(buffer, text.data(), n);
	buffer[n] = '\0';
}

// One recent project: its title, then its game, its expansion and its folder (the middle of a long folder
// giving way), a click opening it; Forget.
void recent_row(Workspace &workspace, const std::string &root, const ProjectView::RecentProject *details, bool opens) {
	const SessionView &v = workspace.view();
	ImGui::PushID(root.c_str());
	ImGui::TableNextRow();
	ImGui::TableNextColumn();
	const float line = ImGui::GetTextLineHeight();
	const float spacing = ImGui::GetStyle().ItemSpacing.y;
	const ImVec2 at = ImGui::GetCursorPos();
	ImGuiSelectableFlags flags = ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap;
	const bool found = !details || details->found;
	if (!opens || !found) flags |= ImGuiSelectableFlags_Disabled;
	if (ImGui::Selectable("###open", false, flags, ImVec2(0.0f, line * 2.0f + spacing)) && opens && found)
		workspace.request(request::open_project(root));
	ui_kit::tooltip(found ? "Open " + root : root + " holds no project now: Forget takes it off the list.");
	ImGui::SetCursorPos(at);
	const std::string title = details && !details->title.empty() ? details->title : root;
	ImGui::TextUnformatted(ui_kit::fit(title, ImGui::GetContentRegionAvail().x).c_str());
	std::string what;
	if (!found) {
		what = "No project here now";
	} else if (details) {
		what = details->game;
		if (!details->expansion.empty())
			what += ", as the expansion " + details->expansion +
			        (details->builds_on.empty() ? std::string() : " on " + details->builds_on);
	}
	what += (what.empty() ? "" : "   ") + root;
	ImGui::TextDisabled("%s", ui_kit::fit_middle(what, ImGui::GetContentRegionAvail().x).c_str());
	ImGui::TableNextColumn();
	const bool forgets = v.allows(EditorRequestKind::ForgetRecent);
	ImGui::BeginDisabled(!forgets);
	if (ImGui::SmallButton("Forget") && forgets) workspace.request(request::forget_recent(root));
	ImGui::EndDisabled();
	ui_kit::tooltip("Take it off this list; the project stays where it is.");
	ImGui::PopID();
}

// The projects of NovaWorld (ADR 0069), yours and everyone's: a project got from it opens, and an empty one of
// yours is set up in the New project form. Its sign-in is the NovaWorld window's, which the button shows over the
// welcome page.
void draw_from_store(Workspace &workspace) {
	const SessionView &v = workspace.view();
	if (!v.novaworld.available) return;
	ImGui::Spacing();
	ImGui::SeparatorText("Get a project from NovaWorld");
	draw_novaworld_projects(workspace);
	if (v.novaworld.signed_in) return;
	ImGui::SameLine();
	if (ImGui::Button("Sign in...")) window_requests::focus(workspace, "novaworld");
	ui_kit::tooltip("Signs in with your NovaWorld account: your own projects list here too, private ones with them.");
}

void draw_recent(Workspace &workspace) {
	const SessionView &v = workspace.view();
	const bool opens = v.allows(EditorRequestKind::OpenProject);
	ImGui::SeparatorText("Open a project");
	ImGui::BeginDisabled(!opens);
	if (ImGui::Button("Open a project folder...") && opens)
		workspace.request(request::pick_directory(PickPurpose::OpenProject));
	ImGui::EndDisabled();
	if (v.project.recent_projects.empty()) {
		ui_kit::empty_state("No recent project.", "The projects you open are listed here.");
		return;
	}
	ImGui::Spacing();
	if (!ImGui::BeginTable("recent", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) return;
	ImGui::TableSetupColumn("Project", ImGuiTableColumnFlags_WidthStretch);
	ImGui::TableSetupColumn("##forget", ImGuiTableColumnFlags_WidthFixed);
	for (size_t i = 0; i < v.project.recent_projects.size(); ++i) {
		const std::string &root = v.project.recent_projects[i];
		const ProjectView::RecentProject *details =
				i < v.project.recent_details.size() && v.project.recent_details[i].root == root ? &v.project.recent_details[i]
				                                                                               : nullptr;
		recent_row(workspace, root, details, opens);
	}
	ImGui::EndTable();
}

} // namespace

std::string InstallFieldCheck::words(Workspace &workspace, const std::string &path, bool typing, const std::string &fallback,
                                     bool &ok) {
	ok = false;
	if (path.empty())
		return fallback.empty() ? std::string("No game install chosen: files come from the disk.")
		                        : "Left empty, it takes the editor's install, " + fallback + ".";
	const std::string root = absolute_install_path(path);
	const InstallCheck &slot = workspace.view().project.install_check;
	// The slot's answer for this folder, kept: another field's check over the slot leaves it.
	if (slot.root == root && (kept_.root != root || kept_.words() != slot.words())) kept_ = slot;
	if (kept_.root == root) {
		ok = kept_.ok();
		return kept_.words();
	}
	if (typing || !workspace.view().allows(EditorRequestKind::CheckInstall)) return std::string();
	// Asked once a folder; again (twice at most) when another folder's answer took the slot since it asked,
	// before this field read its own.
	const bool taken = asked_ == path && slot.root != slot_when_asked_ && slot.root != root && retries_ < 2;
	if (asked_ != path || taken) {
		retries_ = asked_ == path ? retries_ + 1 : 0;
		asked_ = path;
		slot_when_asked_ = slot.root;
		workspace.request(request::check_install(path));
	}
	return std::string();
}

void draw_install_line(const std::string &words, bool ok, bool named) {
	if (words.empty() || !named) return ui_kit::quiet_text(words.empty() ? "Checking the folder..." : words);
	if (!ok) return ui_kit::refusal_text(words);
	ImGui::PushStyleColor(ImGuiCol_Text, ui_kit::reference_color(ReferenceStatus::Present));
	ui_kit::clipped_text(words);
	ImGui::PopStyleColor();
}

const InstallCheck *InstallFieldCheck::answer(const std::string &path) const {
	return !path.empty() && kept_.root == absolute_install_path(path) ? &kept_ : nullptr;
}

void InstallFieldCheck::forget() {
	asked_.clear();
	slot_when_asked_.clear();
	kept_ = InstallCheck();
	retries_ = 0;
}

EditorRequest new_project_request(const WorkspaceView::NewProject &form, const std::string &install,
                                  const std::string &stock_title) {
	using Making = WorkspaceView::Making;
	EditorRequest made;
	switch (form.making) {
	case Making::Stock:
		// An installed expansion is made on the base game's project; the base game stands alone.
		made = request::new_stock_project(form.dir, stock_title, install, form.installed,
		                                  form.installed.empty() ? std::string() : form.base_project);
		break;
	case Making::Mod:
		// A mod of a NovaWorld game (ADR 0074): the session finds its base game's project, or makes it, first.
		made = request::new_expansion_project(form.dir, form.title, form.expansion);
		made.base_key = form.base_key;
		made.game_install = install;
		break;
	case Making::Game:
		made = request::new_project(form.dir, form.title);
		made.game_install = install;
		break;
	}
	made.project_key = form.novaworld; // the NovaWorld project it is set up for (Set up..., ADR 0069)
	return made;
}

namespace {

// What the form offers, each with what it makes.
struct MakingRow {
	WorkspaceView::Making making;
	const char *label;
	const char *tip;
};
constexpr MakingRow kMakings[] = {
	{ WorkspaceView::Making::Stock, "A game you own",
	  "The game as your game install has it, or one of its expansions: every file of it imported into the project, the "
	  "game's own files as it ships them." },
	{ WorkspaceView::Making::Mod, "A mod",
	  "A change to a game: its own files over the game's, shipped as expansion\\<name>\\ and played with /exp <name>." },
	{ WorkspaceView::Making::Game, "A new game", "A game of its own, standalone: you bring in the files it starts from." },
};

// An installed expansion as the combos name it: its folder's name, the Mods list's name after it.
std::string installed_words(const InstallCheck::Expansion &installed) {
	return installed.title.empty() || installed.title == installed.name ? installed.name
	                                                                      : installed.name + " - " + installed.title;
}

// A game's name by its gameprofile code (Joint Operations), JO's for none.
std::string game_name(const std::string &code) {
	const gameprofile::GameProfile *profile = gameprofile::gameprofile_by_code(code.empty() ? kDefaultTargetGame : code.c_str());
	return profile && profile->display_name ? profile->display_name : code;
}

// The base game's name: the game the install was read as (Joint Operations).
std::string base_game_name(const InstallCheck *check) { return game_name(check ? check->game : std::string()); }

// One of NovaWorld's games as the A mod of list names it: its name, then its key.
std::string novaworld_game_words(const RemoteGame &game) {
	return game.title.empty() || game.title == game.key ? game.key : game.title + " (" + game.key + ")";
}

// Whether a mod of NovaWorld's game `key` is made on a project imported from the game install, as the session's
// make_base takes it: a retail game's (its files the install's), or one the account may not read (never listed).
bool base_from_install(const NovaWorldView &novaworld, const std::string &key) {
	for (const RemoteProject &project : novaworld.projects)
		if (project.key == key) return project.retail;
	return true;
}

} // namespace

bool NewProjectForm::draw(Workspace &workspace) {
	using Making = WorkspaceView::Making;
	const SessionView &v = workspace.view();
	const WorkspaceView::NewProject &form = v.workspace.new_project;
	const auto set = [&workspace](const char *member, io::JsonValue value) {
		window_requests::set_workspace(workspace, "new_project", member, std::move(value));
	};
	// A new project is refused while an operation that cannot be cancelled runs: its Browse... and
	// Create wait with it (the busy gate's answer, SessionView::allows).
	const bool allowed = v.allows(EditorRequestKind::NewProject);
	title_.follow(form.title);
	folder_.follow(form.dir);
	base_.follow(form.base_project);
	name_.follow(form.expansion);
	// The install, the editor's own until the author names one (never another project's: an open project's
	// install may be its own, or a session's); checked once it is not being typed.
	install_.follow(form.install_named ? form.game_install : v.project.editor_install);
	// Set up for a NovaWorld project (the projects list's Set up...): said, with its clear.
	if (!form.novaworld.empty()) {
		ui_kit::clipped_text("For NovaWorld project " + form.novaworld,
		                     "Create writes " + form.novaworld + " in the project's file: the folder is then its "
		                     "working copy, ready to push.");
		ImGui::SameLine();
		if (ImGui::SmallButton("x##novaworld")) set("novaworld", io::JsonValue::make_string(""));
		ui_kit::tooltip("Makes the project for no NovaWorld project.");
	}
	// What it makes, the form's first question.
	for (size_t i = 0; i < std::size(kMakings); ++i) {
		if (i) ImGui::SameLine();
		if (ImGui::RadioButton(kMakings[i].label, form.making == kMakings[i].making) && form.making != kMakings[i].making)
			set("making", io::JsonValue::make_string(new_project_making_token(kMakings[i].making)));
		ui_kit::tooltip(kMakings[i].tip);
	}
	const ui_kit::PathField field = ui_kit::path_field("Game install", install_.text, sizeof(install_.text), "Browse...##install", 0.0f, allowed);
	if (field.edited) set("game_install", io::JsonValue::make_string(install_.sent()));
	if (field.browse) workspace.request(request::pick_directory(PickPurpose::NewProjectInstall));
	ui_kit::tooltip("The game's folder: the project imports its files from it and plays in it. Kept with the project.");
	bool found = false;
	const std::string words = check_.words(workspace, install_.text, field.typing, v.project.editor_install, found);
	draw_install_line(words, found, install_.text[0] != '\0');
	// The install's games: its base game, then each of its expansions, once it is checked.
	const InstallCheck *check = found ? check_.answer(install_.text) : nullptr;
	std::vector<InstallCheck::Expansion> installed;
	if (check) installed = check->expansions;
	else if (!install_.text[0])
		for (const ProjectView::InstallExpansion &each : v.project.new_project_expansions) installed.push_back({ each.name, each.title });
	const std::string base_name = base_game_name(check);
	const InstallCheck::Expansion *chosen = nullptr;
	for (const InstallCheck::Expansion &each : installed)
		if (strutil::iequals(each.name, form.installed)) chosen = &each;
	const std::string chosen_words = form.installed.empty() ? base_name
	                                 : chosen               ? installed_words(*chosen)
	                                                        : form.installed + " (not in the game install)";
	// The install's game a game you own is: its base game, or an installed expansion.
	const auto game_combo = [&](const char *label) {
		if (!ImGui::BeginCombo(label, chosen_words.c_str())) return;
		const bool base_now = form.installed.empty();
		if (ImGui::Selectable((base_name + " (the base game)").c_str(), base_now) && !base_now)
			set("installed", io::JsonValue::make_string(""));
		for (const InstallCheck::Expansion &each : installed) {
			// Only what the game can mount by its name is offered (expansion_name_problem's rule for an installed one).
			if (!expansion_name_problem(each.name, ExpansionNameUse::BuildsOn).empty()) continue;
			const bool now = strutil::iequals(each.name, form.installed);
			if (ImGui::Selectable(installed_words(each).c_str(), now) && !now)
				set("installed", io::JsonValue::make_string(each.name));
		}
		ImGui::EndCombo();
	};
	// The base game's project an expansion stands on: a folder, Browse..., the one beside it offered.
	const auto base_field = [&](const char *tip) {
		const ui_kit::PathField base = ui_kit::path_field("Base game's project", base_.text, sizeof(base_.text), "Browse...##base", 0.0f, allowed);
		if (base.edited) set("base_project", io::JsonValue::make_string(base_.sent()));
		if (base.browse) workspace.request(request::pick_directory(PickPurpose::NewProjectBase));
		ui_kit::tooltip(tip);
	};
	const auto name_field = [&] {
		if (ImGui::InputText("Name", title_.text, sizeof(title_.text))) set("title", io::JsonValue::make_string(title_.sent()));
	};
	std::string why; // why Create waits ("" it does not)
	std::string stock_title = base_name;
	switch (form.making) {
	case Making::Stock:
		game_combo("Game");
		ui_kit::tooltip("Which game of the install the project is: its base game, or one of its expansions, made on the "
		                "base game's project.");
		if (!form.installed.empty()) {
			stock_title = chosen && !chosen->title.empty() ? chosen->title : form.installed;
			base_field(("Your " + base_name + " project's folder: " + form.installed + " is made on it, as the game installs "
			            "it over " + base_name + ".").c_str());
			if (form.base_project.empty()) {
				why = "Make " + base_name + " first: " + stock_title + " is made on its project. Then name its folder here.";
				ui_kit::refusal_text(why);
			}
		}
		if (!found && why.empty())
			why = words.empty() ? std::string("The game install is being checked.") : "A game you own is made from its game install.";
		break;
	case Making::Mod: {
		// A mod is of one of NovaWorld's games (ADR 0074): those the account may make a mod of, its sign-in the
		// NovaWorld window's.
		const NovaWorldView &novaworld = v.novaworld;
		const RemoteGame *of = nullptr;
		for (const RemoteGame &game : novaworld.games)
			if (game.key == form.base_key) of = &game;
		if (!novaworld.available) {
			why = "This editor reaches no NovaWorld service: a mod is of one of its games.";
			ui_kit::refusal_text(why);
		} else if (!novaworld.signed_in) {
			ui_kit::quiet_text("Sign in to NovaWorld to make a mod: a mod is of one of its games.");
			ImGui::SameLine();
			if (ImGui::SmallButton("Sign in...##mod")) window_requests::focus(workspace, "novaworld");
			ui_kit::tooltip("Shows the NovaWorld window, where you sign in.");
			why = "Sign in to NovaWorld first.";
		} else {
			const std::string shown = of ? novaworld_game_words(*of)
			                          : novaworld.games.empty() ? std::string("No game listed")
			                                                    : std::string("Choose its game");
			if (ImGui::BeginCombo("A mod of", shown.c_str())) {
				for (const RemoteGame &game : novaworld.games)
					if (ImGui::Selectable(novaworld_game_words(game).c_str(), &game == of) && &game != of)
						set("base_key", io::JsonValue::make_string(game.key));
				ImGui::EndCombo();
			}
			ui_kit::tooltip("The NovaWorld game the mod changes. Create makes the mod on its project: the one on this "
			                "computer, or one made beside the mod first (a retail game's imported from your game install, "
			                "any other got from NovaWorld).");
			if (!of) {
				why = novaworld.games.empty() ? "NovaWorld lists no game yet: List projects in the NovaWorld window."
				                              : "Choose the game it is a mod of.";
			} else if (base_from_install(novaworld, of->key) && (!check || check->game != of->game)) {
				why = "Needs a " + game_name(of->game) + " install (Game install): a mod of " + of->key +
				      " is made over its files.";
				ui_kit::refusal_text(why);
			}
		}
		name_field();
		if (ImGui::InputText("Short name", name_.text, sizeof(name_.text))) set("expansion", io::JsonValue::make_string(name_.sent()));
		ui_kit::tooltip("The mod's folder, expansion\\<name>\\, and what a player types to play it: /exp <name>.");
		std::string problem = expansion_name_problem(form.expansion, ExpansionNameUse::Own);
		if (problem.empty())
			for (const InstallCheck::Expansion &each : installed)
				if (strutil::iequals(each.name, form.expansion)) problem = "The game install has an expansion named " + each.name + " already.";
		if (!problem.empty()) {
			ui_kit::refusal_text(problem);
			if (why.empty()) why = problem;
		}
		break;
	}
	case Making::Game: name_field(); break;
	}
	const ui_kit::PathField folder = ui_kit::path_field("Folder", folder_.text, sizeof(folder_.text), "Browse...##folder", 0.0f, allowed);
	if (folder.edited) set("dir", io::JsonValue::make_string(folder_.sent()));
	if (folder.browse) workspace.request(request::pick_directory(PickPurpose::NewProjectLocation));
	ui_kit::tooltip("The folder is made if it does not exist; it must not hold a project already.");
	// A folder named that holds no install refuses the project (the session says why, as it checks the
	// folder again); none at all is a project of the files from the disk; one not checked yet is the session's
	// to check.
	const bool install_ok = install_.text[0] == '\0' || found || words.empty();
	if (why.empty() && !install_ok) why = "Name a folder that holds the game, or none.";
	if (why.empty() && form.making != Making::Stock && form.title.empty()) why = "Name it first.";
	if (why.empty() && form.dir.empty()) why = "Choose its folder first.";
	// Create raises what the session holds of the form (what a client set, whole), not the fields' copies.
	const std::string install = form.install_named ? form.game_install : v.project.editor_install;
	const bool ready = allowed && why.empty();
	ImGui::BeginDisabled(!ready);
	const bool create = ImGui::Button("Create project") && ready;
	ImGui::EndDisabled();
	if (!why.empty()) ui_kit::tooltip(why);
	else if (form.making == Making::Stock)
		ui_kit::tooltip("Makes the project and imports every file of " + stock_title + " into it, the game's own files "
		                "as it ships them.");
	if (create) workspace.request(new_project_request(form, install, stock_title));
	return create;
}

bool aside_for_welcome(const SessionView &view, bool &asked) {
	if (view.project.open) {
		asked = false;
		return false;
	}
	return !asked;
}

void draw_welcome(Workspace &workspace, NewProjectForm &form) {
	const SessionView &v = workspace.view();
	const float em = ImGui::GetFontSize();
	// One column up to 80 ems wide in the middle of the workspace; two side by side when it has the room.
	const float room = ImGui::GetContentRegionAvail().x;
	const float width = std::min(room, em * 80.0f);
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (room - width) * 0.5f);
	ImGui::BeginGroup();
	ImGui::Dummy(ImVec2(width, em * 0.5f));
	ImGui::SetWindowFontScale(1.6f);
	ImGui::TextUnformatted("OpenNova Editor");
	ImGui::SetWindowFontScale(1.0f);
	ui_kit::tooltip("A project is your mod's own files: import the game's, change them, build, and play.");
	// Why the last New project or Open was refused, in its own words: Output and Problems stand aside while no
	// project is open, so this is where it shows.
	if (!v.project.refused.empty()) {
		ImGui::Spacing();
		ui_kit::refusal_text(v.project.refused);
	}
	ImGui::Spacing();
	const bool beside = width >= em * 56.0f;
	if (beside && ImGui::BeginTable("welcome", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX,
	                                ImVec2(width, 0.0f))) {
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		draw_recent(workspace);
		draw_from_store(workspace);
		ImGui::TableNextColumn();
		ImGui::SeparatorText("New project");
		// A field leaves room for its label and its Browse... beside it.
		ImGui::PushItemWidth(-(ui_kit::text_width("Base game's project") + ui_kit::button_width("Browse...") +
		                       ImGui::GetStyle().ItemSpacing.x * 3.0f));
		form.draw(workspace);
		ImGui::PopItemWidth();
		ImGui::EndTable();
	} else {
		ImGui::SeparatorText("New project");
		form.draw(workspace);
		ImGui::Spacing();
		draw_recent(workspace);
		draw_from_store(workspace);
	}
	if (!v.activity.status.empty() && v.activity.status != v.project.refused) {
		ImGui::Spacing();
		ui_kit::clipped_text(v.activity.status);
	}
	ImGui::EndGroup();
}

} // namespace opennova::editor
