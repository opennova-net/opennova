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

namespace {

// One of NovaWorld's retail games or expansions, as A game you own offers it: its project's key and name, the
// gameprofile code it runs as, an expansion's code and its base game's key ("" both for a game).
struct StockChoice {
	std::string key;
	std::string title;
	std::string game;
	std::string code;
	std::string base;
};

// Whether the projects list holds `key` (one the account may not read is never listed).
const RemoteProject *listed_project(const NovaWorldView &novaworld, const std::string &key) {
	for (const RemoteProject &project : novaworld.projects)
		if (project.key == key) return &project;
	return nullptr;
}

// What A game you own offers, NovaWorld's own (ADR 0074): the projects list's retail games and expansions (an
// admin's view), and the games a mod may be of that the account may not read (the retail games, private: a
// player's view); each game, then its expansions.
std::vector<StockChoice> stock_choices(const NovaWorldView &novaworld) {
	std::vector<StockChoice> games, expansions;
	for (const RemoteProject &project : novaworld.projects) {
		if (!project.retail) continue;
		const bool expansion = project.kind == "expansion";
		(expansion ? expansions : games)
		        .push_back({ project.key, project.title.empty() ? project.key : project.title, project.game,
		                     expansion ? project.expansion_code : std::string(), expansion ? project.base : std::string() });
	}
	for (const RemoteGame &game : novaworld.games)
		if (!listed_project(novaworld, game.key))
			games.push_back({ game.key, game.title.empty() ? game.key : game.title, game.game, std::string(), std::string() });
	std::vector<StockChoice> out;
	for (const StockChoice &game : games) {
		out.push_back(game);
		for (const StockChoice &expansion : expansions)
			if (expansion.base == game.key) out.push_back(expansion);
	}
	for (const StockChoice &expansion : expansions)
		if (std::none_of(games.begin(), games.end(), [&](const StockChoice &game) { return game.key == expansion.base; }))
			out.push_back(expansion);
	return out;
}

const StockChoice *stock_choice(const std::vector<StockChoice> &choices, const std::string &key) {
	for (const StockChoice &choice : choices)
		if (choice.key == key) return &choice;
	return nullptr;
}

const RemoteGame *novaworld_game(const NovaWorldView &novaworld, const std::string &key) {
	for (const RemoteGame &game : novaworld.games)
		if (game.key == key) return &game;
	return nullptr;
}

// A game's name by its gameprofile code (Joint Operations), JO's for none.
std::string game_name(const std::string &code) {
	const gameprofile::GameProfile *profile = gameprofile::gameprofile_by_code(code.empty() ? kDefaultTargetGame : code.c_str());
	return profile && profile->display_name ? profile->display_name : code;
}

// A NovaWorld project or game in a list: its name, then its key.
std::string key_words(const std::string &title, const std::string &key) {
	return title.empty() || title == key ? key : title + " (" + key + ")";
}

// Whether a mod of NovaWorld's game `key` is made on a project imported from the game install, as the session's
// make_base takes it: a retail game's (its files the install's), or one the account may not read (never listed).
bool base_from_install(const NovaWorldView &novaworld, const std::string &key) {
	const RemoteProject *project = listed_project(novaworld, key);
	return !project || project->retail;
}

// A new game's title: the one chosen, else NovaWorld's first.
std::string new_game_title(const WorkspaceView::NewProject &form, const NovaWorldView &novaworld) {
	return !form.game.empty() || novaworld.titles.empty() ? form.game : novaworld.titles.front();
}

// What the form's choice comes to: the game install it needs (`install_game` its gameprofile code, "" none: a mod
// of a game NovaWorld gives, a new game; `install_expansion` an installed expansion it must hold), what it makes in
// a line (a Set up...'s summary), and why it cannot be made yet ("" it can).
struct Chosen {
	std::string install_game;
	std::string install_expansion;
	std::string words;
	std::string why;
};

Chosen chosen_of(const WorkspaceView::NewProject &form, const NovaWorldView &novaworld,
                 const std::vector<StockChoice> &stock) {
	using Making = WorkspaceView::Making;
	Chosen out;
	switch (form.making) {
	case Making::Stock: {
		const StockChoice *choice = stock_choice(stock, form.stock);
		if (!choice) {
			out.why = stock.empty() ? "NovaWorld lists no retail game yet: List projects in the NovaWorld window."
			                        : "Choose the game.";
			break;
		}
		out.install_game = choice->game;
		out.install_expansion = choice->code;
		out.words = "A game you own: " + key_words(choice->title, choice->key);
		break;
	}
	case Making::Mod: {
		const RemoteGame *of = novaworld_game(novaworld, form.base_key);
		if (!of) {
			out.why = novaworld.games.empty() ? "NovaWorld lists no game yet: List projects in the NovaWorld window."
			                                  : "Choose the game it is a mod of.";
			break;
		}
		if (base_from_install(novaworld, of->key)) out.install_game = of->game;
		out.words = "A mod of " + key_words(of->title, of->key) + ", as " + form.expansion + ", named " + form.title;
		break;
	}
	case Making::Game: {
		const std::string title = new_game_title(form, novaworld);
		if (title.empty()) {
			out.why = "NovaWorld lists no title yet: List projects in the NovaWorld window.";
			break;
		}
		out.words = "A game for " + game_name(title) + ", named " + form.title;
		break;
	}
	}
	return out;
}

// What A game you own, a mod and a new game each are, the form's first question.
struct MakingRow {
	WorkspaceView::Making making;
	const char *label;
	const char *tip;
};
constexpr MakingRow kMakings[] = {
	{ WorkspaceView::Making::Stock, "A game you own",
	  "A retail game, or one of its expansions, as NovaWorld lists them: every file of it imported from your game "
	  "install into the project, the game's own files as it ships them." },
	{ WorkspaceView::Making::Mod, "A mod",
	  "A change to one of NovaWorld's games: its own files over the game's, shipped as expansion\\<name>\\ and played "
	  "with /exp <name>." },
	{ WorkspaceView::Making::Game, "A new game", "A game of its own for one of NovaWorld's titles, standalone: you bring "
	                                             "in the files it starts from." },
};

} // namespace

EditorRequest new_project_request(const WorkspaceView::NewProject &form, const NovaWorldView &novaworld,
                                  const std::string &install) {
	using Making = WorkspaceView::Making;
	EditorRequest made;
	switch (form.making) {
	case Making::Stock: {
		const std::vector<StockChoice> stock = stock_choices(novaworld);
		const StockChoice *choice = stock_choice(stock, form.stock);
		if (!choice) break;
		// An expansion is made on its base game's project, which the session finds or makes first (base_key).
		made = request::new_stock_project(form.dir, choice->title, install, choice->code);
		made.game = choice->game;
		made.base_key = choice->code.empty() ? std::string() : choice->base;
		break;
	}
	case Making::Mod:
		// A mod of a NovaWorld game (ADR 0074): the session finds its base game's project, or makes it, first.
		made = request::new_expansion_project(form.dir, form.title, form.expansion);
		made.base_key = form.base_key;
		if (base_from_install(novaworld, form.base_key)) made.game_install = install;
		break;
	case Making::Game:
		made = request::new_project(form.dir, form.title);
		made.game = new_game_title(form, novaworld);
		break;
	}
	made.project_key = form.novaworld; // the NovaWorld project it is set up for (Set up..., ADR 0069)
	return made;
}

bool NewProjectForm::draw(Workspace &workspace) {
	using Making = WorkspaceView::Making;
	const SessionView &v = workspace.view();
	const WorkspaceView::NewProject &form = v.workspace.new_project;
	const NovaWorldView &novaworld = v.novaworld;
	const auto set = [&workspace](const char *member, io::JsonValue value) {
		window_requests::set_workspace(workspace, "new_project", member, std::move(value));
	};
	// A new project is refused while an operation that cannot be cancelled runs: its Browse... and
	// Create wait with it (the busy gate's answer, SessionView::allows).
	const bool allowed = v.allows(EditorRequestKind::NewProject);
	title_.follow(form.title);
	folder_.follow(form.dir);
	name_.follow(form.expansion);
	// The install, the editor's own until the author names one (never another project's: an open project's
	// install may be its own, or a session's); checked once it is not being typed.
	install_.follow(form.install_named ? form.game_install : v.project.editor_install);
	// Set up for a NovaWorld project (the projects list's Set up...): said, with its clear; what that project
	// says (its kind, its game, its name and short name) is not asked again.
	const bool set_up = !form.novaworld.empty();
	if (set_up) {
		ui_kit::clipped_text("For NovaWorld project " + form.novaworld,
		                     "Create writes " + form.novaworld + " in the project's file: the folder is then its "
		                     "working copy, ready to push.");
		ImGui::SameLine();
		if (ImGui::SmallButton("x##novaworld")) set("novaworld", io::JsonValue::make_string(""));
		ui_kit::tooltip("Makes the project for no NovaWorld project: every field the form's again.");
	} else {
		// What it makes, the form's first question.
		for (size_t i = 0; i < std::size(kMakings); ++i) {
			if (i) ImGui::SameLine();
			if (ImGui::RadioButton(kMakings[i].label, form.making == kMakings[i].making) && form.making != kMakings[i].making)
				set("making", io::JsonValue::make_string(new_project_making_token(kMakings[i].making)));
			ui_kit::tooltip(kMakings[i].tip);
		}
	}
	const std::vector<StockChoice> stock = stock_choices(novaworld);
	const Chosen chosen = chosen_of(form, novaworld, stock);
	std::string why; // why Create waits ("" it does not)
	const auto name_field = [&] {
		if (ImGui::InputText("Name", title_.text, sizeof(title_.text))) set("title", io::JsonValue::make_string(title_.sent()));
	};
	// What it is of is NovaWorld's (ADR 0074): signed out, nothing to choose from.
	if (!novaworld.available) {
		why = "This editor reaches no NovaWorld service: a new project is of one of its games.";
		ui_kit::refusal_text(why);
	} else if (!novaworld.signed_in) {
		ui_kit::quiet_text("Sign in to NovaWorld to make a project: its games are NovaWorld's.");
		ImGui::SameLine();
		if (ImGui::SmallButton("Sign in...##new_project")) window_requests::focus(workspace, "novaworld");
		ui_kit::tooltip("Shows the NovaWorld window, where you sign in.");
		why = "Sign in to NovaWorld first.";
	} else if (set_up) {
		ui_kit::clipped_text(chosen.why.empty() ? chosen.words : chosen.why);
		why = chosen.why;
	} else {
		switch (form.making) {
		case Making::Stock: {
			const StockChoice *choice = stock_choice(stock, form.stock);
			if (ImGui::BeginCombo("Game", choice ? key_words(choice->title, choice->key).c_str()
			                              : stock.empty() ? "No retail game listed" : "Choose the game")) {
				for (const StockChoice &each : stock) {
					const std::string words = (each.code.empty() ? "" : "  ") + key_words(each.title, each.key);
					if (ImGui::Selectable(words.c_str(), &each == choice) && &each != choice)
						set("stock", io::JsonValue::make_string(each.key));
				}
				ImGui::EndCombo();
			}
			ui_kit::tooltip("A retail game, or one of its expansions, as NovaWorld lists them. An expansion is made on its "
			                "base game's project: the one on this computer, or one Create makes beside it first.");
			break;
		}
		case Making::Mod: {
			const RemoteGame *of = novaworld_game(novaworld, form.base_key);
			if (ImGui::BeginCombo("A mod of", of ? key_words(of->title, of->key).c_str()
			                                  : novaworld.games.empty() ? "No game listed" : "Choose its game")) {
				for (const RemoteGame &game : novaworld.games)
					if (ImGui::Selectable(key_words(game.title, game.key).c_str(), &game == of) && &game != of)
						set("base_key", io::JsonValue::make_string(game.key));
				ImGui::EndCombo();
			}
			ui_kit::tooltip("The NovaWorld game the mod changes. Create makes the mod on its project: the one on this "
			                "computer, or one made beside the mod first (a retail game's imported from your game install, "
			                "any other got from NovaWorld).");
			name_field();
			if (ImGui::InputText("Short name", name_.text, sizeof(name_.text))) set("expansion", io::JsonValue::make_string(name_.sent()));
			ui_kit::tooltip("The mod's folder, expansion\\<name>\\, and what a player types to play it: /exp <name>.");
			break;
		}
		case Making::Game: {
			const std::string title = new_game_title(form, novaworld);
			if (ImGui::BeginCombo("A game for", title.empty() ? "No title listed" : game_name(title).c_str())) {
				for (const std::string &each : novaworld.titles)
					if (ImGui::Selectable(game_name(each).c_str(), each == title) && each != title)
						set("game", io::JsonValue::make_string(each));
				ImGui::EndCombo();
			}
			ui_kit::tooltip("The game it runs as: its menus, its wire and its updater are that game's.");
			name_field();
			break;
		}
		}
		why = chosen.why;
	}
	// A mod's short name, as the game's rule takes it (its own form's, or one a set up project names).
	if (why.empty() && form.making == Making::Mod) {
		const std::string problem = expansion_name_problem(form.expansion, ExpansionNameUse::Own);
		if (!problem.empty()) {
			ui_kit::refusal_text(problem);
			why = problem;
		}
	}
	// The game install, asked only where the game needs one (a game you own's import reads it; a mod of a retail
	// game is made over its files): the editor's own said in a line where it is that game's, else the field.
	bool install_ok = true;
	if (why.empty() && !chosen.install_game.empty()) {
		const std::string needed = "a " + game_name(chosen.install_game) + " install" +
		                           (chosen.install_expansion.empty() ? std::string() : " with " + chosen.install_expansion);
		const auto fits = [&](const InstallCheck *check) {
			return check && check->ok() && check->game == chosen.install_game &&
			       (chosen.install_expansion.empty() ||
			        std::any_of(check->expansions.begin(), check->expansions.end(), [&](const InstallCheck::Expansion &each) {
				        return strutil::iequals(each.name, chosen.install_expansion);
			        }));
		};
		const InstallCheck *known = check_.answer(install_.text);
		bool found = false;
		if (!form.install_named && !changing_install_ && install_.text[0] && (!known || fits(known))) {
			const std::string words = check_.words(workspace, install_.text, false, v.project.editor_install, found);
			const InstallCheck *check = found ? check_.answer(install_.text) : nullptr;
			if (!check) ui_kit::quiet_text(words.empty() ? "Checking your game install..." : words);
			else ui_kit::clipped_text("Uses your " + game_name(chosen.install_game) + " install, " + std::string(install_.text) + ".");
			ImGui::SameLine();
			if (ImGui::SmallButton("Change...##install")) changing_install_ = true;
			ui_kit::tooltip("Name another game install.");
			install_ok = fits(check);
			if (!install_ok) why = "The game install is being checked.";
		} else {
			const ui_kit::PathField field = ui_kit::path_field("Game install", install_.text, sizeof(install_.text),
			                                                   "Browse...##install", 0.0f, allowed);
			if (field.edited) set("game_install", io::JsonValue::make_string(install_.sent()));
			if (field.browse) workspace.request(request::pick_directory(PickPurpose::NewProjectInstall));
			ui_kit::tooltip("The game's folder: the project imports its files from it and plays in it. Kept with the project.");
			const std::string words = check_.words(workspace, install_.text, field.typing, v.project.editor_install, found);
			if (install_.text[0]) draw_install_line(words, found, true);
			install_ok = fits(found ? check_.answer(install_.text) : nullptr);
			if (!install_ok) {
				why = "Needs " + needed + ": name its folder.";
				if (!words.empty()) ui_kit::refusal_text(why);
			}
		}
	}
	const ui_kit::PathField folder = ui_kit::path_field("Folder", folder_.text, sizeof(folder_.text), "Browse...##folder", 0.0f, allowed);
	if (folder.edited) set("dir", io::JsonValue::make_string(folder_.sent()));
	if (folder.browse) workspace.request(request::pick_directory(PickPurpose::NewProjectLocation));
	ui_kit::tooltip("The folder is made if it does not exist; it must not hold a project already.");
	if (why.empty() && form.making != Making::Stock && form.title.empty()) why = "Name it first.";
	if (why.empty() && form.dir.empty()) why = "Choose its folder first.";
	// Create raises what the session holds of the form (what a client set, whole), not the fields' copies.
	const std::string install = form.install_named ? form.game_install : v.project.editor_install;
	const bool ready = allowed && why.empty() && install_ok;
	ImGui::BeginDisabled(!ready);
	const bool create = ImGui::Button("Create project") && ready;
	ImGui::EndDisabled();
	if (!why.empty()) ui_kit::tooltip(why);
	else if (form.making == Making::Stock)
		ui_kit::tooltip("Makes the project and imports every file of the game from your game install into it, the "
		                "game's own files as it ships them.");
	if (create) workspace.request(new_project_request(form, novaworld, install));
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
		ImGui::PushItemWidth(-(ui_kit::text_width("Game install") + ui_kit::button_width("Browse...") +
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
