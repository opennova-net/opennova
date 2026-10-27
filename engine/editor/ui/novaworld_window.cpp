#include <editor/ui/novaworld_window.h>

#include <algorithm>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/model/field_text.h>
#include <editor/novaworld/novaworld_account.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/ui_kit.h>
#include <editor/ui/welcome_view.h>

#include <imgui.h>

namespace opennova::editor {

namespace {

bool admin(const NovaWorldView &novaworld) { return novaworld.signed_in && novaworld.role == "admin"; }

// A working copy at no revision of a project that has revisions (session/novaworld_controller.h: a folder whose
// files were not the head's as it was set up): a pull comes first, a push waits for it.
bool holds_no_revision(const NovaWorldView &novaworld) {
	return novaworld.tracked && novaworld.revision == 0 && novaworld.head_known && novaworld.head > 0;
}

// What a side of a conflict did to the file: "modified", "deleted", "added".
const char *change_of(const std::optional<NovaWorldChange> &change) {
	return change ? novaworld_change_name(*change) : "unchanged";
}

void copy_text(char *buffer, size_t size, const std::string &text) {
	const size_t n = std::min(size - 1, text.size());
	std::memcpy(buffer, text.data(), n);
	buffer[n] = '\0';
}

// Set up...: the New project form filled for the empty project `project` (what it makes: a game you own for a
// retail game's, a mod for an expansion's, a new game otherwise; its name; the key its project file takes).
void set_up(Workspace &workspace, const RemoteProject &project) {
	const bool mod = !project.retail && project.kind == "expansion";
	io::JsonValue members = io::JsonValue::make_object();
	members.set("making", io::JsonValue::make_string(project.retail ? "stock" : mod ? "mod" : "game"));
	members.set("title", io::JsonValue::make_string(project.title.empty() ? project.key : project.title));
	// A retail expansion is the install's own (installed); a mod builds as its code on its base game, NovaWorld's.
	members.set("installed", io::JsonValue::make_string(project.retail ? project.expansion_code : std::string()));
	if (mod) {
		members.set("expansion", io::JsonValue::make_string(project.expansion_code));
		members.set("base_key", io::JsonValue::make_string(project.base));
	}
	members.set("novaworld", io::JsonValue::make_string(project.key));
	window_requests::set_workspace(workspace, "new_project", std::move(members));
}

// One group of the projects list (Yours, Everyone's): each by its title, its key and its newest revision, with
// Get... or, for an empty one the account may push, Set up....
void project_rows(Workspace &workspace, const char *table, const std::vector<const RemoteProject *> &projects) {
	const SessionView &v = workspace.view();
	const bool gets = v.allows(EditorRequestKind::NovaWorldGet);
	if (!ImGui::BeginTable(table, 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) return;
	ImGui::TableSetupColumn("Project", ImGuiTableColumnFlags_WidthStretch);
	ImGui::TableSetupColumn("##get", ImGuiTableColumnFlags_WidthFixed);
	for (const RemoteProject *each : projects) {
		const RemoteProject &project = *each;
		ImGui::PushID(project.key.c_str());
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		std::string line = (project.title.empty() ? project.key : project.title) + "  " + project.key;
		line += project.head > 0 ? " r" + std::to_string(project.head) : std::string(", empty");
		if (!project.is_public) line += ", private";
		std::string tip = project.kind == "expansion" && !project.base.empty() ? "A mod of " + project.base + "."
		                                                                       : std::string();
		if (!project.owner.empty()) tip += std::string(tip.empty() ? "" : " ") + project.owner + "'s project.";
		if (!project.is_public) tip += std::string(tip.empty() ? "" : " ") + "Only its owner and admins see it.";
		ui_kit::clipped_text(line, tip.empty() ? std::string() : line + "\n" + tip);
		ImGui::TableNextColumn();
		if (project.head == 0 && project.can_push) {
			const bool sets_up = v.allows(EditorRequestKind::NewProject);
			ImGui::BeginDisabled(!sets_up);
			if (ImGui::SmallButton("Set up...") && sets_up) set_up(workspace, project);
			ImGui::EndDisabled();
			ui_kit::tooltip("Fills New project for " + project.key + ": create it, then push its files.");
		} else {
			const bool can = gets && project.head > 0;
			ImGui::BeginDisabled(!can);
			if (ImGui::SmallButton("Get...") && can) {
				EditorRequest pick = request::pick_directory(PickPurpose::NovaWorldFolder);
				pick.project_key = project.key;
				workspace.request(std::move(pick));
			}
			ImGui::EndDisabled();
			ui_kit::tooltip(project.head > 0 ? "Downloads the project into a folder named " + project.key +
			                                           " inside the one you choose, then opens it."
			                                 : "Nothing has been pushed to it yet.");
		}
		ImGui::PopID();
	}
	ImGui::EndTable();
}

} // namespace

bool NovaWorldSignInForm::draw(Workspace &workspace) {
	const SessionView &v = workspace.view();
	// The address the view keeps, until one is typed here.
	if (!server_edited_ && server_taken_ != v.novaworld.server) {
		copy_text(server_, sizeof(server_), v.novaworld.server);
		server_taken_ = v.novaworld.server;
	}
	if (ImGui::InputText("Server", server_, sizeof(server_))) server_edited_ = true;
	ui_kit::tooltip("The NovaWorld site you sign in to.");
	ImGui::InputText("Account", username_, sizeof(username_));
	ui_kit::tooltip("Your NovaWorld account's name, as you sign in on the site.");
	const bool enter = ImGui::InputText("Password", password_, sizeof(password_),
	                                    ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
	const bool allowed = v.allows(EditorRequestKind::NovaWorldSignIn) && !v.novaworld.calling;
	const bool ready = allowed && server_[0] && username_[0] && password_[0];
	ImGui::BeginDisabled(!ready);
	const bool pressed = ImGui::Button("Sign in") && ready;
	ImGui::EndDisabled();
	ui_kit::tooltip(ready ? "Signs in with your NovaWorld account: you push your own projects and see your private ones."
	                      : "Name the server, your account and its password.");
	if (!(pressed || (enter && ready))) return false;
	workspace.request(request::novaworld_sign_in(server_, username_, password_));
	std::memset(password_, 0, sizeof(password_));
	return true;
}

void draw_novaworld_projects(Workspace &workspace) {
	const SessionView &v = workspace.view();
	const NovaWorldView &c = v.novaworld;
	const bool lists = v.allows(EditorRequestKind::NovaWorldCheck) && !c.calling;
	if (c.projects.empty()) {
		if (c.calling) ui_kit::quiet_text("Listing the projects...");
		else if (!c.projects_error.empty()) ui_kit::refusal_text(c.projects_error);
		else ui_kit::empty_state("No projects listed.", "List asks NovaWorld for its projects.");
		ImGui::BeginDisabled(!lists);
		if (ImGui::Button("List projects") && lists) workspace.request(request::novaworld_check());
		ImGui::EndDisabled();
		ui_kit::tooltip("Asks " + novaworld_host(c.server) + " for the projects you may get.");
		return;
	}
	// Yours: those the account may push (an admin's, every one); Everyone's: the rest it may read.
	std::vector<const RemoteProject *> yours, everyones;
	for (const RemoteProject &project : c.projects) (project.can_push ? yours : everyones).push_back(&project);
	if (!yours.empty()) {
		ImGui::SeparatorText("Yours");
		project_rows(workspace, "novaworld_yours", yours);
	}
	if (!everyones.empty()) {
		ImGui::SeparatorText("Everyone's");
		project_rows(workspace, "novaworld_everyones", everyones);
	}
}

std::string novaworld_status_text(const NovaWorldView &novaworld) {
	if (!novaworld.tracked) return std::string();
	std::string text = novaworld.key + " r" + std::to_string(novaworld.revision);
	if (!novaworld.checked) return text;
	if (!novaworld.changes.empty()) text += ", " + std::to_string(novaworld.changes.size()) + " changed";
	if (novaworld.head_known && novaworld.head > novaworld.revision)
		text += ", " + std::to_string(novaworld.head - novaworld.revision) + " incoming";
	if (!novaworld.conflicts.empty()) text += ", " + counted(novaworld.conflicts.size(), "conflict");
	return text;
}

bool NovaWorldWindow::stands_aside() const { return aside_for_welcome(workspace_.view(), welcome_asked_); }

void NovaWorldWindow::draw_account(const NovaWorldView &c) {
	const SessionView &v = workspace_.view();
	const std::string host = novaworld_host(c.server);
	if (c.signed_in) {
		ui_kit::clipped_text(c.username + (admin(c) ? " (admin)" : "") + " on " + host, c.server);
		signing_in_ = false;
		ui_kit::WrapRow row;
		if (ui_kit::tool(row, "Sign out", v.allows(EditorRequestKind::NovaWorldSignOut),
		                 "Signs out of " + host + ": the public projects stay readable."))
			workspace_.request(request::novaworld_sign_out());
		return;
	}
	ui_kit::quiet_text("Sign in to push your own projects.", c.server);
	ui_kit::WrapRow row;
	if (ui_kit::tool(row, signing_in_ ? "Hide sign-in" : "Sign in...", true,
	                 signing_in_ ? "Hides the sign-in." : "Sign in with your NovaWorld account."))
		signing_in_ = !signing_in_;
	if (signing_in_) sign_in_.draw(workspace_);
	if (c.calling) ui_kit::quiet_text("Asking " + host + "...");
}

void NovaWorldWindow::draw_not_on_novaworld(const NovaWorldView &c) {
	const SessionView &v = workspace_.view();
	ui_kit::empty_state("Not on NovaWorld.", "Put it on NovaWorld to push its files there and get them anywhere.");
	ui_kit::WrapRow row;
	if (ui_kit::tool(row, "Put on NovaWorld...", v.allows(EditorRequestKind::NovaWorldPut) && c.signed_in && !c.calling,
	                 c.signed_in ? "Makes it a project on NovaWorld: a new one of yours, or an empty one you may push."
	                             : "Sign in to put the project on NovaWorld."))
		put_.open();
}

void NovaWorldWindow::draw_setting_up(const NovaWorldView &c) {
	const SessionView &v = workspace_.view();
	if (!c.head_error.empty()) ui_kit::refusal_text(c.head_error);
	else
		ui_kit::quiet_text("Setting up " + c.key + "'s working copy...",
		                   "The folder becomes the working copy of " + c.key + " on " + novaworld_host(c.server) +
		                           ": its files are then pushed and pulled.");
	ui_kit::WrapRow row;
	if (ui_kit::tool(row, "Check", v.allows(EditorRequestKind::NovaWorldCheck),
	                 "Sets the folder up as " + c.key + "'s working copy and looks for what changed."))
		workspace_.request(request::novaworld_check());
}

void NovaWorldWindow::draw_standing(const NovaWorldView &c) {
	const SessionView &v = workspace_.view();
	std::string line = c.key + " r" + std::to_string(c.revision);
	std::string tip = "The revision this folder was last pulled or pushed at.";
	if (c.head_known) line += c.head > c.revision ? ", r" + std::to_string(c.head) + " on NovaWorld" : ", up to date";
	else if (!c.head_error.empty()) {
		line += ", NovaWorld not reached";
		tip = c.head_error;
	}
	ui_kit::clipped_text(line, tip);
	// A folder holding no revision while the project has some (its files were not the head's as it was set up, or
	// the empty project it was put on was pushed to since): a pull first.
	if (holds_no_revision(c))
		ui_kit::warning_line("No revision of " + c.key + " here yet: Pull r" + std::to_string(c.head) + " first.",
		                     "The folder's files differ from r" + std::to_string(c.head) + ": a pull takes its files and "
		                     "stops on each file both hold otherwise, for you to settle. Push waits until then.");
	ui_kit::WrapRow row;
	if (ui_kit::tool(row, "Check", v.allows(EditorRequestKind::NovaWorldCheck),
	                 "Looks for what changed here and on NovaWorld."))
		workspace_.request(request::novaworld_check());
	if (ui_kit::tool(row, "Pull", v.allows(EditorRequestKind::NovaWorldPull) && c.conflicts.empty(),
	                 c.conflicts.empty() ? "Brings in the newest revision on NovaWorld; your own changes stay."
	                                     : "Settle every conflict first."))
		workspace_.request(request::novaworld_pull());

	ImGui::SeparatorText(c.checked ? ("Changes (" + std::to_string(c.changes.size()) + ")").c_str() : "Changes");
	if (!c.checked) {
		ui_kit::empty_state("Not checked yet.", "Check counts what changed here since the revision.");
	} else if (c.changes.empty()) {
		ui_kit::empty_state("No changes.", "What you change in the project's files lists here.");
		message_[0] = '\0'; // nothing left to push: the message went with the last push
	} else {
		// Up to twelve rows high, scrolled past that; each change with its Discard (the base's file put back).
		const float rows = std::min(float(c.changes.size()), 12.0f);
		const bool restores = v.allows(EditorRequestKind::NovaWorldRestore);
		if (ImGui::BeginChild("changes", ImVec2(0.0f, rows * ImGui::GetFrameHeightWithSpacing() + 4.0f)) &&
		    ImGui::BeginTable("changes_table", 2, ImGuiTableFlags_SizingStretchProp)) {
			ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("##discard", ImGuiTableColumnFlags_WidthFixed);
			ImGuiListClipper clipper;
			clipper.Begin(static_cast<int>(c.changes.size()));
			while (clipper.Step())
				for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
					const LocalChange &change = c.changes[size_t(i)];
					ImGui::PushID(change.path.c_str());
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ui_kit::clipped_text(std::string(novaworld_change_name(change.change)) + "  " + change.path);
					ImGui::TableNextColumn();
					ImGui::BeginDisabled(!restores);
					if (ImGui::SmallButton("Discard") && restores)
						workspace_.request(request::novaworld_restore(change.path));
					ImGui::EndDisabled();
					ui_kit::tooltip("Puts back the file as revision " + std::to_string(c.revision) +
					                " has it; yours goes to the project's trash (kept until Empty the trash).");
					ImGui::PopID();
				}
			ImGui::EndTable();
		}
		ImGui::EndChild();
	}
	for (const std::string &problem : c.problems) ui_kit::warning_line(problem);
	draw_push(c);
	draw_release(c);
}

// Push, for an account the service says may push the project (its owner, an admin); a read-only line for one
// it says may not. Before the service answered, neither.
void NovaWorldWindow::draw_push(const NovaWorldView &c) {
	const SessionView &v = workspace_.view();
	if (!c.remote_known) return;
	if (!c.remote.can_push) {
		ui_kit::quiet_text("Read-only: you may pull " + c.key + "; its owner and admins push it.");
		return;
	}
	// The first push's message offered, once a project (its own words typed over it).
	if (c.revision == 0 && !c.changes.empty() && !message_[0] && prefilled_ != c.key) {
		copy_text(message_, sizeof(message_), "First revision");
		prefilled_ = c.key;
	}
	// The message stays while there is a change to push: a push refused (the store moved on) keeps it.
	ImGui::SetNextItemWidth(-1.0f);
	ImGui::InputTextWithHint("##message", "What changed", message_, sizeof(message_));
	const bool pushes = v.allows(EditorRequestKind::NovaWorldPush) && message_[0] && !c.changes.empty() &&
	                    c.conflicts.empty() && !holds_no_revision(c);
	ImGui::BeginDisabled(!pushes);
	const bool pushed = ImGui::Button("Push") && pushes;
	ImGui::EndDisabled();
	const char *why = holds_no_revision(c)   ? "Pull first: the folder holds no revision of the project yet."
	                  : c.changes.empty()    ? "Nothing to push."
	                  : !message_[0]         ? "Say what changed first."
	                  : !c.conflicts.empty() ? "Settle every conflict first."
	                                         : "Sends every change as the project's next revision; players get "
	                                           "it once its owner releases it.";
	ui_kit::tooltip(why);
	if (pushed) workspace_.request(request::novaworld_push(message_));
}

// A release of the revision the folder is at, built on the service (ADR 0069 d7), for an account the service says
// may release the project (its owner, an admin: can_release, ADR 0074): a draft once the service has packed and
// checked it, published on the site (an owner's project page, the admin's Products page). The release players get
// now, above it.
void NovaWorldWindow::draw_release(const NovaWorldView &c) {
	const SessionView &v = workspace_.view();
	if (!c.remote_known || !c.remote.can_release) return;
	ImGui::SeparatorText("Release");
	ui_kit::quiet_text(c.remote.release_id != 0 ? "Players get " + c.remote.release_version + "."
	                                            : "No release published yet.",
	                   "The release published on NovaWorld, which players' updaters install.");
	const std::string publish = admin(c) ? "the admin's Products page" : "its page under Projects on the site";
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
	ImGui::InputTextWithHint("##version", "0.2.0", version_, sizeof(version_));
	ui_kit::tooltip("The release's version: one to four numbers between dots.");
	ImGui::SameLine();
	const bool releases = v.allows(EditorRequestKind::NovaWorldRelease) && version_[0] && c.revision > 0 && !c.calling;
	ImGui::BeginDisabled(!releases);
	const bool released = ImGui::Button(("Release r" + std::to_string(c.revision) + "###release").c_str()) && releases;
	ImGui::EndDisabled();
	ui_kit::tooltip(c.revision <= 0 ? "Push first: a release is built from a revision on NovaWorld."
	                : !version_[0]  ? "Name its version first."
	                                : "Builds a release of r" + std::to_string(c.revision) + " on the service; publish it on " +
	                                          publish + " once it is a draft.");
	if (released) {
		workspace_.request(request::novaworld_release(version_));
		version_[0] = '\0';
	}
}

void NovaWorldWindow::draw_conflicts(const NovaWorldView &c) {
	if (c.conflicts.empty()) return;
	const SessionView &v = workspace_.view();
	ImGui::SeparatorText(("Conflicts (" + std::to_string(c.conflicts.size()) + ")").c_str());
	ui_kit::quiet_text("Both sides changed these: keep yours or take theirs, then Pull.",
	                   "The pull of revision " + std::to_string(c.pull_revision) + " waits for them.");
	const bool resolves = v.allows(EditorRequestKind::NovaWorldResolve);
	if (!ImGui::BeginTable("conflicts", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) return;
	ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch);
	ImGui::TableSetupColumn("##choice", ImGuiTableColumnFlags_WidthFixed);
	for (const NovaWorldConflictView &conflict : c.conflicts) {
		ImGui::PushID(conflict.path.c_str());
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ui_kit::clipped_text(conflict.path, conflict.path + "\nYou: " + change_of(conflict.mine) +
		                                            ". NovaWorld: " + change_of(conflict.theirs) + ".");
		ImGui::TableNextColumn();
		ImGui::BeginDisabled(!resolves);
		if (ImGui::SmallButton("Keep mine") && resolves)
			workspace_.request(request::novaworld_resolve(conflict.path, Resolution::KeepMine));
		ui_kit::tooltip("Keeps your file: your change goes in your next push.");
		ImGui::SameLine();
		if (ImGui::SmallButton("Take theirs") && resolves)
			workspace_.request(request::novaworld_resolve(conflict.path, Resolution::TakeTheirs));
		ui_kit::tooltip("Puts NovaWorld's file in its place; yours goes to the project's trash.");
		ImGui::EndDisabled();
		ImGui::PopID();
	}
	ImGui::EndTable();
}

void NovaWorldWindow::draw_history(const NovaWorldView &c) {
	const SessionView &v = workspace_.view();
	ImGui::SeparatorText("History");
	ui_kit::WrapRow row;
	if (ui_kit::tool(row, c.history.empty() ? "Show history" : "Refresh history",
	                 v.allows(EditorRequestKind::NovaWorldHistory) && !c.calling,
	                 "Lists the project's revisions, newest first."))
		workspace_.request(request::novaworld_history());
	if (!c.history_error.empty()) ui_kit::refusal_text(c.history_error);
	for (const RemoteRevision &revision : c.history) {
		ImGui::PushID(static_cast<int>(revision.number));
		ui_kit::clipped_text("r" + std::to_string(revision.number) + "  " + revision.author + ": " + revision.message,
		                     "r" + std::to_string(revision.number) + " by " + revision.author + ", " +
		                             counted(size_t(revision.file_count), "file") + ", " + revision.created_at + "\n" +
		                             revision.message);
		ImGui::PopID();
	}
}

void NovaWorldWindow::draw(devtools::ImGuiPass &, uint64_t) {
	const SessionView &v = workspace_.view();
	const NovaWorldView &c = v.novaworld;
	if (!c.available) {
		ui_kit::empty_state("No NovaWorld service.", "This editor reaches no NovaWorld service.");
		return;
	}
	draw_account(c);
	ImGui::Separator();
	if (!v.project.open) draw_novaworld_projects(workspace_);
	else if (c.key.empty()) draw_not_on_novaworld(c);
	else if (!c.tracked) draw_setting_up(c);
	else {
		draw_standing(c);
		draw_conflicts(c);
		draw_history(c);
	}
	if (!c.last.empty()) {
		ImGui::Spacing();
		ui_kit::quiet_text(c.last);
	}
	put_.draw(workspace_);
}

} // namespace opennova::editor
