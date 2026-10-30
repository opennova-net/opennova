#include <editor/ui/novaworld_window.h>

#include <algorithm>
#include <cstring>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include <base/io/json.h>
#include <base/version/version_line.h>
#include <editor/model/field_text.h>
#include <editor/novaworld/novaworld_account.h>
#include <editor/project/project_document.h>
#include <editor/session/novaworld_standing.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/ui_kit.h>
#include <editor/ui/welcome_view.h>

#include <net/protocol/protocol.h>

#include <imgui.h>

namespace opennova::editor {

namespace {

// A release by its game version and the ONE that built it: "0.2.0, built by ONE 0.0.22" ("0.2.0"
// where the service names no ONE).
std::string release_words(const std::string &version, const std::string &one_version) {
	return one_version.empty() ? version : version + ", built by ONE " + one_version;
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

// Set up...: the New project form filled with all the empty project `project` says (a project made on the website,
// ADR 0074): what it makes (a mod of its base game as its short name, a retail expansion's as its installed
// expansion's, which the session makes from the install; a game of its own of its title, a retail game's too, its
// whole install imported after), its name, the key its project file takes, and its folder, one of the key's name beside
// the folder the form last named, else beside the last project opened; the form then asks only what is left (the
// folder where none is known, the install where its game needs one).
void set_up(Workspace &workspace, const RemoteProject &project) {
	const SessionView &v = workspace.view();
	const bool mod = project.kind == "expansion";
	io::JsonValue members = io::JsonValue::make_object();
	members.set("making", io::JsonValue::make_string(mod ? "mod" : "game"));
	members.set("title", io::JsonValue::make_string(project.title.empty() ? project.key : project.title));
	members.set("base_key", io::JsonValue::make_string(mod ? project.base : std::string()));
	members.set("expansion", io::JsonValue::make_string(mod ? project.expansion_code : std::string()));
	members.set("game", io::JsonValue::make_string(!mod ? project.game : std::string()));
	members.set("novaworld", io::JsonValue::make_string(project.key));
	const std::string beside = !v.workspace.new_project.dir.empty() ? v.workspace.new_project.dir
	                           : !v.project.recent_projects.empty() ? v.project.recent_projects.front()
	                                                                 : std::string();
	if (!beside.empty()) {
		const std::string parent = beside.substr(0, beside.find_last_of("/\\"));
		if (parent != beside) members.set("dir", io::JsonValue::make_string(parent + "/" + project.key));
	}
	window_requests::set_workspace(workspace, "new_project", std::move(members));
}

// This computer's copies of the NovaWorld project `key`, most recently opened first: each a folder its project file
// names the project in, whatever the recent list holds.
std::vector<const ProjectView::RecentProject *> local_copies(const SessionView &v, const std::string &key) {
	std::vector<const ProjectView::RecentProject *> out;
	for (const ProjectView::RecentProject &copy : v.project.working_copies)
		if (copy.found && copy.novaworld == key) out.push_back(&copy);
	return out;
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
		std::string line = novaworld_project_words(v.novaworld, project);
		line += project.head > 0 ? "  r" + std::to_string(project.head) : std::string(", empty");
		if (!project.is_public) line += ", private";
		std::string tip = project.kind == "expansion" && !project.base.empty() ? "A mod of " + project.base + "."
		                                                                       : std::string();
		if (!project.owner.empty()) tip += std::string(tip.empty() ? "" : " ") + project.owner + "'s project.";
		if (!project.is_public) tip += std::string(tip.empty() ? "" : " ") + "Only its owner and admins see it.";
		ui_kit::clipped_text(line, tip.empty() ? std::string() : line + "\n" + tip);
		ImGui::TableNextColumn();
		const std::vector<const ProjectView::RecentProject *> copies = local_copies(v, project.key);
		const bool open_here = v.project.open && std::any_of(copies.begin(), copies.end(), [&v](const auto *copy) {
			return copy->root == v.project.root;
		});
		if (open_here) {
			// The project open now: nothing to open, or to get.
			ImGui::TextDisabled("Open now");
			ui_kit::tooltip("This project is the one open.");
		} else if (!copies.empty()) {
			// A copy of it on this computer already, an empty project's too: opened, not got or set up again.
			const bool opens = v.allows(EditorRequestKind::OpenProject);
			ImGui::BeginDisabled(!opens);
			if (ImGui::SmallButton("Open") && opens) workspace.request(request::open_project(copies.front()->root));
			ImGui::EndDisabled();
			ui_kit::tooltip("Opens your copy in " + copies.front()->root + "." +
			                (copies.size() > 1 ? " You have " + counted(copies.size() - 1, "more") + " here." : ""));
		} else if (project.head == 0 && project.can_push && (project.owner.empty() || project.owner == v.novaworld.username)) {
			const bool sets_up = v.allows(EditorRequestKind::NewProject);
			ImGui::BeginDisabled(!sets_up);
			if (ImGui::SmallButton("Set up...") && sets_up) set_up(workspace, project);
			ImGui::EndDisabled();
			ui_kit::tooltip("Fills New project for it: create it, then push its files.");
		} else {
			const bool can = gets && project.head > 0;
			ImGui::BeginDisabled(!can);
			if (ImGui::SmallButton("Get...") && can) {
				EditorRequest pick = request::pick_directory(PickPurpose::NovaWorldFolder);
				pick.project_key = project.key;
				workspace.request(std::move(pick));
			}
			ImGui::EndDisabled();
			ui_kit::tooltip(project.head > 0 ? std::string("Downloads the project into a folder of its own inside the one "
			                                               "you choose, then opens it") +
			                                           (v.project.open ? " in place of this one." : ".")
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
	ImGui::InputText("Username", username_, sizeof(username_));
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
	ui_kit::WrapRow row;
	if (ui_kit::tool(row, "Refresh", lists, "Lists " + novaworld_host(c.server) + "'s projects again: one made on the "
	                                        "website since shows."))
		workspace.request(request::novaworld_check());
	// By whose they are: the account's own, OpenNova's (no owner: its games and the retail ones), other players'.
	std::vector<const RemoteProject *> yours, opennovas, players;
	for (const RemoteProject &project : c.projects)
		(!project.owner.empty() && project.owner == c.username ? yours
		 : project.owner.empty()                               ? opennovas
		                                                       : players)
		        .push_back(&project);
	if (!yours.empty()) {
		ImGui::SeparatorText("Yours");
		project_rows(workspace, "novaworld_yours", yours);
	}
	if (!opennovas.empty()) {
		ImGui::SeparatorText("OpenNova's");
		project_rows(workspace, "novaworld_opennova", opennovas);
	}
	if (!players.empty()) {
		ImGui::SeparatorText("Players'");
		project_rows(workspace, "novaworld_players", players);
	}
}

bool novaworld_operation(OperationKind kind) {
	switch (kind) {
	case OperationKind::NovaWorldCheck:
	case OperationKind::NovaWorldTrack:
	case OperationKind::NovaWorldPull:
	case OperationKind::NovaWorldPush:
	case OperationKind::BaseSetup: return true;
	default: return false;
	}
}

std::string novaworld_cancel_words(OperationKind kind) {
	switch (kind) {
	case OperationKind::NovaWorldPull:
		return "Stops between files: nothing in the folder changes, and the files downloaded so far are kept, so the "
		       "next pull or get downloads none of them again.";
	case OperationKind::NovaWorldPush:
		return "Stops between files: no revision is made, and the files uploaded so far stay on NovaWorld, so the next "
		       "push sends none of them again.";
	case OperationKind::NovaWorldCheck: return "Stops the check: nothing changes.";
	case OperationKind::NovaWorldTrack:
		return "Stops setting up the working copy: the folder stays as it is, set up again at the next check.";
	case OperationKind::BaseSetup:
		return "Stops making the base game's project: what is made so far stays in its folder.";
	default: return std::string();
	}
}

void draw_novaworld_activity(Workspace &workspace) {
	const SessionView &v = workspace.view();
	const OperationStatus &running = v.activity.operation;
	if (!running.running() || !novaworld_operation(running.kind)) {
		if (!v.novaworld.waiting.empty()) ui_kit::quiet_text(v.novaworld.waiting);
		return;
	}
	// What is drawn changes ten times a second at most, the operation's own steps however fast.
	struct Shown {
		uint64_t id = 0;
		double at = -1.0;
		std::string label;
		float fraction = -1.0f;
	};
	static Shown shown;
	const double now = ImGui::GetTime();
	if (shown.id != running.id || now - shown.at >= 0.1) {
		shown.id = running.id;
		shown.at = now;
		shown.label = running.label.empty() ? std::string(operation_kind_row(running.kind).verb) + "..." : running.label;
		shown.fraction = running.total > 0 ? float(double(std::min(running.done, running.total)) / double(running.total))
		                                   : -1.0f;
	}
	ui_kit::clipped_text(shown.label);
	if (shown.fraction >= 0.0f) ImGui::ProgressBar(shown.fraction, ImVec2(-FLT_MIN, 0.0f));
	const bool cancels = running.cancellable && v.allows(EditorRequestKind::NovaWorldCancel);
	ImGui::BeginDisabled(!cancels);
	if (ImGui::SmallButton("Cancel##novaworld_activity") && cancels) workspace.request(request::novaworld_cancel());
	ImGui::EndDisabled();
	ui_kit::tooltip(novaworld_cancel_words(running.kind));
}

bool NovaWorldWindow::stands_aside() const { return aside_for_welcome(workspace_.view(), welcome_asked_); }

void NovaWorldWindow::draw_account(const NovaWorldView &c) {
	const SessionView &v = workspace_.view();
	const std::string host = novaworld_host(c.server);
	if (c.signed_in) {
		ui_kit::clipped_text(c.username + (admin(c) ? " (admin)" : "") + " on " + host, c.server);
		ui_kit::WrapRow row;
		if (ui_kit::tool(row, "Sign out", v.allows(EditorRequestKind::NovaWorldSignOut),
		                 "Signs out of " + host + ": the public projects stay readable."))
			workspace_.request(request::novaworld_sign_out());
		return;
	}
	ui_kit::quiet_text("Sign in to push your own projects.", c.server);
	sign_in_.draw(workspace_);
	ui_kit::WrapRow row;
	const bool opens = v.allows(EditorRequestKind::NovaWorldOpenSite) && !c.server.empty();
	if (ui_kit::tool(row, "Register...", opens, "No account yet? Opens " + host + "'s Register page in your browser."))
		workspace_.request(request::novaworld_open_site("register"));
	if (ui_kit::tool(row, "Forgot password...", opens, "Opens " + host + "'s page that mails you a new password's link."))
		workspace_.request(request::novaworld_open_site("forgot"));
}

void NovaWorldWindow::draw_not_on_novaworld(const NovaWorldView &c) {
	const SessionView &v = workspace_.view();
	const NovaWorldStanding standing = novaworld_standing(c);
	const bool rejoins = standing.kind == NovaWorldStandingKind::WasOnNovaWorld;
	if (rejoins) ui_kit::clipped_text(standing.line, standing.tip);
	else ui_kit::empty_state("Not on NovaWorld.", "Put it on NovaWorld to push its files there and get them anywhere.");
	ui_kit::WrapRow row;
	if (rejoins && ui_kit::tool(row, "Rejoin", v.allows(EditorRequestKind::NovaWorldRejoin), standing.tip))
		workspace_.request(request::novaworld_rejoin());
	if (ui_kit::tool(row, "Put on NovaWorld...", v.allows(EditorRequestKind::NovaWorldPut) && c.signed_in && !c.calling,
	                 c.signed_in ? "Makes it a project on NovaWorld: a new one of yours, or an empty one you may push."
	                             : "Sign in to put the project on NovaWorld."))
		put_.open();
}

// A project its file names, with no standing here: its setup running, none running, or the service showing it none
// or not answering.
void NovaWorldWindow::draw_no_standing(const NovaWorldView &c) {
	const SessionView &v = workspace_.view();
	const NovaWorldStanding standing = novaworld_standing(c);
	if (standing.kind == NovaWorldStandingKind::SettingUp) {
		ui_kit::quiet_text(standing.line, standing.tip);
		return;
	}
	if (standing.kind == NovaWorldStandingKind::NotSetUp) {
		ui_kit::quiet_text(standing.line, standing.tip);
	} else {
		ui_kit::refusal_text(standing.line);
		ui_kit::tooltip(standing.tip);
	}
	ui_kit::WrapRow row;
	if (ui_kit::tool(row, "Check", v.allows(EditorRequestKind::NovaWorldCheck),
	                 "Sets the folder up as a working copy and looks for what changed."))
		workspace_.request(request::novaworld_check());
	if (standing.kind != NovaWorldStandingKind::NotFound) return;
	// The way back where the folder's own state names the project it left; else, signed in, leaving this one.
	if (!c.former_key.empty() &&
	    ui_kit::tool(row, "Rejoin", v.allows(EditorRequestKind::NovaWorldRejoin),
	                 "Takes this folder back to the project its own state names, with nothing downloaded."))
		workspace_.request(request::novaworld_rejoin());
	const std::string put_here = "Put on " + novaworld_host(c.server) + "...";
	if (c.signed_in && ui_kit::tool(row, put_here.c_str(),
	                                v.allows(EditorRequestKind::NovaWorldPut) && !c.calling,
	                                "Makes it a project of yours on this service, for this folder to push to."))
		put_.open_new_here(c.key);
	if (c.signed_in && ui_kit::tool(row, "Make it your own...", v.allows(EditorRequestKind::NovaWorldMakeOwn),
	                                "This folder leaves the project, its files yours. Asks first."))
		make_own_asked_ = true;
}

void NovaWorldWindow::draw_standing(const NovaWorldView &c) {
	const SessionView &v = workspace_.view();
	const NovaWorldStanding standing = novaworld_standing(c);
	// A folder holding no revision while the project has some (its files were not the head's as it was set up, or
	// the empty project it was put on was pushed to since): a pull first.
	if (standing.kind == NovaWorldStandingKind::NoRevision) {
		ui_kit::warning_line(standing.line, standing.tip);
		if (!standing.elsewhere.empty()) ui_kit::quiet_text(standing.elsewhere);
	} else {
		ui_kit::clipped_text(standing.line, standing.tip);
	}
	// A mod whose base game's project is missing (got with no install of its game): made from here once one is named.
	if (!c.missing_base.empty()) {
		ui_kit::warning_line("Its base game's project is missing: " + c.missing_base + ".",
		                     "The mod builds and plays over its base game's project, which its project file names there.");
		ImGui::SameLine();
		const bool makes = v.allows(EditorRequestKind::NovaWorldMakeBase) && c.remote_known && !c.remote.base.empty();
		ImGui::BeginDisabled(!makes);
		if (ImGui::SmallButton("Make the base game's project") && makes) workspace_.request(request::novaworld_make_base());
		ImGui::EndDisabled();
		ui_kit::tooltip(c.remote_known ? "Makes it there: got from NovaWorld, or a retail game's imported from the game "
		                                 "install (Project settings names it); then opens the mod again."
		                               : "Check first: NovaWorld says which game the mod is of.");
	}
	ui_kit::WrapRow row;
	if (ui_kit::tool(row, "Check", v.allows(EditorRequestKind::NovaWorldCheck),
	                 "Looks for what changed here and on NovaWorld."))
		workspace_.request(request::novaworld_check());
	if (ui_kit::tool(row, "Pull", v.allows(EditorRequestKind::NovaWorldPull) && c.conflicts.empty(),
	                 !c.conflicts.empty() ? "Settle every conflict first."
	                 : novaworld_holds_no_revision(c)
	                         ? "Stops on each file both hold otherwise. Files only NovaWorld has come in, even ones "
	                           "deleted here."
	                         : "Brings in the newest revision on NovaWorld; your own changes stay."))
		workspace_.request(request::novaworld_pull());
	// No base to merge over: this folder taken as the next revision instead, deletions and all.
	if (novaworld_holds_no_revision(c) &&
	    ui_kit::tool(row, "Replace with this folder...", v.allows(EditorRequestKind::NovaWorldKeepFolder),
	                 "Makes this folder the next revision on " + novaworld_host(c.server) + ". Asks first."))
		keep_folder_asked_ = true;

	const bool uncounted = !c.checked && c.changes.empty();
	ImGui::SeparatorText(uncounted ? "Changes" : ("Changes (" + std::to_string(c.changes.size()) + ")").c_str());
	if (uncounted) {
		ui_kit::empty_state("Not counted yet.", "Check lists what changed here since the revision; Push finds the "
		                                        "changes itself.");
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
					const bool added = change.change == NovaWorldChange::Added;
					ImGui::BeginDisabled(!restores);
					if (ImGui::SmallButton(added ? "Remove" : "Discard") && restores)
						workspace_.request(request::novaworld_restore(change.path));
					ImGui::EndDisabled();
					ui_kit::tooltip(added ? "Deletes the file, which revision " + std::to_string(c.revision) +
					                                " does not have: it goes to the project's trash (kept until Empty "
					                                "the trash)."
					                      : "Puts back the file as revision " + std::to_string(c.revision) +
					                                " has it; yours goes to the project's trash (kept until Empty the "
					                                "trash).");
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
		// Signed out, its owner reads as anyone: nothing here says the project is not theirs.
		if (!c.signed_in) {
			ui_kit::quiet_text("Sign in to push.", "You may pull as you are; your own projects push once you sign in.");
			return;
		}
		ui_kit::quiet_text("Read-only: you may pull; its owner and admins push it.");
		ui_kit::WrapRow row;
		if (ui_kit::tool(row, "Make it your own...", v.allows(EditorRequestKind::NovaWorldMakeOwn),
		                 "This folder leaves the project, its files yours. Asks first."))
			make_own_asked_ = true;
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
	const bool behind = c.head_known && c.head > c.revision;
	const bool pushes = v.allows(EditorRequestKind::NovaWorldPush) && message_[0] && (!c.checked || !c.changes.empty()) &&
	                    c.conflicts.empty() && !behind;
	ImGui::BeginDisabled(!pushes);
	const bool pushed = ImGui::Button("Push") && pushes;
	ImGui::EndDisabled();
	const std::string why = novaworld_holds_no_revision(c)  ? "Pull first: the folder holds no revision of the project yet."
	                  : behind               ? "Pull r" + std::to_string(c.head) + " first: NovaWorld is past this folder."
	                  : c.checked && c.changes.empty() ? "Nothing to push."
	                  : !message_[0]         ? "Say what changed first."
	                  : !c.conflicts.empty() ? "Settle every conflict first."
	                                         : "Sends every change to " + novaworld_host(c.server) + " as the "
	                                           "project's next revision; players get it once a release of it is "
	                                           "published (an owner's once an admin approves it).";
	ui_kit::tooltip(why);
	if (pushed) workspace_.request(request::novaworld_push(message_));
}

// A release of the revision the folder is at, built on the service (ADR 0069 d7), for an account the service says
// may release the project (its owner, an admin: can_release, ADR 0074): a draft once the service has packed and
// checked it, then on the site an admin's published (the admin's Products page) and an owner's submitted for an
// admin's approval (its project page under Projects). The release players get now, above it.
void NovaWorldWindow::draw_release(const NovaWorldView &c) {
	const SessionView &v = workspace_.view();
	if (!c.remote_known || !c.remote.can_release) return;
	ImGui::SeparatorText("Release");
	// The published release, which players' updaters install: its game version and the ONE that
	// built it; then the protocol the project's title speaks, which every release of it speaks.
	const std::string published =
			release_words(c.remote.release_version, c.remote.release_one_version);
	ui_kit::quiet_text(c.remote.release_id != 0 ? "Players update to " + published + "."
												: "Nothing published yet.",
			"The release published on NovaWorld, which players' updaters install: its game "
			"version and the ONE (the OpenNova Engine) that built it.");
	Versions versions;
	title_versions(c.remote.game, versions);
	if (!versions.protocol.empty())
		ui_kit::quiet_text(
				"Speaks " + versions.protocol + " protocol " + versions.protocol_version + ".",
				"The in-match protocol " + versions.game + " speaks and its version (PV2), the "
				"revision a host checks a joiner against.");
	// What a release of this revision leaves out, said before it is built.
	if (c.checked && !c.changes.empty())
		ui_kit::warning_line(counted(c.changes.size(), "change") + " not pushed: not in this release.",
		                     "A release is built from a revision on NovaWorld: push first to have them in it.");
	if (c.head_known && c.head > c.revision)
		ui_kit::warning_line("r" + std::to_string(c.head) + " is on NovaWorld: this release is r" + std::to_string(c.revision) + ".",
		                     "Pull first to release the newest revision.");
	const std::string then = admin(c) ? "publish it here, or on the admin's Products page, once it is a draft"
	                                  : "once it is a draft, submit it for approval: an admin approves it before players "
	                                    "get it";
	// The version a release was started with is cleared once the service lists it (started), kept on a refusal.
	for (const RemoteRelease &release : c.releases)
		if (version_[0] && release.version == version_) version_[0] = '\0';
	if (focus_release_) {
		ImGui::SetScrollHereY();
		ImGui::SetKeyboardFocusHere();
		focus_release_ = false;
	}
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
	ImGui::InputTextWithHint("Game version", "0.2.0", version_, sizeof(version_));
	ui_kit::tooltip("The release's game version: one to four numbers between dots (0.2.0).");
	ImGui::SameLine();
	const bool releases = v.allows(EditorRequestKind::NovaWorldRelease) && version_[0] && c.revision > 0 && !c.calling;
	ImGui::BeginDisabled(!releases);
	const bool released = ImGui::Button(("Build release (r" + std::to_string(c.revision) + ")###release").c_str()) && releases;
	ImGui::EndDisabled();
	ui_kit::tooltip(c.revision <= 0 ? "Push first: a release is built from a revision on NovaWorld."
	                : !version_[0]  ? "Name its game version first."
	                                : "Builds a release of r" + std::to_string(c.revision) + " on the service; " + then +
	                                          ".");
	if (released) workspace_.request(request::novaworld_release(version_));
	// The project's releases, newest first: what each came to, a draft's Submit (Publish, an admin's).
	ui_kit::WrapRow row;
	if (ui_kit::tool(row, "Open on the site", v.allows(EditorRequestKind::NovaWorldOpenSite),
	                 admin(c) ? "Opens the admin's Products page for this project in your browser."
	                          : "Opens its page under Projects in your browser: its releases, its history."))
		workspace_.request(request::novaworld_open_site("project"));
	if (!c.releases_error.empty()) ui_kit::refusal_text(c.releases_error);
	const bool submits = v.allows(EditorRequestKind::NovaWorldSubmit) && !c.calling;
	size_t shown = 0;
	for (const RemoteRelease &release : c.releases) {
		if (shown++ == 5) break;
		ImGui::PushID(static_cast<int>(release.id));
		const bool rejected = release.status == "draft" && !release.review_note.empty();
		const std::string status = rejected                                ? "sent back: " + release.review_note
		                           : release.status == "draft"             ? std::string(admin(c) ? "draft" : "draft, ready to submit")
		                           : release.status == "awaiting_approval" ? std::string("awaiting approval")
		                           : release.status == "invalid"           ? "invalid: " + release.error
		                           : release.status == "validating"        ? std::string("being checked")
		                           : release.status == "published"         ? std::string("published: players update to it")
		                                                                   : release.status;
		const std::string built =
				release.one_version.empty() ? "" : ", built by ONE " + release.one_version;
		ui_kit::clipped_text(release.version + "  " + status, release.name + ": " + status + built);
		if (release.status == "draft") {
			ImGui::SameLine();
			ImGui::BeginDisabled(!submits);
			if (ImGui::SmallButton(admin(c) ? "Publish" : "Submit for approval") && submits)
				workspace_.request(request::novaworld_submit(release.version));
			ImGui::EndDisabled();
			ui_kit::tooltip(admin(c) ? "Publishes it: players update to it."
			                         : "Sends it to NovaWorld's admins, who approve it before players get it.");
		}
		ImGui::PopID();
	}
}

void NovaWorldWindow::draw_conflicts(const NovaWorldView &c) {
	if (c.conflicts.empty()) return;
	const SessionView &v = workspace_.view();
	ImGui::SeparatorText(("Conflicts (" + std::to_string(c.conflicts.size()) + ")").c_str());
	ui_kit::quiet_text("This folder and " + novaworld_host(c.server) + " both changed these.",
	                   "The pull of revision " + std::to_string(c.pull_revision) + " waits for them: keep yours or take "
	                   "theirs, and it goes on by itself.");
	{
		ui_kit::WrapRow row;
		const bool resolves = v.allows(EditorRequestKind::NovaWorldResolve);
		for (const auto &[label, resolution, tip] :
		     {std::tuple<const char *, Resolution, const char *>{"Keep all mine", Resolution::KeepMine,
		                                                         "Keeps this folder's file in each: your changes go in your next push."},
		      {"Take all theirs", Resolution::TakeTheirs,
		       "Puts NovaWorld's file in place of each of yours; yours go to the project's trash."}})
			if (ui_kit::tool(row, label, resolves, tip))
				for (const NovaWorldConflictView &conflict : c.conflicts)
					workspace_.request(request::novaworld_resolve(conflict.path, resolution));
		if (ui_kit::tool(row, "Give up the pull", v.allows(EditorRequestKind::NovaWorldCancel),
		                 "Stops the pull of revision " + std::to_string(c.pull_revision) +
		                         ": no file changes; Pull starts it over."))
			workspace_.request(request::novaworld_cancel());
	}
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

// Make it your own...'s ask: the project file every service reads loses its key, so what goes, what stays and the way
// back are said first.
void NovaWorldWindow::draw_make_own(const NovaWorldView &c) {
	const SessionView &v = workspace_.view();
	const bool held = make_own_asked_ && v.project.open && !c.key.empty();
	if (!make_own_popup_.begin("Make it your own", held, true, ImGuiWindowFlags_AlwaysAutoResize)) {
		if (make_own_popup_.dismissed() || !held) make_own_asked_ = false;
		return;
	}
	ui_kit::dialog_text("This folder leaves its NovaWorld project, on every service. Its files stay as they are, and the "
	                    "project stays on NovaWorld. Rejoin brings the folder back, until you put it there as a new project.");
	const ProjectDocument &doc = *v.project.document;
	if (!doc.expansion.standalone())
		ui_kit::warning_line("NovaWorld keeps this mod's short name.",
		                     "A short name is one project's across NovaWorld, so this copy cannot be put there as it is.");
	else if (!doc.archives.empty() && !admin(c))
		ui_kit::warning_line("This copy holds the game's own files.", "Only an admin puts those on NovaWorld.");
	const bool allowed = v.allows(EditorRequestKind::NovaWorldMakeOwn);
	ImGui::BeginDisabled(!allowed);
	const bool own = ImGui::Button("Make it your own");
	ImGui::EndDisabled();
	if (own && allowed) {
		workspace_.request(request::novaworld_make_own());
		make_own_asked_ = false;
		make_own_popup_.close();
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel")) {
		make_own_asked_ = false;
		make_own_popup_.close();
	}
	ImGui::EndPopup();
}

// Replace with this folder...'s ask: nothing is sent yet, but the next push takes the place of what the service holds.
void NovaWorldWindow::draw_keep_folder(const NovaWorldView &c) {
	const SessionView &v = workspace_.view();
	const bool held = keep_folder_asked_ && v.project.open && novaworld_holds_no_revision(c);
	if (!keep_folder_popup_.begin("Replace with this folder", held, true, ImGuiWindowFlags_AlwaysAutoResize)) {
		if (keep_folder_popup_.dismissed() || !held) keep_folder_asked_ = false;
		return;
	}
	const std::string revision = "r" + std::to_string(c.head);
	ui_kit::dialog_text("This folder becomes the next revision on " + novaworld_host(c.server) + ": your next push puts its "
	                    "files in place of " + revision + "'s. A file only " + revision + " has goes, and anything newer "
	                    "there that this folder lacks is undone. Nothing is sent until you push, and Discard puts a file back.");
	const bool allowed = v.allows(EditorRequestKind::NovaWorldKeepFolder);
	ImGui::BeginDisabled(!allowed);
	const bool keep = ImGui::Button("Replace with this folder");
	ImGui::EndDisabled();
	if (keep && allowed) {
		workspace_.request(request::novaworld_keep_folder());
		keep_folder_asked_ = false;
		keep_folder_popup_.close();
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel")) {
		keep_folder_asked_ = false;
		keep_folder_popup_.close();
	}
	ImGui::EndPopup();
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
	draw_novaworld_activity(workspace_);
	if (!v.project.open) draw_novaworld_projects(workspace_);
	else if (c.key.empty()) draw_not_on_novaworld(c);
	else if (!c.tracked || novaworld_standing(c).kind == NovaWorldStandingKind::NotFound) draw_no_standing(c);
	else {
		draw_standing(c);
		draw_conflicts(c);
		draw_history(c);
	}
	// NovaWorld's other projects, to get while this one is open (Get... opens the one got in its place).
	if (v.project.open && ImGui::CollapsingHeader("Other projects on NovaWorld")) draw_novaworld_projects(workspace_);
	if (!c.last.empty()) {
		ImGui::Spacing();
		ui_kit::quiet_text(c.last);
	}
	put_.draw(workspace_);
	draw_make_own(c);
	draw_keep_folder(c);
}

} // namespace opennova::editor
