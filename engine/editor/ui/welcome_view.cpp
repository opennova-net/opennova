#include <editor/ui/welcome_view.h>

#include <algorithm>
#include <cstring>

#include <editor/project/local_settings.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/ui_kit.h>

#include <imgui.h>

namespace opennova::editor {

namespace {

const ImVec4 kFoundColor(0.55f, 0.85f, 0.55f, 1.0f);
const ImVec4 kWrongColor(0.95f, 0.55f, 0.45f, 1.0f);

template <size_t N> void copy_into(char (&buffer)[N], const std::string &text) {
	const size_t n = std::min(text.size(), N - 1);
	std::memcpy(buffer, text.data(), n);
	buffer[n] = '\0';
}

// A path field with its label and Browse... beside it, under it when the row is narrow: whether Browse...
// was pressed, the text edited this frame, and the field being typed in.
struct PathField {
	bool browse = false;
	bool edited = false;
	bool typing = false;
};
PathField path_field(const char *label, char *buffer, size_t size, const char *browse, bool enabled) {
	PathField out;
	ui_kit::WrapRow row;
	row.next(ui_kit::field_width(ImGui::CalcItemWidth(), label));
	ImGui::InputText(label, buffer, size);
	out.edited = ImGui::IsItemEdited();
	out.typing = ImGui::IsItemActive();
	row.next(ui_kit::button_width(browse));
	ImGui::BeginDisabled(!enabled);
	out.browse = ImGui::Button(browse) && enabled;
	ImGui::EndDisabled();
	return out;
}

// The check's line under an install field: found in green, no game in red, none named (or not checked yet)
// muted.
void install_line(const std::string &words, bool ok, bool named) {
	if (words.empty()) return;
	ImGui::PushStyleColor(ImGuiCol_Text, !named ? ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled) : ok ? kFoundColor : kWrongColor);
	ImGui::TextWrapped("%s", words.c_str());
	ImGui::PopStyleColor();
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

void draw_recent(Workspace &workspace) {
	const SessionView &v = workspace.view();
	const bool opens = v.allows(EditorRequestKind::OpenProject);
	ImGui::SeparatorText("Open a project");
	ImGui::BeginDisabled(!opens);
	if (ImGui::Button("Open a project folder...") && opens)
		workspace.request(request::pick_directory(PickPurpose::OpenProject));
	ImGui::EndDisabled();
	if (v.project.recent_projects.empty()) {
		ImGui::TextDisabled("The projects you open are listed here.");
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
		return fallback.empty() ? std::string("No game install chosen: the project can still take files from the disk.")
		                        : "Left empty, the project takes the editor's game install, " + fallback + ".";
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

const InstallCheck *InstallFieldCheck::answer(const std::string &path) const {
	return !path.empty() && kept_.root == absolute_install_path(path) ? &kept_ : nullptr;
}

void InstallFieldCheck::forget() {
	asked_.clear();
	slot_when_asked_.clear();
	kept_ = InstallCheck();
	retries_ = 0;
}

bool NewProjectForm::draw(Workspace &workspace) {
	const SessionView &v = workspace.view();
	// A new project is refused while an operation that cannot be cancelled runs: its Browse... and
	// Create wait with it (the busy gate's answer, SessionView::allows).
	const bool allowed = v.allows(EditorRequestKind::NewProject);
	ImGui::InputText("Name", title_, sizeof(title_));
	if (path_field("Folder", folder_, sizeof(folder_), "Browse...##folder", allowed).browse)
		workspace.request(request::pick_directory(PickPurpose::NewProjectLocation));
	ImGui::PushTextWrapPos(0.0f);
	ImGui::TextDisabled("The folder is made if it does not exist; it must not hold a project already.");
	ImGui::PopTextWrapPos();
	// The install, the editor's own until the author names one (never another project's: an open project's
	// install may be its own, or a session's); checked once it is not being typed.
	if (!install_named_) copy_into(install_, v.project.editor_install);
	const PathField field = path_field("Game install", install_, sizeof(install_), "Browse...##install", allowed);
	if (field.edited) install_named_ = true;
	if (field.browse) workspace.request(request::pick_directory(PickPurpose::NewProjectInstall));
	ui_kit::tooltip("The game's folder: the project imports its files from it and plays in it. Kept with the project.");
	bool found = false;
	const std::string words = check_.words(workspace, install_, field.typing, v.project.editor_install, found);
	install_line(words.empty() ? std::string("Checking the folder...") : words, found, install_[0] != '\0' && !words.empty());
	// What it builds on: the expansions of the install named, once it is checked.
	std::vector<ProjectView::InstallExpansion> installed;
	if (const InstallCheck *check = found ? check_.answer(install_) : nullptr)
		for (const InstallCheck::Expansion &each : check->expansions) installed.push_back({ each.name, each.title, std::string() });
	const bool expansion_ok = expansion_.draw(install_[0] ? installed : v.project.new_project_expansions);
	// A folder named that holds no install refuses the project (the session says why, as it checks the
	// folder again); none at all is a project of the files from the disk; one not checked yet is the session's
	// to check.
	const bool install_ok = install_[0] == '\0' || found || words.empty();
	const bool ready = title_[0] != '\0' && folder_[0] != '\0' && allowed && expansion_ok && install_ok;
	ImGui::BeginDisabled(!ready);
	const bool create = ImGui::Button("Create project") && ready;
	ImGui::EndDisabled();
	if (!install_ok && !words.empty()) ui_kit::tooltip("Name a folder that holds the game, or none.");
	if (create) {
		const ProjectExpansion expansion = expansion_.value();
		EditorRequest made = expansion.standalone() ? request::new_project(folder_, title_)
		                                            : request::new_expansion_project(folder_, title_, expansion.name,
		                                                                             expansion.builds_on);
		made.game_install = install_;
		workspace.request(std::move(made));
	}
	return create;
}

void NewProjectForm::set_folder(const std::string &path) { copy_into(folder_, path); }

void NewProjectForm::set_install(const std::string &path) {
	copy_into(install_, path);
	install_named_ = true;
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
	ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
	ImGui::Dummy(ImVec2(width, em * 0.5f));
	ImGui::SetWindowFontScale(1.6f);
	ImGui::TextUnformatted("OpenNova Editor");
	ImGui::SetWindowFontScale(1.0f);
	ImGui::TextDisabled("A project is your mod's own files: import the game's, change them, build, and play.");
	// Why the last New project or Open was refused, in its own words: Output and Problems stand aside while no
	// project is open, so this is where it shows.
	if (!v.project.refused.empty()) {
		ImGui::Spacing();
		ImGui::PushStyleColor(ImGuiCol_Text, kWrongColor);
		ImGui::TextWrapped("%s", v.project.refused.c_str());
		ImGui::PopStyleColor();
	}
	ImGui::PopTextWrapPos();
	ImGui::Spacing();
	const bool beside = width >= em * 56.0f;
	if (beside && ImGui::BeginTable("welcome", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX,
	                                ImVec2(width, 0.0f))) {
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		draw_recent(workspace);
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
	}
	if (!v.activity.status.empty() && v.activity.status != v.project.refused) {
		ImGui::Spacing();
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
		ImGui::TextWrapped("%s", v.activity.status.c_str());
		ImGui::PopTextWrapPos();
	}
	ImGui::EndGroup();
}

} // namespace opennova::editor
