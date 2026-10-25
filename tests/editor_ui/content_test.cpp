// The Content window and the content store's place in the workspace (ADR 0069): over a hand-made view, what the
// window shows of a bound project (its revision, the store's, its changes) and the status bar's words for it; a
// push's message typed and Push raising content_push, refused while there is nothing to push; a conflict's Keep
// mine; an admin's Bind of an unbound project; the sign-in's form raising content_sign_in with what was typed; the
// store's projects listed with no project open, Get... asking for a folder and the pick getting the project into
// a folder of its key's name; the welcome page's section; nothing past its room.
#include <cstring>
#include <string>
#include <vector>

#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/editor_windows.h>

#include "editor_ui_test_support.h"

#include <imgui.h>

namespace editor_ui_test {

namespace {

// A project bound to the store's MOD at revision 3, the store at 5, two files changed, an admin signed in.
SessionView bound_view() {
	SessionView v;
	v.project.open = true;
	v.project.root = "C:/mods/Mod";
	ContentView &c = v.content;
	c.available = true;
	c.server = "https://content.test";
	c.username = "taylor";
	c.role = "admin";
	c.signed_in = true;
	c.bound = true;
	c.key = "MOD";
	c.revision = 3;
	c.checked = true;
	c.head_known = true;
	c.head = 5;
	c.changes = {{"defs/a.def", ContentChange::Modified}, {"models/b.3di", ContentChange::Added}};
	RemoteProject mod;
	mod.key = "MOD";
	mod.title = "A mod";
	mod.is_public = true;
	mod.head = 5;
	mod.can_push = true;
	c.projects = {mod};
	return v;
}

ImGuiID content_item(std::initializer_list<const char *> steps) { return item_id(Ui::window_id("Content"), steps); }

void test_bound_project() {
	Ui ui;
	SessionView v = bound_view();
	ui.windows.set_view(&v);
	ui.focus("Content");
	ui.away();
	const std::string text = logged_frame(ui);
	CHECK(text.find("taylor (admin) on content.test") != std::string::npos, "who is signed in, and where");
	CHECK(text.find("MOD r3, r5 in the store") != std::string::npos, "the revision and the store's newest");
	CHECK(text.find("Changes (2)") != std::string::npos && text.find("modified  defs/a.def") != std::string::npos &&
	              text.find("added  models/b.3di") != std::string::npos,
	      "the changes, each with what it did");
	CHECK(text.find("MOD r3, 2 changed, 2 incoming") != std::string::npos, "the status bar's standing");
	for (const std::string &offender : overflowing()) CHECK(false, ("bound: " + offender).c_str());

	// Push waits for a message; typed, it raises content_push with it.
	ui.activate(content_item({"Push"}));
	CHECK(ui.drain().empty(), "no message: Push raises nothing");
	ui.activate(content_item({"##message"}));
	ImGui::GetIO().AddInputCharactersUTF8("Fix the armory");
	ui.frames(2);
	ui.activate(content_item({"Push"}));
	std::vector<EditorRequest> raised = ui.drain();
	const EditorRequest *push = one(raised, EditorRequestKind::ContentPush);
	CHECK(push && push->message == "Fix the armory", "Push: one content_push with the message");

	// Check and Pull.
	ui.activate(content_item({"Check"}));
	CHECK(one(ui.drain(), EditorRequestKind::ContentCheck), "Check: one content_check");
	ui.activate(content_item({"Pull"}));
	CHECK(one(ui.drain(), EditorRequestKind::ContentPull), "Pull: one content_pull");

	// A pull stopped on a conflict: Keep mine settles it, Pull and Push wait for it.
	v.content.conflicts = {{"defs/a.def", ContentChange::Modified, ContentChange::Deleted}};
	ui.frames(2);
	ui.away();
	CHECK(logged_frame(ui).find("Conflicts (1)") != std::string::npos, "the conflicts listed");
	ui.activate(content_item({"conflicts", "defs/a.def", "Keep mine"}));
	raised = ui.drain();
	const EditorRequest *keep = one(raised, EditorRequestKind::ContentResolve);
	CHECK(keep && keep->path == "defs/a.def" && keep->resolution == Resolution::KeepMine,
	      "Keep mine: one content_resolve of the file");
	ui.activate(content_item({"Pull"}));
	ui.activate(content_item({"Push"}));
	CHECK(ui.drain().empty(), "Pull and Push wait for the conflict");
	v.content.conflicts.clear();

	// Signed in as a player: no message box, no Push.
	v.content.role = "player";
	ui.frames(2);
	ui.away();
	CHECK(logged_frame(ui).find("Only an admin pushes.") != std::string::npos, "a player: only an admin pushes");
	ui.activate(content_item({"Push"}));
	CHECK(ui.drain().empty(), "a player's Push raises nothing");
	std::printf("content: a bound project, its changes and the bar's words, Push with its message, Keep mine\n");
}

void test_bind_and_sign_in() {
	Ui ui;
	SessionView v = bound_view();
	v.content.bound = false;
	v.content.checked = false;
	v.content.changes.clear();
	ui.windows.set_view(&v);
	ui.focus("Content");
	ui.away();
	const std::string text = logged_frame(ui);
	CHECK(text.find("Not in the content store.") != std::string::npos, "an unbound project says so");
	CHECK(text.find("MOD r") == std::string::npos, "the bar says nothing of the store");
	ui.activate(content_item({"Bind"}));
	std::vector<EditorRequest> raised = ui.drain();
	const EditorRequest *bind = one(raised, EditorRequestKind::ContentBind);
	CHECK(bind && bind->project_key == "MOD", "Bind: one content_bind of the project listed");

	// Signed out: the form, its server the view's, and what was typed.
	v.content.signed_in = false;
	v.content.role.clear();
	ui.frames(2);
	ui.activate(content_item({"Sign in..."}));
	ui.frames(2);
	ui.activate(content_item({"Account"}));
	ImGui::GetIO().AddInputCharactersUTF8("taylor");
	ui.frames(2);
	ui.activate(content_item({"Password"}));
	ImGui::GetIO().AddInputCharactersUTF8("pw");
	ui.frames(2);
	ui.activate(content_item({"Sign in"}));
	raised = ui.drain();
	const EditorRequest *sign_in = one(raised, EditorRequestKind::ContentSignIn);
	CHECK(sign_in && sign_in->server == "https://content.test" && sign_in->username == "taylor" &&
	              sign_in->password == "pw",
	      "Sign in: one content_sign_in with the server, the account and the password");
	ui.away();
	CHECK(logged_frame(ui).find("pw") == std::string::npos, "the password is never drawn");
	std::printf("content: an admin's Bind, the sign-in's form\n");
}

void test_get_a_project() {
	Ui ui;
	SessionView v = bound_view();
	v.project.open = false;
	v.project.root.clear();
	ui.windows.set_view(&v);
	ui.frames(3);
	ui.away();
	const std::string home = logged_frame(ui);
	CHECK(home.find("Get a project from NovaWorld") != std::string::npos && home.find("A mod  MOD r5") != std::string::npos,
	      "the welcome page lists the store's projects");
	// The Content window shown over the welcome page lists them too; Get... asks for a folder.
	auto *content = const_cast<devtools::Window *>(find_window(ui.windows.pass(), "Content"));
	CHECK(content != nullptr, "the Content window");
	if (!content) return;
	content->show_anyway();
	ui.focus("Content");
	ui.activate(content_item({"content_projects", "MOD", "Get..."}));
	std::vector<EditorRequest> raised = ui.drain();
	const EditorRequest *pick = one(raised, EditorRequestKind::PickDirectory);
	CHECK(pick && pick->purpose == PickPurpose::ContentFolder && pick->project_key.empty(),
	      "Get...: one pick of a folder, the key kept by the windows");
	ui.windows.deliver_pick(PickPurpose::ContentFolder, "C:/mods");
	raised = ui.drain();
	const EditorRequest *get = one(raised, EditorRequestKind::ContentGet);
	CHECK(get && get->project_key == "MOD" && get->dir == "C:/mods/MOD", "the pick: the project got into C:/mods/MOD");
	ui.windows.deliver_pick(PickPurpose::ContentFolder, "C:/other");
	CHECK(ui.drain().empty(), "a second pick with no Get... before it gets nothing");
	std::printf("content: the store's projects on the welcome page, Get... and its folder\n");
}

} // namespace

void run_content_tests() {
	test_bound_project();
	test_bind_and_sign_in();
	test_get_a_project();
}

} // namespace editor_ui_test
