// The NovaWorld window and NovaWorld's place in the workspace (ADR 0069): over a hand-made view, what the
// window shows of a working copy (its revision, the store's, its changes) and the status bar's words for it; a
// push's message typed and Push raising novaworld_push, refused while there is nothing to push; a conflict's Keep
// mine; an account that may not push the project shown a read-only line, a player's own project its Push and no
// Release; a project not on NovaWorld, Put on NovaWorld... raising novaworld_put for a new project of the
// account's (its name and key from the project's title) or an empty one it may push, and no Bind anywhere; a
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

// The store's MOD, a game: at revision 5, public, the account may push it.
RemoteProject mod_project() {
	RemoteProject mod;
	mod.key = "MOD";
	mod.title = "A mod";
	mod.kind = "game";
	mod.game = "jo";
	mod.is_public = true;
	mod.head = 5;
	mod.can_push = true;
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

	// Release r3 waits for a version; typed, it raises novaworld_release with it.
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
	ui.frames(2);
	ui.away();
	std::string read_only = logged_frame(ui);
	CHECK(read_only.find("Read-only") != std::string::npos, "may not push: a read-only line");
	CHECK(read_only.find("Release r3") == std::string::npos, "a player: no Release");
	ui.activate(novaworld_item({"Push"}));
	CHECK(ui.drain().empty(), "a read-only project's Push raises nothing");
	// A player's own project: Push, still no Release.
	v.novaworld.remote.can_push = true;
	ui.frames(2);
	ui.away();
	read_only = logged_frame(ui);
	CHECK(read_only.find("Read-only") == std::string::npos && read_only.find("Release r3") == std::string::npos,
	      "a player's own project: no read-only line, no Release");
	ui.activate(novaworld_item({"Push"}));
	CHECK(one(ui.drain(), EditorRequestKind::NovaWorldPush), "a player's own project: Push");

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
	// Put on NovaWorld...: a new project of the account's, its name and key from the project's title.
	ui.activate(novaworld_item({"Put on NovaWorld..."}));
	ui.frames(2);
	ui.away();
	text = logged_frame(ui);
	CHECK(text.find("A game of Joint Operations") != std::string::npos, "what it makes, shown");
	ui.activate(put_item({"Put"}));
	std::vector<EditorRequest> raised = ui.drain();
	const EditorRequest *made = one(raised, EditorRequestKind::NovaWorldPut);
	CHECK(made && made->create && made->project_key == "MYMOD" && made->title == "My Mod" &&
	              made->visibility == "private",
	      "Put: one novaworld_put of a new private project, keyed from its name");
	// An empty project the account may push, chosen: Put takes it.
	c.projects.front().head = 0;
	ui.activate(novaworld_item({"Put on NovaWorld..."}));
	ui.frames(2);
	ui.activate(put_item({"An empty project you may push"}));
	ui.frames(2);
	ui.activate(put_item({"Put"}));
	raised = ui.drain();
	const EditorRequest *taken = one(raised, EditorRequestKind::NovaWorldPut);
	CHECK(taken && !taken->create && taken->project_key == "MOD", "Put: one novaworld_put of the empty MOD");
	c.projects.front().head = 5;

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
	ui.activate(novaworld_item({"Account"}));
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
	CHECK(home.find("Yours") != std::string::npos && home.find("Everyone's") != std::string::npos &&
	              home.find("Theirs  THEIRS r5") != std::string::npos && home.find("Mine  EMPTY, empty") != std::string::npos,
	      "in two groups, yours and everyone's");
	// The NovaWorld window shown over the welcome page lists them too; Get... asks for a folder.
	auto *content = const_cast<devtools::Window *>(find_window(ui.windows.pass(), "NovaWorld"));
	CHECK(content != nullptr, "the NovaWorld window");
	if (!content) return;
	content->show_anyway();
	ui.focus("NovaWorld");
	ui.activate(novaworld_item({"novaworld_yours", "MOD", "Get..."}));
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
	ui.activate(novaworld_item({"novaworld_everyones", "THEIRS", "Get..."}));
	CHECK(one(ui.drain(), EditorRequestKind::PickDirectory), "another's project: Get...");
	ui.windows.deliver_pick(PickPurpose::NovaWorldFolder, "C:/mods");
	ui.drain();
	// Set up...: the New project form filled for the empty project, a mod of its code (the workspace's, as the
	// session takes the set_workspace).
	ui.activate(novaworld_item({"novaworld_yours", "EMPTY", "Set up..."}));
	ui.drain();
	const WorkspaceView::NewProject &form = v.workspace.new_project;
	CHECK(form.novaworld == "EMPTY" && form.making == WorkspaceView::Making::Mod && form.expansion == "mine" &&
	              form.title == "Mine" && form.installed.empty() && !form.on_project,
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
	ui.activate(item_id(welcome, {"Create project"}));
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
	ui.activate(put_item({"Put"}));
	const std::vector<EditorRequest> raised = ui.drain();
	const EditorRequest *made = one(raised, EditorRequestKind::NovaWorldPut);
	CHECK(made && made->create && made->project_key == "MYMOD", "settings: Put on NovaWorld... puts it there");
	ui.activate(item_id(ImHashStr("Project settings"), {"Cancel"}));
	ui.drain();
	std::printf("content: Project settings' NovaWorld section\n");
}

} // namespace

void run_novaworld_tests() {
	test_working_copy();
	test_put_and_sign_in();
	test_get_a_project();
	test_settings_section();
}

} // namespace editor_ui_test
