// The NovaWorld window and NovaWorld's place in the workspace (ADR 0069): over a hand-made view, what the
// window shows of a working copy (its revision, the store's, its changes) and the status bar's words for it; a
// push's message typed and Push raising novaworld_push, refused while there is nothing to push; a conflict's Keep
// mine; an account that may not push the project shown a read-only line and no Release (the window's or the Build
// menu's), a player's own project its Push and its Release (can_release) under the release players get; a folder
// holding no revision of a project that has some said so, its Push waiting for a pull; a project not on
// NovaWorld, Put on NovaWorld... asking the lists again as it opens and raising novaworld_put for a new public
// project of the account's (its name and key from the project's title) or an empty one it may push that the project
// is (none of another kind chosen), and no Bind anywhere; a
// project whose working copy is being set up, or that the service does not show; the sign-in's form raising
// novaworld_sign_in with what was typed; the store's projects listed with no project open in two groups, Yours
// and Everyone's, Get... asking for a folder and the pick getting the project into a folder of its key's name,
// Set up... filling the New project form for an empty one, which then says so with its clear; the welcome page's
// section; Project settings' NovaWorld section; nothing past its room.
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <editor/project/project_document.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/editor_windows.h>

#include "editor_ui_test_support.h"

#include <imgui.h>

namespace editor_ui_test {

namespace {

// The store's MOD, a game: at revision 5, public, the account may push and release it.
RemoteProject mod_project() {
	RemoteProject mod;
	mod.key = "MOD";
	mod.title = "A mod";
	mod.kind = "game";
	mod.game = "jo";
	mod.is_public = true;
	mod.head = 5;
	mod.can_push = true;
	mod.can_release = true;
	return mod;
}

// A project, a working copy of the store's MOD at revision 3, the store at 5, two files changed, an admin signed
// in.
SessionView tracked_view() {
	SessionView v;
	v.project.open = true;
	v.project.root = "C:/mods/Mod";
	ProjectDocument doc;
	doc.title = "My Mod";
	doc.novaworld = "MOD";
	v.project.document = std::make_shared<const ProjectDocument>(doc);
	NovaWorldView &c = v.novaworld;
	c.available = true;
	c.server = "https://novaworld.test";
	c.username = "taylor";
	c.role = "admin";
	c.signed_in = true;
	c.key = "MOD";
	c.tracked = true;
	c.revision = 3;
	c.remote_known = true;
	c.remote = mod_project();
	c.checked = true;
	c.head_known = true;
	c.head = 5;
	c.changes = {{"defs/a.def", NovaWorldChange::Modified}, {"models/b.3di", NovaWorldChange::Added}};
	c.projects = {mod_project()};
	return v;
}

ImGuiID novaworld_item(std::initializer_list<const char *> steps) { return item_id(Ui::window_id("NovaWorld"), steps); }
ImGuiID put_item(std::initializer_list<const char *> steps) { return item_id(ImHashStr("Put on NovaWorld"), steps); }

void test_working_copy() {
	Ui ui;
	SessionView v = tracked_view();
	ui.windows.set_view(&v);
	ui.focus("NovaWorld");
	ui.away();
	const std::string text = logged_frame(ui);
	CHECK(text.find("taylor (admin) on novaworld.test") != std::string::npos, "who is signed in, and where");
	CHECK(text.find("MOD r3, r5 on NovaWorld") != std::string::npos, "the revision and NovaWorld's newest");
	CHECK(text.find("Changes (2)") != std::string::npos && text.find("modified  defs/a.def") != std::string::npos &&
	              text.find("added  models/b.3di") != std::string::npos,
	      "the changes, each with what it did");
	CHECK(text.find("MOD r3, 2 changed, 2 incoming") != std::string::npos, "the status bar's standing");
	CHECK(text.find("Bind") == std::string::npos, "no Bind");
	for (const std::string &offender : overflowing()) CHECK(false, ("tracked: " + offender).c_str());

	// Push waits for a message; typed, it raises novaworld_push with it.
	ui.activate(novaworld_item({"Push"}));
	CHECK(ui.drain().empty(), "no message: Push raises nothing");
	ui.activate(novaworld_item({"##message"}));
	ImGui::GetIO().AddInputCharactersUTF8("Fix the armory");
	ui.frames(2);
	ui.activate(novaworld_item({"Push"}));
	std::vector<EditorRequest> raised = ui.drain();
	const EditorRequest *push = one(raised, EditorRequestKind::NovaWorldPush);
	CHECK(push && push->message == "Fix the armory", "Push: one novaworld_push with the message");

	// Release r3 waits for a version; typed, it raises novaworld_release with it. Above it, the release players get.
	CHECK(text.find("Nothing published yet.") != std::string::npos && text.find("2 changes not pushed") != std::string::npos &&
	              text.find("r5 is on NovaWorld: this release is r3.") != std::string::npos,
	      "nothing published, the unpushed changes and the newer head said");
	ui.activate(novaworld_item({"###release"}));
	CHECK(ui.drain().empty(), "no version: Release raises nothing");
	ui.activate(novaworld_item({"##version"}));
	ImGui::GetIO().AddInputCharactersUTF8("0.2.0");
	ui.frames(2);
	ui.activate(novaworld_item({"###release"}));
	raised = ui.drain();
	const EditorRequest *release = one(raised, EditorRequestKind::NovaWorldRelease);
	CHECK(release && release->version == "0.2.0", "Release r3: one novaworld_release of the version");

	// A change's Discard: the file put back as the revision has it (in the changes' own child window).
	{
		char name[96];
		std::snprintf(name, sizeof(name), "NovaWorld/changes_%08X", novaworld_item({"changes"}));
		ui.activate(item_id(Ui::window_id(name), {"changes_table", "defs/a.def", "Discard"}));
		raised = ui.drain();
		const EditorRequest *discard = one(raised, EditorRequestKind::NovaWorldRestore);
		CHECK(discard && discard->path == "defs/a.def" && discard->revision == 0,
		      "Discard: one novaworld_restore of the file to the base");
	}

	// The history, newest first, and Refresh history.
	RemoteRevision r5;
	r5.number = 5;
	r5.author = "alex";
	r5.message = "More rifles";
	r5.file_count = 2;
	v.novaworld.history = {r5};
	ui.frames(2);
	ui.away();
	CHECK(logged_frame(ui).find("r5  alex: More rifles") != std::string::npos, "the history listed");
	ui.activate(novaworld_item({"Refresh history"}));
	CHECK(one(ui.drain(), EditorRequestKind::NovaWorldHistory), "Refresh history: one novaworld_history");

	// Check and Pull.
	ui.activate(novaworld_item({"Check"}));
	CHECK(one(ui.drain(), EditorRequestKind::NovaWorldCheck), "Check: one novaworld_check");
	ui.activate(novaworld_item({"Pull"}));
	CHECK(one(ui.drain(), EditorRequestKind::NovaWorldPull), "Pull: one novaworld_pull");

	// A pull stopped on a conflict: Keep mine settles it, Pull and Push wait for it.
	v.novaworld.conflicts = {{"defs/a.def", NovaWorldChange::Modified, NovaWorldChange::Deleted}};
	ui.frames(2);
	ui.away();
	CHECK(logged_frame(ui).find("Conflicts (1)") != std::string::npos, "the conflicts listed");
	ui.activate(novaworld_item({"conflicts", "defs/a.def", "Keep mine"}));
	raised = ui.drain();
	const EditorRequest *keep = one(raised, EditorRequestKind::NovaWorldResolve);
	CHECK(keep && keep->path == "defs/a.def" && keep->resolution == Resolution::KeepMine,
	      "Keep mine: one novaworld_resolve of the file");
	ui.activate(novaworld_item({"Pull"}));
	ui.activate(novaworld_item({"Push"}));
	CHECK(ui.drain().empty(), "Pull and Push wait for the conflict");
	v.novaworld.conflicts.clear();

	// A player whose project it is not: a read-only line, no message box, no Push, no Release.
	v.novaworld.role = "player";
	v.novaworld.remote.can_push = false;
	v.novaworld.remote.can_release = false;
	ui.frames(2);
	ui.away();
	std::string read_only = logged_frame(ui);
	CHECK(read_only.find("Read-only") != std::string::npos, "may not push: a read-only line");
	CHECK(read_only.find("Build release (r3)") == std::string::npos, "another's project: no Release");
	ui.activate(novaworld_item({"Push"}));
	CHECK(ui.drain().empty(), "a read-only project's Push raises nothing");
	const uint64_t before_disabled = v.events.next_seq();
	CHECK(choose(ui, "Build", {"###release"}).empty() && v.events.next_seq() == before_disabled,
	      "another's project: Build > Release... does nothing");
	// A player's own project: Push and Release (its owner releases it), the release players get above it.
	v.novaworld.remote.can_push = true;
	v.novaworld.remote.can_release = true;
	v.novaworld.remote.release_id = 9;
	v.novaworld.remote.release_version = "0.1.0";
	ui.frames(2);
	ui.away();
	read_only = logged_frame(ui);
	CHECK(read_only.find("Read-only") == std::string::npos && read_only.find("Build release (r3)") != std::string::npos &&
	              read_only.find("Players update to 0.1.0.") != std::string::npos,
	      "a player's own project: no read-only line, its Release and the release players get");
	ui.activate(novaworld_item({"Push"}));
	CHECK(one(ui.drain(), EditorRequestKind::NovaWorldPush), "a player's own project: Push");
	const uint64_t before_menu = v.events.next_seq();
	CHECK(choose(ui, "Build", {"###release"}).empty() && v.events.next_seq() > before_menu,
	      "a player's own project: Build > Release... brings the NovaWorld window forward");
	ui.focus("NovaWorld");
	// The version typed before stays (no release of it listed yet): Build release asks again with it.
	ui.activate(novaworld_item({"###release"}));
	raised = ui.drain();
	release = one(raised, EditorRequestKind::NovaWorldRelease);
	CHECK(release && release->version == "0.2.0", "a player's own project: Build release raises novaworld_release");
	// Its releases: a draft's Submit for approval, a sent back one's note; the version cleared once listed.
	RemoteRelease draft;
	draft.id = 8;
	draft.name = "MOD 0.2.0";
	draft.version = "0.2.0";
	draft.status = "draft";
	RemoteRelease back = draft;
	back.id = 7;
	back.version = "0.1.9";
	back.review_note = "The menus crash";
	v.novaworld.releases = {draft, back};
	ui.frames(2);
	ui.away();
	const std::string listed = logged_frame(ui);
	CHECK(listed.find("0.2.0  draft, ready to submit") != std::string::npos &&
	              listed.find("0.1.9  sent back: The menus crash") != std::string::npos,
	      "the releases, what each came to");
	ui.activate(item_id(pushed(Ui::window_id("NovaWorld"), 8), {"Submit for approval"}));
	raised = ui.drain();
	const EditorRequest *submitted = one(raised, EditorRequestKind::NovaWorldSubmit);
	CHECK(submitted && submitted->version == "0.2.0", "Submit for approval: one novaworld_submit of its version");
	ui.activate(novaworld_item({"Open on the site"}));
	raised = ui.drain();
	const EditorRequest *site = one(raised, EditorRequestKind::NovaWorldOpenSite);
	CHECK(site && site->page == "project", "Open on the site: the project's page");
	v.novaworld.releases.clear();

	// A folder holding no revision while the project has some (its files not the head's as it was set up): said,
	// and Push waits for a pull.
	v.novaworld.revision = 0;
	ui.frames(2);
	ui.away();
	CHECK(logged_frame(ui).find("No revision of MOD here yet: Pull r5 first.") != std::string::npos, "no revision: said");
	ui.activate(novaworld_item({"Push"}));
	CHECK(ui.drain().empty(), "no revision: Push waits for a pull");
	v.novaworld.revision = 3;

	// The first push's message offered: a working copy at no revision yet (the last message gone with its
	// changes first).
	v.novaworld.changes.clear();
	ui.frames(2);
	v.novaworld.revision = 0;
	v.novaworld.head = 0;
	v.novaworld.changes = {{"project.opennova", NovaWorldChange::Added}};
	ui.frames(2);
	ui.activate(novaworld_item({"Push"}));
	raised = ui.drain();
	push = one(raised, EditorRequestKind::NovaWorldPush);
	CHECK(push && push->message == "First revision", "a project's first push: its message prefilled");
	std::printf("content: a working copy, its changes and the bar's words, Push with its message, Keep mine, "
	            "read-only\n");
}

void test_put_and_sign_in() {
	Ui ui;
	SessionView v = tracked_view();
	ProjectDocument doc;
	doc.title = "My Mod";
	v.project.document = std::make_shared<const ProjectDocument>(doc);
	NovaWorldView &c = v.novaworld;
	c.key.clear();
	c.tracked = false;
	c.revision = 0;
	c.remote_known = false;
	c.checked = false;
	c.changes.clear();
	c.put.kind = "game";
	c.put.game = "jo";
	ui.windows.set_view(&v);
	ui.focus("NovaWorld");
	ui.away();
	std::string text = logged_frame(ui);
	CHECK(text.find("Not on NovaWorld.") != std::string::npos, "a project not on NovaWorld says so");
	CHECK(text.find("MOD r") == std::string::npos && text.find("Bind") == std::string::npos,
	      "the bar says nothing of the store, and no Bind");
	// Put on NovaWorld...: a new project of the account's, its name and key from the project's title, public; the
	// lists asked again as it opens (a novaworld_check), what it makes found as they come back.
	ui.activate(novaworld_item({"Put on NovaWorld..."}));
	ui.frames(2);
	CHECK(one(ui.drain(), EditorRequestKind::NovaWorldCheck), "Put on NovaWorld...: one novaworld_check as it opens");
	ui.away();
	text = logged_frame(ui);
	CHECK(text.find("A game for Joint Operations") != std::string::npos && text.find("Who gets it") != std::string::npos,
	      "what it makes, and who gets it, shown");
	ui.activate(put_item({"Put"}));
	std::vector<EditorRequest> raised = ui.drain();
	const EditorRequest *made = one(raised, EditorRequestKind::NovaWorldPut);
	CHECK(made && made->create && made->project_key == "MYMOD" && made->title == "My Mod" &&
	              made->visibility == "public",
	      "Put: one novaworld_put of a new public project, keyed from its name");
	// It stays open while NovaWorld answers, and says a refusal under its fields.
	c.put_refusal = "NovaWorld has a project MYMOD already: choose another key.";
	ui.frames(2);
	ui.away();
	CHECK(logged_frame(ui).find("NovaWorld has a project MYMOD already") != std::string::npos,
	      "the put refused: said in the dialog, its fields kept");
	c.put_refusal.clear();
	ui.activate(put_item({"Cancel"}));
	ui.drain();
	// An empty project the account may push that the project is not (a mod, the project a game): none chosen,
	// and Put waits for one.
	RemoteProject empty_mod = mod_project();
	empty_mod.key = "EMPTYMOD";
	empty_mod.kind = "expansion";
	empty_mod.expansion_code = "emptymod";
	empty_mod.head = 0;
	c.projects = {empty_mod};
	ui.activate(novaworld_item({"Put on NovaWorld..."}));
	ui.frames(2);
	ui.drain();
	ui.away();
	CHECK(logged_frame(ui).find("An empty project you may push") == std::string::npos,
	      "an empty mod is no choice for a game: no empty project offered");
	ui.activate(put_item({"Cancel"}));
	ui.drain();
	c.projects = {mod_project()};
	// An empty project the account may push, chosen (a game of its game): Put takes it.
	c.projects.front().head = 0;
	ui.activate(novaworld_item({"Put on NovaWorld..."}));
	ui.frames(2);
	ui.drain();
	ui.activate(put_item({"An empty project you may push"}));
	ui.frames(2);
	ui.activate(put_item({"Put"}));
	raised = ui.drain();
	const EditorRequest *taken = one(raised, EditorRequestKind::NovaWorldPut);
	CHECK(taken && !taken->create && taken->project_key == "MOD", "Put: one novaworld_put of the empty MOD");
	ui.activate(put_item({"Cancel"}));
	ui.drain();
	c.projects.front().head = 5;
	// A mod: its base game named as the games the account may make a mod of name it.
	c.put.kind = "expansion";
	c.put.base = "JO";
	c.put.expansion_code = "mymod";
	c.games = {{"JO", "Joint Operations", "jo"}};
	ui.activate(novaworld_item({"Put on NovaWorld..."}));
	ui.frames(2);
	ui.drain();
	ui.away();
	CHECK(logged_frame(ui).find("A mod of Joint Operations (JO), as mymod") != std::string::npos,
	      "a mod's base game, by its name");
	ui.activate(put_item({"Cancel"}));
	ui.drain();
	c.put.kind = "game";
	c.put.base.clear();
	// A player's key, as the service takes it: up to 11, no "QQ".
	{
		ProjectDocument quest;
		quest.title = "QQuest of the Long Night";
		v.project.document = std::make_shared<const ProjectDocument>(quest);
		c.role = "player";
		ui.activate(novaworld_item({"Put on NovaWorld..."}));
		ui.frames(2);
		ui.drain();
		ui.activate(put_item({"Put"}));
		raised = ui.drain();
		made = one(raised, EditorRequestKind::NovaWorldPut);
		CHECK(made && made->project_key == "QUESTOFTHEL", "a player's key: up to 11, no QQ");
		ui.activate(put_item({"Cancel"}));
		ui.drain();
		c.role = "admin";
		v.project.document = std::make_shared<const ProjectDocument>(doc);
	}

	// Set up for a key the service does not show: said so, and Check.
	c.key = "NOPE";
	c.head_error = "NOPE is not on novaworld.test, or is private: sign in with an account that may read it.";
	ui.frames(2);
	ui.away();
	CHECK(logged_frame(ui).find("NOPE is not on novaworld.test, or is private") != std::string::npos,
	      "a key the service does not show: said");
	ui.activate(novaworld_item({"Check"}));
	CHECK(one(ui.drain(), EditorRequestKind::NovaWorldCheck), "Check: one novaworld_check");
	c.head_error.clear();
	ui.frames(2);
	ui.away();
	CHECK(logged_frame(ui).find("Setting up NOPE's working copy...") != std::string::npos, "being set up: said");
	c.key.clear();

	// Signed out: the form, its server the view's, and what was typed.
	c.signed_in = false;
	c.role.clear();
	ui.frames(2);
	ui.away();
	CHECK(logged_frame(ui).find("Sign in to push your own projects.") != std::string::npos, "signed out: said");
	ui.activate(novaworld_item({"Sign in..."}));
	ui.frames(2);
	ui.activate(novaworld_item({"Username"}));
	ImGui::GetIO().AddInputCharactersUTF8("taylor");
	ui.frames(2);
	ui.activate(novaworld_item({"Password"}));
	ImGui::GetIO().AddInputCharactersUTF8("pw");
	ui.frames(2);
	ui.activate(novaworld_item({"Sign in"}));
	raised = ui.drain();
	const EditorRequest *sign_in = one(raised, EditorRequestKind::NovaWorldSignIn);
	CHECK(sign_in && sign_in->server == "https://novaworld.test" && sign_in->username == "taylor" &&
	              sign_in->password == "pw",
	      "Sign in: one novaworld_sign_in with the server, the account and the password");
	ui.away();
	CHECK(logged_frame(ui).find("pw") == std::string::npos, "the password is never drawn");
	std::printf("content: Put on NovaWorld..., a new project or an empty one; set up, not shown; the sign-in's form\n");
}

void test_get_a_project() {
	Ui ui;
	SessionView v = tracked_view();
	v.project.open = false;
	v.project.root.clear();
	v.novaworld.titles = {"jo"};
	// Beside MOD (the account's to push): an empty expansion's project it may push, and another's it may read.
	RemoteProject empty;
	empty.key = "EMPTY";
	empty.title = "Mine";
	empty.kind = "expansion";
	empty.base = "MOD";
	empty.expansion_code = "mine";
	empty.can_push = true;
	RemoteProject theirs = mod_project();
	theirs.key = "THEIRS";
	theirs.title = "Theirs";
	theirs.can_push = false;
	theirs.owner = "alex";
	v.novaworld.projects = {mod_project(), empty, theirs};
	ui.windows.set_view(&v);
	ui.frames(3);
	ui.away();
	const std::string home = logged_frame(ui);
	CHECK(home.find("Get a project from NovaWorld") != std::string::npos && home.find("A mod  MOD r5") != std::string::npos,
	      "the welcome page lists the store's projects");
	CHECK(home.find("OpenNova's") != std::string::npos && home.find("Players'") != std::string::npos &&
	              home.find("Theirs  THEIRS r5") != std::string::npos && home.find("Mine  EMPTY, empty") != std::string::npos,
	      "in groups by whose they are: OpenNova's, other players'");
	// The NovaWorld window shown over the welcome page lists them too; Get... asks for a folder.
	auto *content = const_cast<devtools::Window *>(find_window(ui.windows.pass(), "NovaWorld"));
	CHECK(content != nullptr, "the NovaWorld window");
	if (!content) return;
	content->show_anyway();
	ui.focus("NovaWorld");
	ui.activate(novaworld_item({"novaworld_opennova", "MOD", "Get..."}));
	std::vector<EditorRequest> raised = ui.drain();
	const EditorRequest *pick = one(raised, EditorRequestKind::PickDirectory);
	CHECK(pick && pick->purpose == PickPurpose::NovaWorldFolder && pick->project_key.empty(),
	      "Get...: one pick of a folder, the key kept by the windows");
	ui.windows.deliver_pick(PickPurpose::NovaWorldFolder, "C:/mods");
	raised = ui.drain();
	const EditorRequest *get = one(raised, EditorRequestKind::NovaWorldGet);
	CHECK(get && get->project_key == "MOD" && get->dir == "C:/mods/MOD", "the pick: the project got into C:/mods/MOD");
	ui.windows.deliver_pick(PickPurpose::NovaWorldFolder, "C:/other");
	CHECK(ui.drain().empty(), "a second pick with no Get... before it gets nothing");
	ui.activate(novaworld_item({"novaworld_players", "THEIRS", "Get..."}));
	CHECK(one(ui.drain(), EditorRequestKind::PickDirectory), "another's project: Get...");
	ui.windows.deliver_pick(PickPurpose::NovaWorldFolder, "C:/mods");
	ui.drain();
	// A copy of it on this computer (a recent project naming its key): Open, no second get.
	ProjectView::RecentProject copy;
	copy.root = "C:/mods/THEIRS";
	copy.found = true;
	copy.title = "Theirs";
	copy.novaworld = "THEIRS";
	v.project.recent_details = {copy};
	ui.frames(2);
	ui.activate(novaworld_item({"novaworld_players", "THEIRS", "Open"}));
	raised = ui.drain();
	const EditorRequest *opened = one(raised, EditorRequestKind::OpenProject);
	CHECK(opened && opened->dir == "C:/mods/THEIRS", "a copy on this computer: Open opens it");
	v.project.recent_details.clear();
	ui.frames(2);
	// Set up...: the New project form filled for the empty project, a mod of its code (the workspace's, as the
	// session takes the set_workspace).
	ui.activate(novaworld_item({"novaworld_opennova", "EMPTY", "Set up..."}));
	ui.drain();
	const WorkspaceView::NewProject &form = v.workspace.new_project;
	CHECK(form.novaworld == "EMPTY" && form.making == WorkspaceView::Making::Mod && form.expansion == "mine" &&
	              form.title == "Mine" && form.stock.empty() && form.game.empty() && form.base_key == "MOD",
	      "Set up...: the form filled for EMPTY, a mod as mine");
	// The form set up for it says so, its clear taking it off, and Create names it.
	v.workspace.new_project.making = WorkspaceView::Making::Game;
	v.workspace.new_project.dir = "C:/mods/Mine";
	v.revisions.touch(ViewConcern::Workspace);
	ui.focus("Document");
	ui.frames(2);
	ui.away();
	CHECK(logged_frame(ui).find("For NovaWorld project EMPTY") != std::string::npos, "the form says what it is for");
	const ImGuiID welcome = item_id(Ui::window_id("Document"), {"welcome"});
	ui.activate(item_id(welcome, {"Make the project"}));
	raised = ui.drain();
	const EditorRequest *made = one(raised, EditorRequestKind::NewProject);
	CHECK(made && made->project_key == "EMPTY" && made->dir == "C:/mods/Mine", "Create: the project made for EMPTY");
	ui.activate(item_id(welcome, {"x##novaworld"}));
	ui.drain();
	CHECK(form.novaworld.empty(), "its clear: the form for no NovaWorld project");
	std::printf("content: the store's projects on the welcome page, yours and everyone's, Get... and its folder, "
	            "Set up... and the form it fills\n");
}

// Project settings' NovaWorld section: the project's NovaWorld project and its revision, its key no setting;
// a project not on NovaWorld, Put on NovaWorld... over the dialog.
void test_settings_section() {
	Ui ui;
	SessionView v = tracked_view();
	v.workspace.settings.open = true;
	v.workspace.settings.title = "My Mod";
	v.revisions.touch(ViewConcern::Workspace);
	ui.windows.set_view(&v);
	ui.frames(3);
	ui.away();
	CHECK(logged_frame(ui).find("A mod (MOD) on novaworld.test, revision 3") != std::string::npos,
	      "settings: its NovaWorld project and revision");
	v.novaworld.key.clear();
	v.novaworld.tracked = false;
	v.novaworld.put.kind = "game";
	v.novaworld.put.game = "jo";
	ProjectDocument doc;
	doc.title = "My Mod";
	v.project.document = std::make_shared<const ProjectDocument>(doc);
	ui.frames(2);
	ui.away();
	CHECK(logged_frame(ui).find("Not on NovaWorld.") != std::string::npos, "settings: not on NovaWorld");
	ui.activate(item_id(ImHashStr("Project settings"), {"Put on NovaWorld..."}));
	ui.frames(2);
	CHECK(one(ui.drain(), EditorRequestKind::NovaWorldCheck), "settings: Put on NovaWorld... asks the lists again");
	ui.activate(put_item({"Put"}));
	const std::vector<EditorRequest> raised = ui.drain();
	const EditorRequest *made = one(raised, EditorRequestKind::NovaWorldPut);
	CHECK(made && made->create && made->project_key == "MYMOD", "settings: Put on NovaWorld... puts it there");
	ui.activate(put_item({"Cancel"}));
	ui.drain();
	ui.activate(item_id(ImHashStr("Project settings"), {"Cancel"}));
	ui.drain();
	std::printf("content: Project settings' NovaWorld section\n");
}

// The New project form's A mod of (ADR 0074): a mod is of one of NovaWorld's games, and the form names no base game's
// project. Signed out, it says to sign in, its Sign in... bringing the NovaWorld window forward, and Create waits;
// signed in, NovaWorld's games listed, one chosen the form's base_key; a retail game's mod (JO, which the account
// does not see listed) waits for an install of its game; Create raises new_project naming the game by base_key.
void test_mod_form() {
	SessionView v;
	v.novaworld.available = true;
	v.novaworld.server = "https://novaworld.test";
	WorkspaceView::NewProject &form = v.workspace.new_project;
	form.making = WorkspaceView::Making::Mod;
	form.title = "Night Ops";
	form.expansion = "nightops";
	form.dir = "C:/mods/Night Ops";
	Ui ui;
	ui.serve_workspace = false; // the set_workspace requests looked at, the view set by hand
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.away();
	ui.drain();
	const ImGuiID document = item_id(Ui::window_id("Document"), {"welcome"});
	std::string text = logged_frame(ui);
	CHECK(text.find("Sign in to NovaWorld to make a project") != std::string::npos &&
	              text.find("Base game's project") == std::string::npos && text.find("A project of yours") == std::string::npos,
	      "signed out: sign in to make a mod, no base game's project to name");
	ui.activate(item_id(document, {"Make the project"}));
	CHECK(ui.drain().empty(), "signed out: Create waits");
	ui.activate(item_id(document, {"Sign in...##new_project"}));
	std::vector<EditorRequest> raised = ui.drain();
	CHECK(raised.size() == 1 && raised[0].kind == EditorRequestKind::SetWorkspace &&
	              raised[0].workspace.find("novaworld") != std::string::npos,
	      "Sign in...: the NovaWorld window brought forward");

	// Signed in: NovaWorld's games, one chosen.
	v.novaworld.signed_in = true;
	v.novaworld.username = "player";
	v.novaworld.role = "player";
	v.novaworld.games = {{"ACIDGAMBIT", "OP: Acid Gambit", "jo"}, {"JO", "Joint Operations", "jo"}};
	RemoteProject acid;
	acid.key = "ACIDGAMBIT";
	acid.title = "OP: Acid Gambit";
	acid.kind = "game";
	acid.game = "jo";
	acid.is_public = true;
	acid.head = 3;
	v.novaworld.projects = {acid}; // JO a retail game's, private: not listed
	ui.frames(2);
	ui.away();
	CHECK(logged_frame(ui).find("Choose its game") != std::string::npos, "signed in: A mod of asks for a game");
	ui.activate(item_id(document, {"Make the project"}));
	CHECK(ui.drain().empty(), "no game chosen: Create waits");
	ui.activate(item_id(document, {"A mod of"}));
	ui.activate(item_id(ImHashStr("##Combo_00"), {"Joint Operations (JO)"}));
	raised = ui.drain();
	CHECK(raised.size() == 1 && workspace_member(raised[0], "new_project", "base_key").string == "JO",
	      "a game chosen: its key the form's");
	ImGui::ClosePopupsExceptModals();
	form.base_key = "JO";
	ui.frames(2);
	ui.away();
	CHECK(logged_frame(ui).find("Needs a Joint Operations install") != std::string::npos,
	      "a retail game's mod: an install of its game named first");
	ui.activate(item_id(document, {"Make the project"}));
	CHECK(ui.drain().empty(), "no install of its game: Create waits");
	// A game NovaWorld lets the account get: no install asked, and Create makes the mod of it.
	form.base_key = "ACIDGAMBIT";
	ui.frames(2);
	ui.away();
	CHECK(logged_frame(ui).find("Game install") == std::string::npos, "a mod of a game NovaWorld gives: no install asked");
	ui.activate(item_id(document, {"Make the project"}));
	raised = ui.drain();
	const EditorRequest *made = one(raised, EditorRequestKind::NewProject);
	CHECK(made && made->base_key == "ACIDGAMBIT" && made->expansion == "nightops" && made->base_project.empty() &&
	              made->title == "Night Ops" && made->dir == "C:/mods/Night Ops" && made->game_install.empty(),
	      "Create: new_project of the mod, its game by key, no install");

	// Set up... of a mod made on the website (ADR 0074): everything it says filled, only the folder asked; the folder
	// beside the last one named; Create with nothing to fill.
	RemoteProject web;
	web.key = "WEBMOD";
	web.title = "Web Mod";
	web.kind = "expansion";
	web.game = "jo";
	web.base = "ACIDGAMBIT";
	web.expansion_code = "webmod";
	web.owner = "player";
	web.can_push = true;
	v.novaworld.projects = {acid, web};
	v.project.recent_projects = {"C:/mods/Armory"};
	ui.serve_workspace = true;
	auto *nw = const_cast<devtools::Window *>(find_window(ui.windows.pass(), "NovaWorld"));
	CHECK(nw != nullptr, "the NovaWorld window");
	if (!nw) return;
	nw->show_anyway();
	ui.focus("NovaWorld");
	ui.activate(item_id(Ui::window_id("NovaWorld"), {"novaworld_yours", "WEBMOD", "Set up..."}));
	ui.drain();
	CHECK(form.making == WorkspaceView::Making::Mod && form.base_key == "ACIDGAMBIT" && form.expansion == "webmod" &&
	              form.title == "Web Mod" && form.novaworld == "WEBMOD" && form.dir == "C:/mods/WEBMOD",
	      "Set up...: the form filled with what the project says, its folder beside the last");
	ui.focus("Document");
	ui.frames(2);
	ui.away();
	text = logged_frame(ui);
	CHECK(text.find("A mod of OP: Acid Gambit (ACIDGAMBIT), as webmod, named Web Mod") != std::string::npos &&
	              text.find("Short name") == std::string::npos && text.find("A game you own") == std::string::npos &&
	              text.find("Game install") == std::string::npos,
	      "set up: what it makes in a line, nothing it says asked again, no install");
	ui.activate(item_id(document, {"Make the project"}));
	raised = ui.drain();
	made = one(raised, EditorRequestKind::NewProject);
	CHECK(made && made->project_key == "WEBMOD" && made->base_key == "ACIDGAMBIT" && made->expansion == "webmod" &&
	              made->dir == "C:/mods/WEBMOD",
	      "set up: Create with nothing to fill");
	std::printf("content: the New project form's A mod of, signed out and in; Set up... of a website's mod\n");
}

// What NovaWorld is doing (draw_novaworld_activity): a running pull's phase in words, its bar and its Cancel, which
// raises novaworld_cancel; the same line in the status bar with no project open; a call waiting on NovaWorld said; a
// stopped pull's Give up the pull.
void test_activity() {
	Ui ui;
	SessionView v = tracked_view();
	OperationStatus &running = v.activity.operation;
	running.id = 9;
	running.kind = OperationKind::NovaWorldPull;
	running.label = "Downloading 3 of 10 files (1.0 MiB of 4.0 MiB)";
	running.done = 1 << 20;
	running.total = 4 << 20;
	running.unit = OperationUnit::Bytes;
	running.cancellable = true;
	ui.windows.set_view(&v);
	ui.focus("NovaWorld");
	ui.frames(2);
	ui.away();
	std::string text = logged_frame(ui);
	CHECK(text.find("Downloading 3 of 10 files (1.0 MiB of 4.0 MiB)") != std::string::npos,
	      "a pull's phase in words, its files and bytes counted");
	ui.activate(novaworld_item({"Cancel##novaworld_activity"}));
	CHECK(one(ui.drain(), EditorRequestKind::NovaWorldCancel), "its Cancel: one novaworld_cancel");
	// No project open (a get from the welcome page): the status bar says the same.
	v.project.open = false;
	v.revisions.touch(ViewConcern::Project);
	ui.frames(2);
	ui.away();
	CHECK(logged_frame(ui).find("Downloading 3 of 10 files") != std::string::npos, "no project open: the bar's line");
	v.project.open = true;
	v.revisions.touch(ViewConcern::Project);
	// A call waiting on NovaWorld.
	running = OperationStatus();
	v.novaworld.waiting = "Putting MYMOD on NovaWorld...";
	ui.frames(2);
	ui.away();
	CHECK(logged_frame(ui).find("Putting MYMOD on NovaWorld...") != std::string::npos, "a put waiting: said");
	v.novaworld.waiting.clear();
	// A pull stopped on a conflict: Give up the pull.
	v.novaworld.conflicts = {{"defs/a.def", NovaWorldChange::Modified, NovaWorldChange::Deleted}};
	ui.frames(2);
	ui.activate(novaworld_item({"Give up the pull"}));
	CHECK(one(ui.drain(), EditorRequestKind::NovaWorldCancel), "Give up the pull: one novaworld_cancel");
	ui.activate(novaworld_item({"Keep all mine"}));
	std::vector<EditorRequest> settled = ui.drain();
	CHECK(settled.size() == 1 && settled[0].kind == EditorRequestKind::NovaWorldResolve &&
	              settled[0].resolution == Resolution::KeepMine,
	      "Keep all mine: one novaworld_resolve a conflict");
	ui.away();
	text = logged_frame(ui);
	CHECK(text.find("Remove") != std::string::npos, "an added file's Discard is a Remove");
	std::printf("content: what NovaWorld is doing, its Cancel, the bar's line, a waiting call, Give up the pull\n");
}

} // namespace

void run_novaworld_tests() {
	test_working_copy();
	test_put_and_sign_in();
	test_get_a_project();
	test_settings_section();
	test_mod_form();
	test_activity();
}

} // namespace editor_ui_test
