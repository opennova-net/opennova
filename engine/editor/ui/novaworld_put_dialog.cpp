#include <editor/ui/novaworld_put_dialog.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include <base/gameprofile/gameprofile.h>
#include <base/io/strutil.h>
#include <editor/project/project_document.h>
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

// The empty projects the account may push (no revision yet, its can_push), as listed.
std::vector<const RemoteProject *> empty_projects(const NovaWorldView &novaworld) {
	std::vector<const RemoteProject *> out;
	for (const RemoteProject &project : novaworld.projects)
		if (project.head == 0 && project.can_push) out.push_back(&project);
	return out;
}

// A project's words in a list: its title, then its key.
std::string project_words(const RemoteProject &project) {
	return project.title.empty() || project.title == project.key ? project.key : project.title + " (" + project.key + ")";
}

} // namespace

std::string novaworld_key_for(const std::string &title) {
	std::string key;
	for (const char c : title) {
		if (key.size() == 16) break;
		if (c >= 'a' && c <= 'z') key += char(c - 'a' + 'A');
		else if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) key += c;
	}
	return key;
}

void NovaWorldPutDialog::fill(const SessionView &view) {
	const std::string title = view.project.document ? view.project.document->title : std::string();
	const std::string game = view.project.document ? view.project.document->target_game : std::string();
	copy_text(title_, sizeof(title_), title);
	copy_text(key_, sizeof(key_), novaworld_key_for(title));
	key_typed_ = false;
	public_ = false;
	base_.clear();
	// The likeliest empty project: its key or its name the project's, else one of the project's game.
	const std::vector<const RemoteProject *> empty = empty_projects(view.novaworld);
	const RemoteProject *likely = nullptr;
	for (const RemoteProject *project : empty)
		if (project->key == key_ || strutil::iequals(project->title, title)) likely = project;
	for (const RemoteProject *project : empty)
		if (!likely && project->game == game) likely = project;
	if (!likely && !empty.empty()) likely = empty.front();
	empty_ = likely ? likely->key : std::string();
	// A new project of the account's unless there is an empty one it is likely for.
	create_ = !likely || (likely->key != key_ && !strutil::iequals(likely->title, title));
}

void NovaWorldPutDialog::draw(Workspace &workspace) {
	const SessionView &v = workspace.view();
	const NovaWorldView &c = v.novaworld;
	if (opening_) {
		opening_ = false;
		fill(v);
		ImGui::OpenPopup(kTitle);
	}
	if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
	// The project it was for closed, or put there already (another client's put).
	if (!v.project.open || !c.key.empty()) {
		ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		return;
	}
	if (ImGui::RadioButton("A new project of yours", create_)) create_ = true;
	ui_kit::tooltip("Makes a project of your account's on NovaWorld for this one.");
	ImGui::SameLine();
	if (ImGui::RadioButton("An empty project you may push", !create_)) create_ = false;
	ui_kit::tooltip("Takes a project made for you on NovaWorld that has nothing pushed yet.");
	const float width = ImGui::GetFontSize() * 18.0f;
	std::string why; // why Put waits ("" it does not)
	if (create_) {
		ImGui::SetNextItemWidth(width);
		if (ImGui::InputText("Name", title_, sizeof(title_)) && !key_typed_)
			copy_text(key_, sizeof(key_), novaworld_key_for(title_));
		ui_kit::tooltip("The project's name on NovaWorld.");
		ImGui::SetNextItemWidth(width);
		if (ImGui::InputText("Key", key_, sizeof(key_), ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_CharsNoBlank))
			key_typed_ = true;
		ui_kit::tooltip("How NovaWorld and every copy of the project name it: 1 to 16 of A-Z, 0-9 and _.");
		if (ImGui::RadioButton("Private", !public_)) public_ = false;
		ui_kit::tooltip("You and NovaWorld's admins alone see it.");
		ImGui::SameLine();
		if (ImGui::RadioButton("Public", public_)) public_ = true;
		ui_kit::tooltip("Anyone may get it.");
		// What it is, as the session finds it from the project.
		if (c.put.kind == "expansion") {
			const RemoteProject *base = listed(c, c.put.base);
			if (!c.put.base.empty()) {
				ui_kit::clipped_text("A mod of " + (base ? project_words(*base) : c.put.base) + ", as " + c.put.expansion_code);
			} else {
				// No base game found for it: one of NovaWorld's games chosen.
				ImGui::SetNextItemWidth(width);
				const RemoteProject *chosen = listed(c, base_);
				if (ImGui::BeginCombo("A mod of", chosen ? project_words(*chosen).c_str() : "Choose its game")) {
					for (const RemoteProject &project : c.projects)
						if (project.kind == "game" && ImGui::Selectable(project_words(project).c_str(), project.key == base_))
							base_ = project.key;
					ImGui::EndCombo();
				}
				ui_kit::tooltip("The game on NovaWorld the mod changes.");
				if (base_.empty()) why = "Choose the game it changes.";
			}
		} else if (!c.put.kind.empty()) {
			ui_kit::clipped_text("A game of " + game_name(c.put.game));
		}
		if (!c.put.refusal.empty()) {
			ui_kit::refusal_text(c.put.refusal);
			if (why.empty()) why = c.put.refusal;
		}
		if (why.empty() && !title_[0]) why = "Name it first.";
		if (why.empty() && !is_novaworld_key(key_)) why = "Its key is 1 to 16 of A-Z, 0-9 and _.";
	} else {
		const std::vector<const RemoteProject *> empty = empty_projects(c);
		if (empty.empty()) {
			ui_kit::empty_state("No empty project listed.", "An empty project made for your account lists here.");
			why = "No empty project to take.";
		} else {
			ImGui::SetNextItemWidth(width);
			const RemoteProject *chosen = listed(c, empty_);
			if (ImGui::BeginCombo("Project", chosen ? project_words(*chosen).c_str() : "Choose one")) {
				for (const RemoteProject *project : empty)
					if (ImGui::Selectable(project_words(*project).c_str(), project->key == empty_)) empty_ = project->key;
				ImGui::EndCombo();
			}
			ui_kit::tooltip("A project on NovaWorld with nothing pushed yet, which you may push.");
			if (!chosen) why = "Choose the project.";
		}
	}
	const bool allowed = v.allows(EditorRequestKind::NovaWorldPut) && c.signed_in && !c.calling;
	if (why.empty() && !c.signed_in) why = "Sign in to NovaWorld first.";
	const bool ready = allowed && why.empty();
	ImGui::BeginDisabled(!ready);
	const bool put = ImGui::Button("Put") && ready;
	ImGui::EndDisabled();
	ui_kit::tooltip(!why.empty() ? why : "Puts the project on NovaWorld; then push its files as the first revision.");
	if (put) {
		workspace.request(create_ ? request::novaworld_put_new(key_, title_, public_ ? "public" : "private", base_)
		                          : request::novaworld_put_empty(empty_));
		ImGui::CloseCurrentPopup();
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}

} // namespace opennova::editor
