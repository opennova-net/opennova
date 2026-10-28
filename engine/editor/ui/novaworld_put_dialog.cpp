#include <editor/ui/novaworld_put_dialog.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include <base/gameprofile/gameprofile.h>
#include <base/io/strutil.h>
#include <editor/project/project_document.h>
#include <editor/session/novaworld_controller.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/ui_kit.h>

#include <imgui.h>

namespace opennova::editor {

namespace {

constexpr const char *kTitle = "Put on NovaWorld";

void copy_text(char *buffer, size_t size, const std::string &text) {
	const size_t n = std::min(size - 1, text.size());
	std::memcpy(buffer, text.data(), n);
	buffer[n] = '\0';
}

// A game's name by its gameprofile code ("Joint Operations"), the code for one the editor has no profile of.
std::string game_name(const std::string &code) {
	const gameprofile::GameProfile *profile = gameprofile::gameprofile_by_code(code.c_str());
	return profile && profile->display_name ? profile->display_name : code;
}

// A listed project by its key, null for none.
const RemoteProject *listed(const NovaWorldView &novaworld, const std::string &key) {
	for (const RemoteProject &project : novaworld.projects)
		if (project.key == key) return &project;
	return nullptr;
}

// The empty projects the open project may be put on, as listed: no revision yet, the account may push them, and
// what the project is (novaworld_put_mismatch: a game or a mod as it is, of its game, a mod by its short name).
std::vector<const RemoteProject *> empty_projects(const SessionView &view) {
	std::vector<const RemoteProject *> out;
	if (!view.project.document) return out;
	for (const RemoteProject &project : view.novaworld.projects)
		if (project.head == 0 && project.can_push && novaworld_put_mismatch(*view.project.document, project).empty())
			out.push_back(&project);
	return out;
}

// A project's words in a list: its title, then its key.
std::string project_words(const RemoteProject &project) {
	return project.title.empty() || project.title == project.key ? project.key : project.title + " (" + project.key + ")";
}

// A base game's words by its key: its name, as the games or the projects list it, then its key.
std::string base_words(const NovaWorldView &novaworld, const std::string &key) {
	for (const RemoteGame &game : novaworld.games)
		if (game.key == key) return game.title.empty() || game.title == key ? key : game.title + " (" + key + ")";
	const RemoteProject *project = listed(novaworld, key);
	return project ? project_words(*project) : key;
}

} // namespace

std::string novaworld_key_for(const std::string &title, bool player) {
	const size_t most = player ? kPlayerKeyMost : 16;
	std::string key;
	for (const char c : title) {
		if (key.size() == most) break;
		const char upper = c >= 'a' && c <= 'z' ? char(c - 'a' + 'A') : c;
		if (!((upper >= 'A' && upper <= 'Z') || (upper >= '0' && upper <= '9'))) continue;
		// A player's key holds no "QQ" (the territory substitution): a second Q in a row left out.
		if (player && upper == 'Q' && !key.empty() && key.back() == 'Q') continue;
		key += upper;
	}
	return key;
}

void NovaWorldPutDialog::fill(const SessionView &view) {
	const std::string title = view.project.document ? view.project.document->title : std::string();
	const bool player = !(view.novaworld.signed_in && view.novaworld.role == "admin");
	copy_text(title_, sizeof(title_), title);
	copy_text(key_, sizeof(key_), novaworld_key_for(title, player));
	key_typed_ = false;
	public_ = true;
	base_.clear();
	lobby_.clear();
	lobby_name_[0] = '\0';
	asked_ = false;
	// The likeliest empty project, of those the project may be put on (novaworld_put_mismatch: a game or a mod as
	// it is, of its game, a mod by its short name): its key or its name the project's, else the first. None of
	// another kind, game or short name is chosen.
	const std::string expansion = view.project.document ? view.project.document->expansion.name : std::string();
	const std::vector<const RemoteProject *> matching = empty_projects(view);
	const RemoteProject *likely = nullptr;
	for (const RemoteProject *project : matching)
		if (!likely && (project->key == key_ || strutil::iequals(project->title, title))) likely = project;
	if (!likely && !matching.empty()) likely = matching.front();
	empty_ = likely ? likely->key : std::string();
	// A new project of the account's unless there is an empty one it is likely for: by its key or name, or a mod's
	// of its short name.
	create_ = !likely || (likely->key != key_ && !strutil::iequals(likely->title, title) && expansion.empty());
}

void NovaWorldPutDialog::draw(Workspace &workspace) {
	const SessionView &v = workspace.view();
	const NovaWorldView &c = v.novaworld;
	if (opening_) {
		opening_ = false;
		fill(v);
		ImGui::OpenPopup(kTitle);
		// The lists asked again, and what a put makes of the project found once they are in (the view's put).
		if (v.allows(EditorRequestKind::NovaWorldCheck)) workspace.request(request::novaworld_check());
	}
	if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
	// The project it was for closed, or put there now (its key written: this put's, or another client's).
	if (!v.project.open || !c.key.empty()) {
		ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		return;
	}
	ImGui::Dummy(ImVec2(ImGui::GetFontSize() * 30.0f, 0.0f)); // as wide as its lines need, whatever is offered
	const bool player = !(c.signed_in && c.role == "admin");
	const std::vector<const RemoteProject *> empty = empty_projects(v);
	// An empty project to take only where one the project may be put on is listed.
	if (!empty.empty()) {
		if (ImGui::RadioButton("A new project of yours", create_)) create_ = true;
		ui_kit::tooltip("Makes a project of your account's on NovaWorld for this one.");
		ImGui::SameLine();
		if (ImGui::RadioButton("An empty project you may push", !create_)) create_ = false;
		ui_kit::tooltip("Takes a project made for you on NovaWorld (on the website, or by an admin) that has nothing "
		                "pushed yet.");
	} else {
		create_ = true;
	}
	const float width = ImGui::GetFontSize() * 18.0f;
	std::string why; // why Put waits ("" it does not)
	if (create_) {
		ImGui::SetNextItemWidth(width);
		if (ImGui::InputText("Name", title_, sizeof(title_)) && !key_typed_)
			copy_text(key_, sizeof(key_), novaworld_key_for(title_, player));
		ui_kit::tooltip("The project's name on NovaWorld.");
		ImGui::SetNextItemWidth(width);
		if (ImGui::InputText("Key", key_, sizeof(key_), ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_CharsNoBlank))
			key_typed_ = true;
		const char *key_rule = player ? "Up to 11 of A-Z, 0-9 and _, without \"QQ\"." : "Up to 16 of A-Z, 0-9 and _.";
		ui_kit::tooltip(std::string("How NovaWorld and every copy of the project name it. ") + key_rule);
		// Who gets it, as the website asks it: public first.
		ui_kit::quiet_text("Who gets it");
		if (ImGui::RadioButton("Public", public_)) public_ = true;
		ui_kit::tooltip("Public: anyone may get it.");
		ImGui::SameLine();
		if (ImGui::RadioButton("Private", !public_)) public_ = false;
		ui_kit::tooltip("Private: only you and NovaWorld's admins may get it.");
		// What it is, as the session finds it from the project.
		if (c.put.kind == "expansion") {
			if (!c.put.base.empty()) {
				ui_kit::clipped_text("A mod of " + base_words(c, c.put.base) + ", as " + c.put.expansion_code);
			} else {
				// No base game found for it: one of the games the account may make a mod of chosen (those listed
				// when the service named none).
				std::vector<std::string> games;
				for (const RemoteGame &game : c.games) games.push_back(game.key);
				if (games.empty())
					for (const RemoteProject &project : c.projects)
						if (project.kind == "game") games.push_back(project.key);
				ImGui::SetNextItemWidth(width);
				if (ImGui::BeginCombo("A mod of", base_.empty() ? "Choose its game" : base_words(c, base_).c_str())) {
					for (const std::string &key : games)
						if (ImGui::Selectable(base_words(c, key).c_str(), key == base_)) base_ = key;
					ImGui::EndCombo();
				}
				ui_kit::tooltip("The game on NovaWorld the mod changes.");
				if (base_.empty()) why = "Choose the game it changes.";
			}
		} else if (!c.put.kind.empty()) {
			ui_kit::clipped_text("A game for " + game_name(c.put.game));
			const RemoteLobby *selected = nullptr;
			for (const auto &lobby : c.lobbies)
				if (lobby.slug == lobby_ && lobby.title == c.put.game)
					selected = &lobby;
			if (ImGui::BeginCombo("Lobby",
						selected ? selected->display_name.c_str() : "Create a separate lobby")) {
				if (ImGui::Selectable("Create a separate lobby", lobby_.empty()))
					lobby_.clear();
				for (const auto &lobby : c.lobbies)
					if (lobby.title == c.put.game &&
							ImGui::Selectable((lobby.display_name + "##" + lobby.slug).c_str(),
									lobby.slug == lobby_))
						lobby_ = lobby.slug;
				ImGui::EndCombo();
			}
			if (lobby_.empty())
				ImGui::InputTextWithHint("Lobby name", title_, lobby_name_, sizeof(lobby_name_));
			else if (!selected)
				why = "Choose a compatible lobby.";
		}
		if (!c.put.refusal.empty()) {
			ui_kit::refusal_text(c.put.refusal);
			if (why.empty()) why = c.put.refusal;
		}
		if (why.empty() && !title_[0]) why = "Name it first.";
		if (why.empty() && !(player ? novaworld_player_key(key_) : is_novaworld_key(key_)))
			why = std::string("Its key: ") + key_rule;
	} else {
		ImGui::SetNextItemWidth(width);
		const RemoteProject *chosen = listed(c, empty_);
		if (ImGui::BeginCombo("Project", chosen ? project_words(*chosen).c_str() : "Choose one")) {
			for (const RemoteProject *project : empty)
				if (ImGui::Selectable(project_words(*project).c_str(), project->key == empty_)) empty_ = project->key;
			ImGui::EndCombo();
		}
		ui_kit::tooltip("A project on NovaWorld with nothing pushed yet that this project is (a game or a mod, of its "
		                "game, a mod by its short name), which you may push.");
		if (!chosen) why = "Choose the project.";
	}
	// Asked: it stays open while NovaWorld answers, and says a refusal here (its fields as they were).
	const bool asking = asked_ && c.calling;
	if (asked_ && !c.calling && !c.put_refusal.empty()) {
		ImGui::PushStyleColor(ImGuiCol_Text, ui_kit::refusal_color());
		ui_kit::dialog_text(c.put_refusal);
		ImGui::PopStyleColor();
	}
	if (asking) ui_kit::quiet_text(c.waiting.empty() ? std::string("Asking NovaWorld...") : c.waiting);
	const bool allowed = v.allows(EditorRequestKind::NovaWorldPut) && c.signed_in && !c.calling;
	if (why.empty() && !c.signed_in) why = "Sign in to NovaWorld first.";
	const bool ready = allowed && why.empty();
	ImGui::BeginDisabled(!ready);
	const bool put = ImGui::Button("Put") && ready;
	ImGui::EndDisabled();
	ui_kit::tooltip(!why.empty() ? why : "Puts the project on NovaWorld; then push its files as the first revision.");
	if (put) {
		workspace.request(create_ ? request::novaworld_put_new(key_, title_, public_ ? "public" : "private", base_, c.put.kind == "game" ? lobby_ : std::string(),
								  c.put.kind == "game" && lobby_.empty()
										  ? (lobby_name_[0] ? std::string(lobby_name_)
															: std::string(title_))
										  : std::string())
		                          : request::novaworld_put_empty(empty_));
		asked_ = true;
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}

} // namespace opennova::editor
