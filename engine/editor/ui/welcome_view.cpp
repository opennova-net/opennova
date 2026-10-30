#include <editor/ui/welcome_view.h>

#include <algorithm>
#include <cstring>
#include <iterator>

#include <base/io/json.h>
#include <base/io/strutil.h>
#include <base/title/title.h>
#include <base/version/version_line.h>
#include <editor/project/expansion_name.h>
#include <editor/project/local_settings.h>
#include <editor/project/project_document.h>
#include <editor/session/novaworld_controller.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/session/workspace_parts.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/novaworld_put_dialog.h>
#include <editor/ui/novaworld_window.h>
#include <editor/ui/ui_kit.h>

#include <net/protocol/protocol.h>

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
		if (!details->novaworld.empty()) what += ", on NovaWorld";
	}
	what += (what.empty() ? "" : "   ") + root;
	ImGui::TextDisabled("%s", ui_kit::fit_middle(what, ImGui::GetContentRegionAvail().x).c_str());
	ImGui::TableNextColumn();
	const bool forgets = v.allows(EditorRequestKind::ForgetRecent);
	ImGui::BeginDisabled(!forgets);
	if (ImGui::SmallButton("Forget") && forgets) workspace.request(request::forget_recent(root));
	ImGui::EndDisabled();
	ui_kit::tooltip(!found ? "Take it off this list."
	                : details && !details->novaworld.empty()
	                        ? "Take it off this list. The folder stays, and the NovaWorld list still opens it."
	                        : "Take it off this list; the project stays where it is.");
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
	draw_novaworld_activity(workspace); // a get, a base game's project made, what NovaWorld is asked
	draw_novaworld_projects(workspace);
	if (v.novaworld.signed_in) return;
	ImGui::SameLine();
	if (ImGui::Button("Sign in...")) window_requests::focus(workspace, "novaworld"); // its form shown at once
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

// Whether the projects list holds `key` (one the account may not read is never listed).
const RemoteProject *listed_project(const NovaWorldView &novaworld, const std::string &key) {
	for (const RemoteProject &project : novaworld.projects)
		if (project.key == key) return &project;
	return nullptr;
}

const RemoteGame *novaworld_game(const NovaWorldView &novaworld, const std::string &key) {
	for (const RemoteGame &game : novaworld.games)
		if (game.key == key) return &game;
	return nullptr;
}

// A game's name by its title code (Joint Operations), the default title's for none.
std::string game_name(const std::string &code) {
	const Title *title = code.empty() ? &default_title() : title_by_code(code);
	return title && title->display_name ? title->display_name : code;
}

// The protocol title `code` speaks, in words ("JO protocol 16"); "" for a title with no complete protocol
// (title_versions).
std::string protocol_line(const std::string &code) {
	Versions versions;
	title_versions(code, versions);
	return versions.protocol.empty() ? std::string() : versions.protocol + " protocol " + versions.protocol_version;
}

// A title as Runs as offers it: "Joint Operations · JO protocol 16", its name alone with no complete protocol.
std::string runs_as_option(const std::string &code) {
	const std::string protocol = protocol_line(code);
	return protocol.empty() ? game_name(code) : game_name(code) + " \xC2\xB7 " + protocol;
}

// A name's possessive: "Delta Force Xtreme 2's", "Joint Operations'".
std::string possessive(const std::string &name) {
	return !name.empty() && (name.back() == 's' || name.back() == 'S') ? name + "'" : name + "'s";
}

// The titles that speak the protocol title `code` speaks (the one Protocol object: OP: Acid Gambit speaks JO's), by
// name in the registry's order, joined "and": "Joint Operations and OP: Acid Gambit"; the title's own name where it
// speaks none.
std::string plays_with(const std::string &code) {
	const Protocol *protocol = title_protocol(code);
	std::vector<std::string> names;
	for (size_t i = 0; protocol && i < title_count(); ++i) {
		const Title *title = title_at(i);
		if (title && title->code && title->display_name && title_protocol(title->code) == protocol)
			names.push_back(title->display_name);
	}
	if (names.empty()) return game_name(code);
	std::string out;
	for (size_t i = 0; i < names.size(); ++i)
		out += (i == 0 ? "" : i + 1 == names.size() ? " and " : ", ") + names[i];
	return out;
}

// A NovaWorld project or game in a list: its name, then its key.
std::string key_words(const std::string &title, const std::string &key) {
	return title.empty() || title == key ? key : title + " (" + key + ")";
}

// Whether a mod of NovaWorld's game `key` is made on a project imported from the game install, as the session's
// make_base takes it: NovaLogic's game's (its files the install's), or one the account may not read
// (never listed).
bool base_from_install(const NovaWorldView &novaworld, const std::string &key) {
	const RemoteProject *project = listed_project(novaworld, key);
	return !project || novalogic_project(*project);
}

// A mod's short name as NovaWorld takes one (the service's expansion code): 1 to 8 of a-z and 0-9.
bool short_name_ok(const std::string &name) {
	return !name.empty() && name.size() <= 8 &&
	       std::all_of(name.begin(), name.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); });
}

// The short name a mod named `title` is offered, as the website offers it: its letters and digits, small, at most 8.
std::string short_name_for(const std::string &title) {
	std::string name;
	for (const char c : title) {
		if (name.size() == 8) break;
		if (c >= 'A' && c <= 'Z') name += char(c - 'A' + 'a');
		else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) name += c;
	}
	return name;
}

// A new game's title: the one chosen, else NovaWorld's first.
std::string new_game_title(const WorkspaceView::NewProject &form, const NovaWorldView &novaworld) {
	return !form.game.empty() || novaworld.titles.empty() ? form.game : novaworld.titles.front();
}

// What the form's choice comes to: the game install it needs (`install_game` its title code, "" none: a mod
// of a game NovaWorld gives, a new game), what it makes in a line (a Set up...'s summary), and why it cannot be
// made yet ("" it can).
struct Chosen {
	std::string install_game;
	std::string words;
	std::string why;
};

Chosen chosen_of(const WorkspaceView::NewProject &form, const NovaWorldView &novaworld) {
	using Making = WorkspaceView::Making;
	Chosen out;
	switch (form.making) {
	case Making::Mod: {
		const RemoteGame *of = novaworld_game(novaworld, form.base_key);
		if (!of) {
			out.why = novaworld.games.empty() ? "NovaWorld lists no game yet: Refresh its list."
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
			out.why = "NovaWorld lists no title yet: Refresh its list.";
			break;
		}
		const std::string protocol = protocol_line(title);
		out.words = form.title + ", a game that runs as " + game_name(title) +
		            (protocol.empty() ? std::string() : " (" + protocol + ")");
		break;
	}
	}
	return out;
}

// What a mod and a game of its own each are, the form's first question.
struct MakingRow {
	WorkspaceView::Making making;
	const char *label;
	const char *tip;
};
constexpr MakingRow kMakings[] = {
	{ WorkspaceView::Making::Mod, "A mod of a game",
	  "A change to one of NovaWorld's games: its own files over the game's, shipped as expansion\\<name>\\ and played "
	  "with /exp <name>. Named as one of your game install's expansions, it is that expansion's project, made from "
	  "the install." },
	{ WorkspaceView::Making::Game, "A game of its own",
	  "A game of its own for one of NovaWorld's titles, standalone: you bring in the files it starts from. A retail "
	  "game's project is one too: then import its whole game install (File > Import...)." },
};

} // namespace

EditorRequest new_project_request(const WorkspaceView::NewProject &form, const NovaWorldView &novaworld,
                                  const std::string &install) {
	using Making = WorkspaceView::Making;
	EditorRequest made;
	switch (form.making) {
	case Making::Mod:
		// A mod of a NovaWorld game (ADR 0074): the session finds its base game's project, or makes it, first (and one
		// named as an installed expansion of a retail game is that expansion's project, made from the install).
		made = request::new_expansion_project(form.dir, form.title, form.expansion);
		made.base_key = form.base_key;
		if (base_from_install(novaworld, form.base_key)) made.game_install = install;
		break;
	case Making::Game:
		made = request::new_project(form.dir, form.title);
		made.game = new_game_title(form, novaworld);
		break;
	}
	if (!form.novaworld.empty()) {
		made.project_key = form.novaworld; // the NovaWorld project it is set up for (Set up..., ADR 0069)
	} else if (form.put && novaworld.signed_in) {
		// Put on NovaWorld as it is made: a new project of the account's (new_project's create).
		made.create = true;
		made.project_key = form.put_key.empty() ? novaworld_key_for(form.title, novaworld.role != "admin") : form.put_key;
		made.visibility = form.put_public ? "public" : "private";
		if (form.making == Making::Game) {
			made.lobby = form.lobby;
			made.new_lobby_name = form.new_lobby_name.empty() ? form.title : form.new_lobby_name;
			if (!made.lobby.empty())
				made.new_lobby_name.clear();
		}
	}
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
		std::string named = form.novaworld;
		for (const RemoteProject &project : novaworld.projects)
			if (project.key == form.novaworld && !project.title.empty()) named = project.title;
		ui_kit::clipped_text("For NovaWorld project " + named,
		                     "Make the project ties the project's file to it: the folder is then its working copy, ready "
		                     "to push.");
		ImGui::SameLine();
		if (ImGui::SmallButton("x##novaworld")) set("novaworld", io::JsonValue::make_string(""));
		ui_kit::tooltip("Makes the project for no NovaWorld project: every field the form's again.");
	} else {
		// What it makes, the form's first question. A game of its own is offered to admins alone for now: a
		// player's lobby has no pages of its own yet.
		const bool games_offered = !novaworld.signed_in || novaworld.role == "admin";
		if (!games_offered && form.making == Making::Game)
			set("making", io::JsonValue::make_string(new_project_making_token(Making::Mod)));
		bool first = true;
		for (const MakingRow &row : kMakings) {
			if (row.making == Making::Game && !games_offered) continue;
			if (!first) ImGui::SameLine();
			first = false;
			if (ImGui::RadioButton(row.label, form.making == row.making) && form.making != row.making)
				set("making", io::JsonValue::make_string(new_project_making_token(row.making)));
			ui_kit::tooltip(row.tip);
		}
	}
	const Chosen chosen = chosen_of(form, novaworld);
	std::string why; // why Create waits ("" it does not)
	const auto name_field = [&] {
		if (!ImGui::InputText("Name", title_.text, sizeof(title_.text))) return false;
		set("title", io::JsonValue::make_string(title_.sent()));
		return true;
	};
	// What it is of is NovaWorld's (ADR 0074): signed out, nothing to choose from.
	if (!novaworld.available) {
		why = "This editor reaches no NovaWorld service: a new project is of one of its games.";
		ui_kit::refusal_text(why);
	} else if (!novaworld.signed_in) {
		ui_kit::quiet_text("Sign in to NovaWorld to make a project: its games are NovaWorld's.");
		ImGui::SameLine();
		if (ImGui::SmallButton("Sign in...##new_project")) {
			if (form.open) set("open", io::JsonValue::make_bool(false)); // the modal out of the sign-in's way
			window_requests::focus(workspace, "novaworld");
		}
		ui_kit::tooltip("Shows the NovaWorld window, where you sign in.");
		why = "Sign in to NovaWorld first.";
	} else if (set_up) {
		ui_kit::clipped_text(chosen.why.empty() ? chosen.words : chosen.why);
		why = chosen.why;
	} else {
		// Nothing listed yet: the lists asked again from here.
		if (novaworld.games.empty() && !novaworld.calling) {
			if (ImGui::SmallButton("Refresh##new_project") && v.allows(EditorRequestKind::NovaWorldCheck))
				workspace.request(request::novaworld_check());
			ui_kit::tooltip("Lists NovaWorld's games again.");
		}
		switch (form.making) {
		case Making::Mod: {
			const RemoteGame *of = novaworld_game(novaworld, form.base_key);
			if (ImGui::BeginCombo("A mod of", of ? key_words(of->title, of->key).c_str()
			                                  : novaworld.games.empty() ? "No game listed" : "Choose its game")) {
				for (const RemoteGame &game : novaworld.games)
					if (ImGui::Selectable(key_words(game.title, game.key).c_str(), &game == of) && &game != of)
						set("base_key", io::JsonValue::make_string(game.key));
				ImGui::EndCombo();
			}
			ui_kit::tooltip("The NovaWorld game the mod changes. Make the project makes the mod on its project: the one on this "
			                "computer, or one made beside the mod first (a retail game's imported from your game install, "
			                "any other got from NovaWorld).");
			// Its short name offered from its name, as the website offers it, until one is typed.
			if (name_field() && !short_typed_)
				set("expansion", io::JsonValue::make_string(short_name_for(title_.sent())));
			if (ImGui::InputText("Short name", name_.text, sizeof(name_.text))) {
				short_typed_ = true;
				set("expansion", io::JsonValue::make_string(name_.sent()));
			}
			ui_kit::tooltip("Up to 8 lowercase letters and digits, used by no other mod: the mod's folder, "
			                "expansion\\<name>\\, and what a player types to play it, /exp <name>.");
			break;
		}
		case Making::Game: {
			const std::string title = new_game_title(form, novaworld);
			if (ImGui::BeginCombo("Runs as", title.empty() ? "No title listed" : runs_as_option(title).c_str())) {
				for (const std::string &each : novaworld.titles)
					if (ImGui::Selectable(runs_as_option(each).c_str(), each == title) && each != title)
						set("game", io::JsonValue::make_string(each));
				ImGui::EndCombo();
			}
			if (!title.empty())
				ui_kit::tooltip("Its rules and its protocol are " + possessive(game_name(title)) + ": it plays with " +
				                plays_with(title) + " players.");
			name_field();
			break;
		}
		}
		why = chosen.why;
	}
	// A mod's short name, as the game's rule takes it (its own form's, or one a set up project names).
	if (why.empty() && form.making == Making::Mod) {
		const std::string problem = !short_name_ok(form.expansion)
		                                    ? std::string("A short name is up to 8 lowercase letters and digits.")
		                                    : expansion_name_problem(form.expansion, ExpansionNameUse::Own);
		if (!problem.empty()) {
			ui_kit::refusal_text(problem);
			why = problem;
		}
	}
	// The game install, asked only where the game needs one (a mod of a retail game is made over its files): the
	// editor's own said in a line where it is that game's, else the field.
	bool install_ok = true;
	if (why.empty() && !chosen.install_game.empty()) {
		const std::string needed = "a " + game_name(chosen.install_game) + " install";
		const auto fits = [&](const InstallCheck *check) {
			return check && check->ok() && check->game == chosen.install_game;
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
	// A short name that is one of the install's own expansions (jox01 on JO's): that expansion's project, made from
	// the install as it ships (the session's new_mod), never a mod standing in for it, and put nowhere (NovaWorld
	// has it already).
	const InstallCheck::Expansion *installed = nullptr;
	if (form.making == Making::Mod && !chosen.install_game.empty() && install_ok)
		if (const InstallCheck *check = check_.answer(install_.text))
			for (const InstallCheck::Expansion &each : check->expansions)
				if (strutil::iequals(each.name, form.expansion)) installed = &each;
	if (installed)
		ui_kit::clipped_text((installed->title.empty() ? installed->name : installed->title) +
		                     " is installed: its project is made from your game install.");
	const ui_kit::PathField folder = ui_kit::path_field("Folder", folder_.text, sizeof(folder_.text), "Browse...##folder", 0.0f, allowed);
	if (folder.edited) set("dir", io::JsonValue::make_string(folder_.sent()));
	if (folder.browse) workspace.request(request::pick_directory(PickPurpose::NewProjectLocation));
	ui_kit::tooltip("The folder is made if it does not exist; it must not hold a project already.");
	// The base game's project a mod is made on, as the session finds it for the form's game and folder (the view's
	// new_base): one on this computer, or where it is made first.
	const std::string base_key = form.making == Making::Mod ? form.base_key : std::string();
	const NovaWorldView::NewBase &base = novaworld.new_base;
	if (novaworld.signed_in && !base_key.empty() && base.key == base_key && base.for_dir == form.dir) {
		const RemoteGame *of = novaworld_game(novaworld, base.key);
		const std::string game = of && !of->title.empty() ? of->title : base.key;
		// A short line (its column holds ~70 characters), the whole sentence in its tooltip.
		ui_kit::clipped_text((base.found ? "Base game's project: " : "Base game's project, made first: ") + base.dir,
		                     base.found ? game + "'s project on this computer, in " + base.dir + ": the mod is made on it."
		                                : game + "'s project is made in " + base.dir + " first, " +
		                                          (base.from_install ? "from your game install." : "got from NovaWorld."));
	}
	if (why.empty() && form.title.empty()) why = "Name it first.";
	if (why.empty() && form.dir.empty()) why = "Choose its folder first.";
	// Put on NovaWorld as it is made (a mod or a new game, signed in, no Set up...'s, no installed expansion's): its
	// key, and a mod's short name, its own from now (ADR 0074), as Put on NovaWorld... puts one.
	if (novaworld.signed_in && !set_up && !installed) {
		bool put = form.put;
		if (ImGui::Checkbox("Put it on NovaWorld", &put)) set("put", io::JsonValue::make_bool(put));
		ui_kit::tooltip("Makes it a project of yours on NovaWorld once it is made, its key and short name yours from now; "
		                "then push its files as the first revision.");
		if (form.put) {
			if (form.making == Making::Game) {
				const std::string game = new_game_title(form, novaworld);
				const RemoteLobby *chosen_lobby = nullptr;
				bool any = false;
				for (const auto &lobby : novaworld.lobbies) {
					any = any || lobby_speaks(lobby, game);
					if (lobby.slug == form.lobby && lobby_speaks(lobby, game))
						chosen_lobby = &lobby;
				}
				if (ImGui::BeginCombo("Lobby",
							chosen_lobby ? chosen_lobby->display_name.c_str()
										 : "Create a separate lobby")) {
					if (ImGui::Selectable("Create a separate lobby", form.lobby.empty()))
						set("lobby", io::json_string(""));
					for (const auto &lobby : novaworld.lobbies)
						if (lobby_speaks(lobby, game) &&
								ImGui::Selectable((lobby.display_name + "##" + lobby.slug).c_str(),
										lobby.slug == form.lobby))
							set("lobby", io::json_string(lobby.slug));
					if (!any)
						ImGui::TextDisabled("No lobbies for this protocol");
					ImGui::EndCombo();
				}
				if (!form.lobby.empty() && !chosen_lobby)
					why = "Choose a lobby for this game's protocol.";
				if (form.lobby.empty()) {
					lobby_name_.follow(form.new_lobby_name);
					if (ImGui::InputTextWithHint("Lobby name", form.title.c_str(), lobby_name_.text,
								sizeof(lobby_name_.text)))
						set("new_lobby_name", io::json_string(lobby_name_.sent()));
				}
				ui_kit::tooltip("Players and hosted servers use this lobby. It can change until "
								"the first release is built.");
			}
			const bool player = novaworld.role != "admin";
			const std::string made_key = novaworld_key_for(form.title, player);
			put_key_.follow(form.put_key);
			ImGui::Indent();
			const char *key_rule = player ? "Up to 11 of A-Z, 0-9 and _, without \"QQ\"." : "Up to 16 of A-Z, 0-9 and _.";
			if (ImGui::TreeNodeEx("Details", ImGuiTreeNodeFlags_NoAutoOpenOnLog)) {
				if (ImGui::InputTextWithHint("Key", made_key.c_str(), put_key_.text, sizeof(put_key_.text),
				                             ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_CharsNoBlank))
					set("put_key", io::JsonValue::make_string(put_key_.sent()));
				ui_kit::tooltip("How NovaWorld and every copy of the project name it (left empty, " + made_key + "). " + key_rule);
				ImGui::TreePop();
			}
			ui_kit::quiet_text("Who gets it");
			ImGui::SameLine();
			if (ImGui::RadioButton("Public", form.put_public) && !form.put_public) set("put_public", io::JsonValue::make_bool(true));
			ui_kit::tooltip("Public: anyone may get it.");
			ImGui::SameLine();
			if (ImGui::RadioButton("Private", !form.put_public) && form.put_public)
				set("put_public", io::JsonValue::make_bool(false));
			ui_kit::tooltip("Private: only you and NovaWorld's admins may get it.");
			ImGui::Unindent();
			const std::string key = form.put_key.empty() ? made_key : form.put_key;
			if (why.empty() && !(player ? novaworld_player_key(key) : is_novaworld_key(key)))
				why = std::string("Its key on NovaWorld: ") + key_rule;
		}
	}
	// Create raises what the session holds of the form (what a client set, whole), not the fields' copies.
	const std::string install = form.install_named ? form.game_install : v.project.editor_install;
	const bool ready = allowed && why.empty() && install_ok;
	ImGui::BeginDisabled(!ready);
	const bool create = ImGui::Button("Make the project") && ready;
	ImGui::EndDisabled();
	if (!why.empty()) {
		ui_kit::tooltip(why);
		ui_kit::quiet_text(why); // what Make the project waits for, under it
	}
	if (create) {
		EditorRequest made = new_project_request(form, novaworld, install);
		// An installed expansion's project is NovaWorld's retail project already: put nowhere as it is made.
		if (installed && made.create) {
			made.create = false;
			made.project_key.clear();
		}
		workspace.request(made);
	}
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
