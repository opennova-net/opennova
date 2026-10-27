#include <editor/ui/novaworld_window.h>

#include <algorithm>
#include <cstring>
#include <optional>
#include <string>

#include <editor/model/field_text.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/ui_kit.h>
#include <editor/ui/welcome_view.h>

#include <imgui.h>

namespace opennova::editor {

namespace {

// The service's address as a line says it: its host, without the scheme ("staging.opennova.net").
std::string host_of(const std::string &server) {
	const size_t scheme = server.find("://");
	return scheme == std::string::npos ? server : server.substr(scheme + 3);
}

bool admin(const NovaWorldView &novaworld) { return novaworld.signed_in && novaworld.role == "admin"; }

// What a side of a conflict did to the file: "modified", "deleted", "added".
const char *change_of(const std::optional<NovaWorldChange> &change) {
	return change ? novaworld_change_name(*change) : "unchanged";
}

void copy_text(char *buffer, size_t size, const std::string &text) {
	const size_t n = std::min(size - 1, text.size());
	std::memcpy(buffer, text.data(), n);
	buffer[n] = '\0';
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
	ui_kit::tooltip(ready ? "Signs in with your NovaWorld account: an admin may push and sees the private projects."
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
		ui_kit::tooltip("Asks " + host_of(c.server) + " for the projects you may get.");
		return;
	}
	const bool gets = v.allows(EditorRequestKind::NovaWorldGet);
	if (!ImGui::BeginTable("novaworld_projects", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) return;
	ImGui::TableSetupColumn("Project", ImGuiTableColumnFlags_WidthStretch);
	ImGui::TableSetupColumn("##get", ImGuiTableColumnFlags_WidthFixed);
	for (const RemoteProject &project : c.projects) {
		ImGui::PushID(project.key.c_str());
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		std::string line = (project.title.empty() ? project.key : project.title) + "  " + project.key;
		line += project.head > 0 ? " r" + std::to_string(project.head) : std::string(", empty");
		if (!project.is_public) line += ", private";
		ui_kit::clipped_text(line, project.is_public ? std::string() : line + "\nOnly admins see a private project.");
		ImGui::TableNextColumn();
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
		ImGui::PopID();
	}
	ImGui::EndTable();
}

std::string novaworld_status_text(const NovaWorldView &novaworld) {
	if (!novaworld.bound) return std::string();
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
	if (c.signed_in) {
		ui_kit::clipped_text(c.username + (admin(c) ? " (admin)" : "") + " on " + host_of(c.server), c.server);
		signing_in_ = false;
		ui_kit::WrapRow row;
		if (ui_kit::tool(row, "Sign out", v.allows(EditorRequestKind::NovaWorldSignOut),
		                 "Signs out of " + host_of(c.server) + ": the public projects stay readable."))
			workspace_.request(request::novaworld_sign_out());
		return;
	}
	ui_kit::quiet_text("Not signed in: the public projects alone.", c.server);
	ui_kit::WrapRow row;
	if (ui_kit::tool(row, signing_in_ ? "Hide sign-in" : "Sign in...", true,
	                 signing_in_ ? "Hides the sign-in." : "Sign in with your NovaWorld account to push."))
		signing_in_ = !signing_in_;
	if (signing_in_) sign_in_.draw(workspace_);
	if (c.calling) ui_kit::quiet_text("Asking " + host_of(c.server) + "...");
}

void NovaWorldWindow::draw_binding(const NovaWorldView &c) {
	ui_kit::empty_state("Not on NovaWorld.", "An admin binds the project's folder to a project on NovaWorld; "
	                                          "its files are then pushed and pulled.");
	draw_bind(c);
}

// The bind controls, an admin's alone: an unbound folder's, and a bound one's again (the store
// reset, or the folder moved to another service's project).
void NovaWorldWindow::draw_bind(const NovaWorldView &c) {
	const SessionView &v = workspace_.view();
	if (!admin(c)) return;
	if (bind_key_.empty() && !c.projects.empty()) bind_key_ = c.projects.front().key;
	ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x * 0.6f, ImGui::GetFontSize() * 14.0f));
	if (ImGui::BeginCombo("##bind", bind_key_.empty() ? "No projects listed" : bind_key_.c_str())) {
		for (const RemoteProject &project : c.projects)
			if (ImGui::Selectable(project.key.c_str(), project.key == bind_key_)) bind_key_ = project.key;
		ImGui::EndCombo();
	}
	ui_kit::tooltip("The NovaWorld project this folder is a working copy of.");
	ImGui::SameLine();
	const bool binds = v.allows(EditorRequestKind::NovaWorldBind) && !bind_key_.empty();
	ImGui::BeginDisabled(!binds);
	if (ImGui::Button("Bind") && binds) workspace_.request(request::novaworld_bind(bind_key_));
	ImGui::EndDisabled();
	ui_kit::tooltip("Makes this folder a working copy of " + (bind_key_.empty() ? std::string("the project") : bind_key_) +
	                ": what it changed since the project's newest revision is then its push.");
	if (c.projects.empty()) {
		ImGui::BeginDisabled(!v.allows(EditorRequestKind::NovaWorldCheck) || c.calling);
		if (ImGui::Button("List projects") && v.allows(EditorRequestKind::NovaWorldCheck) && !c.calling)
			workspace_.request(request::novaworld_check());
		ImGui::EndDisabled();
	}
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

	if (!admin(c)) {
		ui_kit::quiet_text("Only an admin pushes.", "Sign in with an admin's account to push your changes.");
		return;
	}
	// The message stays while there is a change to push: a push refused (the store moved on) keeps it.
	ImGui::SetNextItemWidth(-1.0f);
	ImGui::InputTextWithHint("##message", "What changed", message_, sizeof(message_));
	const bool pushes = v.allows(EditorRequestKind::NovaWorldPush) && message_[0] && !c.changes.empty() &&
	                    c.conflicts.empty();
	ImGui::BeginDisabled(!pushes);
	const bool pushed = ImGui::Button("Push") && pushes;
	ImGui::EndDisabled();
	const char *why = c.changes.empty()      ? "Nothing to push."
	                  : !message_[0]         ? "Say what changed first."
	                  : !c.conflicts.empty() ? "Settle every conflict first."
	                                         : "Sends every change as the project's next revision; players get "
	                                           "it only once an admin releases it.";
	ui_kit::tooltip(why);
	if (pushed) workspace_.request(request::novaworld_push(message_));

	// A release of the revision the folder is at, built on the service (ADR 0069 d7): a draft once the
	// service has packed and checked it, published in the web admin's Releases.
	ImGui::SeparatorText("Release");
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
	                                : "Builds a release of r" + std::to_string(c.revision) +
	                                          " on the service; publish it in the web admin's Releases once it is a draft.");
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
	else if (!c.bound) draw_binding(c);
	else {
		draw_standing(c);
		draw_conflicts(c);
		draw_history(c);
		if (admin(c) && ImGui::CollapsingHeader("Bind again")) draw_bind(c);
	}
	if (!c.last.empty()) {
		ImGui::Spacing();
		ui_kit::quiet_text(c.last);
	}
}

} // namespace opennova::editor
