// A menu tried in its preview (DI-35; editor/preview/menu_try, the menu viewport's Try mode): the picture
// behaving as the game's menu through the runtime's own driver, over a session's project (headless: no device
// follows the viewport, a SetViewport's sample follows it). Menus written here, a bank minted through
// lwf::encode_lwf and a short wave, a mission from the fixtures with a text table minted through rtxt::write.
//
// Pinned: Try on from the screen the viewport shows; a click runs a button's SCREEN row into another menu (the
// screen it leaves on the history, the breadcrumb, the runtime's own sounds heard), Esc reaches the hotkey
// whose POP_SCREEN comes back through the history; a same-file jump; a check box and an edit field change in
// the sandbox alone (the settings list; the document untouched); EXIT, a URL and a jump to a menu the project
// lacks are said, not done; the single-player screen's list holds the project's missions, a pick enables its
// ACCEPT and ACCEPT says the mission it would start; Reset; the canvas's click refused while trying; Try off
// is the editor's picture again; refused changes (a click or a key with Try off, a key no name gives). A Mods
// screen (D-MNU-31) lists the base game's row, then the expansion folders of the game install the project plays
// over by their names in the game's order, the base game highlighted; a pick shows its description, ACCEPT on the
// game running takes nothing and on another says the switch.
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <base/io/strutil.h>
#include <editor/assets/asset_registry.h>
#include <editor/documents/mnu_document.h>
#include <editor/preview/menu_try.h>
#include <editor/preview/menu_viewport.h>
#include <editor/preview/viewport_json.h>
#include <editor/preview/viewports.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/rtxt/rtxt.h>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova;
using namespace opennova::editor;
using opennova::io::JsonValue;

namespace {

std::string repo() { return test_paths_repo_root(__FILE__); }

// A button at (l, t, r, b) holding `inner` (its ACTION rows, SOUND rows, HOTKEY).
std::string button(const char *type, const char *name, int l, int t, int r, int b, const std::string &inner) {
	char position[160];
	std::snprintf(position, sizeof(position),
			"<POSITION><LEFT>%d</LEFT><TOP>%d</TOP><RIGHT>%d</RIGHT><BOTTOM>%d</BOTTOM></POSITION>", l, t, r, b);
	return std::string("<WINDOW type=\"") + type + "\" name=\"" + name + "\">" + inner + position + "</WINDOW>\n";
}

std::string screen(const char *name, const std::string &windows) {
	return std::string("<SCREEN><NAME>") + name +
	       "</NAME><WINDOW type=\"window\" name=\"MAIN\"><POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT>"
	       "<BOTTOM>600</BOTTOM></POSITION>\n" +
	       windows + "</WINDOW></SCREEN>\n";
}

const char *kBack = "<HOTKEY VIRTUAL>VK_ESCAPE</HOTKEY><ACTION type=\"pop_screen\"></ACTION>";

std::string main_menu() {
	return screen("STARTUP",
	               button("button", "GO", 100, 100, 300, 140,
	                       "<ACTION type=\"screen\" file=\"other.mnu\">OTHER</ACTION>"
	                       "<SOUND state=\"mousein\" trigger=\"OVER\">menu.lwf</SOUND>"
	                       "<SOUND state=\"selected\" trigger=\"CLICK\">menu.lwf</SOUND>") +
	                       button("button", "SAME", 100, 150, 300, 190,
	                               "<ACTION type=\"screen\" file=\"main.mnu\">SECOND</ACTION>") +
	                       button("checkbox", "CHECK", 100, 200, 300, 240, "") +
	                       button("edit", "NAME", 100, 250, 300, 290, "") +
	                       button("button", "EXIT", 400, 100, 600, 140, "") +
	                       button("button", "PLAY", 400, 150, 600, 190,
	                               "<ACTION type=\"screen\" file=\"sp.mnu\">SINGLE_PLAYER</ACTION>") +
	                       button("button", "WEB", 400, 200, 600, 240,
	                               "<ACTION type=\"url\" EXTERNAL_BROWSER>www.example.com</ACTION>") +
	                       button("button", "NOWHERE", 400, 250, 600, 290,
	                               "<ACTION type=\"screen\" file=\"missing.mnu\">X</ACTION>") +
	                       button("button", "OPTS", 400, 300, 600, 340,
	                               "<ACTION type=\"screen\" file=\"opt.mnu\">OPTIONS</ACTION>")) +
	       screen("SECOND", button("button", "BACK", 100, 500, 300, 540, kBack));
}

std::string other_menu() {
	return screen("OTHER", button("button", "BACK", 100, 500, 300, 540, kBack));
}

std::string sp_menu() {
	// The list's rows are its font's height: the project's built-in font.
	return screen("SINGLE_PLAYER", button("list", "IA_LIST", 100, 100, 500, 300, "<FONT><NAME>Arial12b.fnt</NAME></FONT>") +
	                                       button("button", "ACCEPT", 100, 400, 300, 440, "") +
	                                       button("button", "BACK", 400, 400, 600, 440, kBack));
}

// An options surface: a witnessed slider and the controls table, which the game fills with its bindings.
std::string options_menu() {
	return screen("OPTIONS", button("scroll", "SOUNDFXVOLUME", 100, 100, 400, 120, "") +
	                                 button("table", "CONTROL_MAPPING", 100, 200, 700, 400,
	                                         "<FONT><NAME>Arial12b.fnt</NAME></FONT>") +
	                                 button("button", "BACK", 100, 500, 300, 540, kBack));
}

std::vector<uint8_t> mission_text() {
	rtxt::File text;
	text.sections.push_back({ "Info", 2 });
	text.entries.push_back({ "TITLE", "Try Mission", {}, 0 });
	text.entries.push_back({ "BRIEFING", "Go.", {}, 0 });
	std::vector<uint8_t> out;
	std::string error;
	rtxt::write(text, out, error);
	return out;
}

struct TryProject {
	editor_test::TempProjectDir dir{"opennova_editor_menu_try"};
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	std::string root;
	std::string path;
	std::string written;
	bool made = false;

	TryProject() {
		session.handle(request::new_project(dir.file("project"), "Menu Try"));
		session.run_operations();
		editor_test::create_missing_files(session);
		root = session.view().project.root;
		const AssetEntry *menu = session.view().project.scan->named("main.mnu");
		path = menu ? menu->relative_path : std::string();
		written = main_menu();
		bool ok = menu && editor_test::write_text(root + "/" + path, written);
		ok = ok && editor_test::write_text(root + "/other.mnu", other_menu());
		ok = ok && editor_test::write_text(root + "/sp.mnu", sp_menu());
		ok = ok && editor_test::write_text(root + "/opt.mnu", options_menu());
		ok = ok && editor_test::write_bytes(root + "/sounds/menu.lwf", editor_test::sound_bank_of({"OVER", "CLICK"}));
		const std::vector<uint8_t> tone = test_io::read_file(repo() + "/fixtures/lwf/tone.wav");
		for (const char *wave : {"over", "click"}) ok = ok && editor_test::write_bytes(root + "/sounds/" + wave + ".wav", tone);
		ok = ok && editor_test::write_bytes(root + "/trymission.bms", test_io::read_file(repo() + "/fixtures/bms/synth_logic.bms"));
		ok = ok && editor_test::write_bytes(root + "/trymission.bin", mission_text());
		editor_test::handle_to_end(session, request::rescan());
		while (session.view().activity.validation.running) session.poll();
		session.handle(request::open_document(path));
		made = ok && session.view().documents.previews[ViewportKind::Menu].path == path;
	}
	const MenuViewport *viewport() {
		return dynamic_cast<const MenuViewport *>(session.viewports().find(path, ViewportKind::Menu));
	}
	bool change(const std::string &member, const std::string &value) {
		session.handle(request::set_viewport(path, "{\"" + member + "\": " + value + "}"));
		return session.outcome().done();
	}
	bool click(float x, float y) {
		char text[64];
		std::snprintf(text, sizeof(text), R"({"at": [%g, %g]})", x, y);
		return change("click", text);
	}
	bool key(const std::string &name) { return change("key", R"({"key": ")" + name + R"("})"); }
	JsonValue json() {
		ViewportModel *model = session.viewports().follow_one(session.view(), path, ViewportKind::Menu);
		return model ? viewport_to_json(session.view(), *model, JsonPage()) : JsonValue();
	}
	JsonValue tried() {
		const JsonValue shown = json();
		const JsonValue *body = shown.get("body");
		const JsonValue *member = body ? body->get("try") : nullptr;
		return member ? *member : JsonValue();
	}
	// Where Try is: "file:SCREEN".
	std::string at() {
		const JsonValue member = tried();
		return member.get_string("file", "") + ":" + member.get_string("screen", "");
	}
	// The item of the screen Try shows named `name` (null: none).
	JsonValue item(const char *name) {
		const JsonValue shown = json();
		if (const JsonValue *items = shown.get("items"))
			for (const JsonValue &widget : items->array)
				if (widget.get_string("name", "") == name) return widget;
		return JsonValue();
	}
	std::string last_outcome() {
		const JsonValue member = tried();
		const JsonValue *last = member.get("last");
		return last && last->is_object() ? last->get_string("kind", "") + ": " + last->get_string("words", "") : "";
	}
	std::string setting(const char *control) {
		const JsonValue member = tried();
		if (const JsonValue *settings = member.get("settings"))
			for (const JsonValue &row : settings->array)
				if (row.get_string("control", "") == control) return row.get_string("kind", "") + "=" + row.get_string("value", "");
		return std::string();
	}
	// The window sounds fired after `seq`, spelled "GO:SELECTED:CLICK" (the bank's sets have no member: each
	// is fired and says it is silent).
	std::vector<std::string> fired_after(uint64_t seq) {
		std::vector<std::string> out;
		if (const MenuViewport *menu = viewport())
			for (const MenuSoundFired &fired : menu->sounds_fired())
				if (fired.sound.seq > seq)
					out.push_back(fired.name + ":" + menu::menu_sound_state_token(fired.state) + ":" + fired.sound.set);
		return out;
	}
};

int test_try_navigates() {
	TryProject project;
	TEST_EXPECT(project.made);
	if (!project.made) return 1;
	TEST_EXPECT(project.json().get("body")->get("try")->get_bool("on", true) == false);
	// A click while Try is off is the editor's: refused, pointing at Try.
	TEST_EXPECT(!project.click(200, 120));
	TEST_EXPECT(project.change("try", R"({"on": true})"));
	TEST_EXPECT(project.at() == "main.mnu:STARTUP");
	TEST_EXPECT(project.viewport()->trying() && project.viewport()->caption() == " - STARTUP (Try)");

	// GO jumps to another menu: the screen it leaves on the history, both in the breadcrumb, its sounds the
	// runtime's own pump's (MOUSEIN as the mouse comes on, SELECTED on the click).
	const uint64_t before = project.session.viewports().clip_sound_seq();
	TEST_EXPECT(project.click(200, 120));
	TEST_EXPECT(project.at() == "other.mnu:OTHER");
	JsonValue member = project.tried();
	TEST_EXPECT(member.get("breadcrumb")->array.size() == 2 && member.get("history")->array.size() == 1 &&
	            member.get("history")->array[0].get_string("screen", "") == "STARTUP");
	const std::vector<std::string> fired = project.fired_after(before);
	TEST_EXPECT(fired.size() >= 2 && fired[0] == "GO:MOUSEIN:OVER" && fired[1] == "GO:SELECTED:CLICK");
	TEST_EXPECT(project.viewport()->caption() == " - OTHER of other.mnu (Try)");
	// Esc: BACK's hotkey, its POP_SCREEN past the file's own history back through the screens' history.
	TEST_EXPECT(project.key("VK_ESCAPE"));
	TEST_EXPECT(project.at() == "main.mnu:STARTUP");
	TEST_EXPECT(project.tried().get("history")->array.empty());
	// A jump within the file, and back by its own history (Escape by its plain name).
	TEST_EXPECT(project.click(200, 170) && project.at() == "main.mnu:SECOND");
	TEST_EXPECT(project.key("Escape") && project.at() == "main.mnu:STARTUP");

	// A check box and an edit field: the sandbox's alone.
	TEST_EXPECT(project.click(200, 220));
	TEST_EXPECT(project.item("CHECK").get_bool("checked", false));
	TEST_EXPECT(project.setting("CHECK") == "checkbox=checked");
	TEST_EXPECT(project.click(200, 270));
	TEST_EXPECT(project.tried().get_string("focus", "") == "NAME");
	TEST_EXPECT(project.change("key", R"({"text": "Joe"})"));
	TEST_EXPECT(project.item("NAME").get_string("text", "") == "Joe");
	TEST_EXPECT(project.setting("NAME") == "edit=Joe");
	TEST_EXPECT(project.key("VK_RETURN") && project.tried().get("focus")->is_null());
	const auto *document = dynamic_cast<const MnuDocument *>(project.session.document_for(project.path));
	TEST_EXPECT(document && !document->dirty());
	TEST_EXPECT(test_io::read_file_text(project.root + "/" + project.path) == project.written);

	// What leaves the menu is said, not done; a menu the project lacks changes nothing.
	TEST_EXPECT(project.click(500, 120) && project.at() == "main.mnu:STARTUP");
	TEST_EXPECT(project.last_outcome() == "exit: the game would quit to the desktop");
	TEST_EXPECT(project.click(500, 220));
	TEST_EXPECT(project.last_outcome() == "url: the game would open www.example.com in the web browser");
	TEST_EXPECT(project.click(500, 270) && project.at() == "main.mnu:STARTUP");
	TEST_EXPECT(project.last_outcome() ==
	            "missing_menu: the game would not find missing.mnu, so the screen would stay");

	// The single-player screen: its list the project's missions, ACCEPT held until a pick.
	TEST_EXPECT(project.click(500, 170) && project.at() == "sp.mnu:SINGLE_PLAYER");
	const JsonValue list = project.item("IA_LIST");
	TEST_EXPECT(list.get("items") && list.get("items")->array.size() == 1 &&
	            list.get("items")->array[0].string == "*Try Mission");
	TEST_EXPECT(project.item("ACCEPT").get_bool("disabled", false));
	// The list's first row: a pick selects its mission and enables ACCEPT.
	TEST_EXPECT(project.click(200, 105));
	TEST_EXPECT(project.item("IA_LIST").get_number("selected", -1) == 0);
	TEST_EXPECT(!project.item("ACCEPT").get_bool("disabled", true));
	TEST_EXPECT(project.click(200, 420));
	TEST_EXPECT(project.last_outcome() == "start_mission: the game would start trymission.bms");
	TEST_EXPECT(project.at() == "sp.mnu:SINGLE_PLAYER");

	// An options surface: its slider at the game's range from its minimum, its controls table holding the game's
	// default bindings (the sandbox's: the user's are never read).
	TEST_EXPECT(project.key("VK_ESCAPE") && project.at() == "main.mnu:STARTUP");
	TEST_EXPECT(project.click(500, 320) && project.at() == "opt.mnu:OPTIONS");
	const JsonValue volume = project.item("SOUNDFXVOLUME");
	TEST_EXPECT(volume.get_number("value", -1) == 0 && volume.get("range") && volume.get("range")->array.size() == 2 &&
	            volume.get("range")->array[1].number == 255);
	const JsonValue table = project.item("CONTROL_MAPPING");
	TEST_EXPECT(table.get("rows") && table.get("rows")->array.size() > 10 &&
	            table.get("rows")->array[0].array.size() == 3);
	TEST_EXPECT(project.key("VK_ESCAPE") && project.at() == "main.mnu:STARTUP");
	TEST_EXPECT(project.click(500, 170) && project.at() == "sp.mnu:SINGLE_PLAYER");

	// The canvas's click is refused while trying; the hit names the game's window there.
	ViewportCommand canvas_click;
	canvas_click.name = "click";
	canvas_click.has_at = true;
	canvas_click.at_x = 10.0f;
	canvas_click.at_y = 10.0f;
	project.session.handle(request::edit_in_viewport(project.path, canvas_click));
	TEST_EXPECT(!project.session.outcome().done());
	const MenuViewport *menu = project.viewport();
	const ViewportHit hit = menu->hit(viewport_context(project.session.view(), *menu, 0.0f), 200, 420);
	TEST_EXPECT(hit.name == "ACCEPT" && hit.id == 0); // another menu's window: no record of this one

	// Reset: back where it started, the sandbox dropped.
	TEST_EXPECT(project.change("try", R"({"reset": true})"));
	TEST_EXPECT(project.at() == "main.mnu:STARTUP");
	member = project.tried();
	TEST_EXPECT(member.get("settings")->array.empty() && member.get("outcomes")->array.empty() &&
	            member.get("breadcrumb")->array.size() == 1);
	TEST_EXPECT(!project.item("CHECK").get_bool("checked", true));
	// On this menu's screen, a window answers with its record.
	const ViewportHit own = menu->hit(viewport_context(project.session.view(), *menu, 0.0f), 200, 120);
	TEST_EXPECT(own.name == "GO" && own.id != 0);
	// An edit while trying: the game's menu read again where it is, the edit in it; the history kept.
	TEST_EXPECT(project.click(200, 170) && project.at() == "main.mnu:SECOND");
	EditorRequest edit = request::edit_record(project.path, Edit());
	edit.edits[0].address = document->address_of(own.id);
	edit.edits[0].field = "string.value";
	edit.edits[0].value = std::string("ONWARD");
	project.session.handle(edit);
	TEST_EXPECT(project.session.outcome().done());
	TEST_EXPECT(project.at() == "main.mnu:SECOND");
	TEST_EXPECT(project.key("VK_ESCAPE") && project.at() == "main.mnu:STARTUP");
	TEST_EXPECT(project.item("GO").get_string("text", "") == "ONWARD");

	// Refused: a key no name gives, a click off the picture, a try member of another type.
	TEST_EXPECT(!project.key("NO_SUCH_KEY") && !project.click(900, 10) && !project.change("try", R"({"on": 1})"));
	// Off: the editor's picture again; a key is refused.
	TEST_EXPECT(project.change("try", R"({"on": false})"));
	TEST_EXPECT(!project.viewport()->trying() && project.viewport()->caption() == " - STARTUP");
	TEST_EXPECT(project.json().get("body")->get("try")->get_bool("on", true) == false);
	TEST_EXPECT(!project.key("VK_RETURN"));
	return 0;
}

std::vector<uint8_t> exp_info(const char *name, const char *description) {
	rtxt::File text;
	text.sections.push_back({ "exp_info", 2 });
	text.entries.push_back({ "EXP_NAME", name, {}, 0 });
	text.entries.push_back({ "EXP_DESC", description, {}, 0 });
	std::vector<uint8_t> out;
	std::string error;
	rtxt::write(text, out, error);
	return out;
}

int test_try_lists_the_mods() {
	TryProject project;
	TEST_EXPECT(project.made);
	if (!project.made) return 1;
	// The game install the project plays over: two expansion folders, one named by its loose <n>.bin.
	const std::string install = project.dir.file("install");
	bool ok = editor_test::write_bytes(install + "/expansion/onjo1/onjo1.bin", exp_info("OpenNova: Indigo Storm", "A storm."));
	ok = ok && editor_test::write_bytes(install + "/expansion/jox01/version.txt", { '1' });
	const std::string mods = screen("OPTIONS",
			button("list", "AVAIL_LIST", 100, 100, 390, 300,
					"<FONT><NAME>Arial12b.fnt</NAME></FONT><MIN_ITEM_HEIGHT>20</MIN_ITEM_HEIGHT>") +
					button("multiline_edit", "MOD_DESC", 410, 100, 700, 300, "") +
					button("button", "ACCEPT", 510, 400, 700, 440, ""));
	ok = ok && editor_test::write_text(project.root + "/mods.mnu", mods);
	TEST_EXPECT(ok);
	editor_test::handle_to_end(project.session, request::open_project(project.root, false, install));
	while (project.session.view().activity.validation.running) project.session.poll();
	TEST_EXPECT(project.session.view().project.mods_list.size() == 2);
	const AssetEntry *menu = project.session.view().project.scan->named("mods.mnu");
	TEST_EXPECT(menu != nullptr);
	if (!menu) return 1;
	project.path = menu->relative_path;
	project.session.handle(request::open_document(project.path));
	TEST_EXPECT(project.change("try", R"({"on": true})"));
	TEST_EXPECT(project.at() == "mods.mnu:OPTIONS");
	const JsonValue list = project.item("AVAIL_LIST");
	const JsonValue *items = list.get("items");
	TEST_EXPECT(items && items->array.size() == 3);
	if (!items || items->array.size() != 3) return 1;
	TEST_EXPECT(items->array[0].string == "Joint Operations: Typhoon Rising");
	TEST_EXPECT(items->array[1].string == "Unnamed Expansion" && items->array[2].string == "OpenNova: Indigo Storm");
	TEST_EXPECT(list.get_number("selected", -1) == 0);
	// ACCEPT on the game running takes nothing.
	TEST_EXPECT(project.click(600, 420));
	TEST_EXPECT(project.last_outcome() ==
	            "nothing: the game would take nothing: Joint Operations: Typhoon Rising is the game running");
	// A pick of the third row: its description; ACCEPT says the switch.
	TEST_EXPECT(project.click(200, 150));
	TEST_EXPECT(project.item("AVAIL_LIST").get_number("selected", -1) == 2);
	TEST_EXPECT(project.item("MOD_DESC").get_string("text", "") == "A storm.");
	TEST_EXPECT(project.click(600, 420));
	TEST_EXPECT(project.last_outcome() ==
	            "apply_expansion: the game would switch to OpenNova: Indigo Storm (expansion\\onjo1) for this run, "
	            "reloading everything and showing its main menu");
	return 0;
}

int test_key_names() {
	menu::MenuKeyInput key;
	TEST_EXPECT(menu_try_key_from_name("vk_return", false, key) && key.vk == 13 && key.unicode == 0);
	TEST_EXPECT(menu_try_key_from_name("Escape", false, key) && key.vk == 27);
	TEST_EXPECT(menu_try_key_from_name("VK_SPACE", false, key) && key.vk == 32 && key.unicode == ' ');
	TEST_EXPECT(menu_try_key_from_name("a", true, key) && key.vk == 'A' && key.unicode == 'a' && key.shift);
	TEST_EXPECT(menu_try_key_from_name("7", false, key) && key.vk == '7' && key.unicode == '7');
	TEST_EXPECT(menu_try_key_from_name("=", false, key) && key.vk == 0 && key.unicode == '=');
	TEST_EXPECT(!menu_try_key_from_name("VK_F1", false, key) && !menu_try_key_from_name("", false, key));
	return 0;
}

} // namespace

int main() {
	TEST_EXPECT(test_try_navigates() == 0);
	TEST_EXPECT(test_try_lists_the_mods() == 0);
	TEST_EXPECT(test_key_names() == 0);
	std::printf("editor_menu_try OK\n");
	return 0;
}
