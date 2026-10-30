// The menu shell (menu/menu_shell.h) headless over its runtime and a fake host: the game's menu mode
// and each open's wiring, the one the game's binding and the editor's Try mode run.
//
// Pinned: the menu mode's enter shows the main menu's first screen, a mission's return the screen it
// was started from (D-MNU-28), an enter that left no mission the main menu again; a jump to another
// file pushes the screen it leaves and the back pops it, a jump to a file or a screen the game lacks
// changes nothing; the main menu's EXIT and its back with nothing behind quit, the in-game back
// resumes, CONFIRM_YES and RESTART answer only in a mission; ACCEPT starts the selected mission and
// with none starts nothing; the Mods pick of the game running takes nothing, another saves the profile
// and reloads the game at the frame's tail in the original's order (the jar saved, the gate response
// forgotten, the archives mounted, the shell's resources read again, the history dropped, the main
// menu shown), a loose mount refuses it; a URL row saves the jar and opens the browser, one without
// the external browser fetches the page; the update gate shows UPDATE and disables the multiplayer
// entries only while an update waits, and UPDATE starts the updater then quits; the Options dialog's
// edits apply at once, the front BACK puts the visit's baseline back, the in-game OK takes a new
// baseline and its Cancel restores it; a screen controller owns the documents its owner names, is
// released when another opens, hears the events only while it holds the document, and a claim holds
// a document its embedder opened itself.
#include <formats/mnu/mnu.h>
#include <runtime/menu/menu_shell.h>
#include <runtime/menu/menu_state_frame.h>

#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace opennova;
using namespace opennova::menu;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

std::string window(const char *type, const char *name, int top, const std::string &inner = std::string()) {
	char position[160];
	std::snprintf(position, sizeof(position),
			"<POSITION><LEFT>100</LEFT><TOP>%d</TOP><RIGHT>300</RIGHT><BOTTOM>%d</BOTTOM></POSITION>", top, top + 20);
	return std::string("<WINDOW type=\"") + type + "\" name=\"" + name + "\">" + inner + position + "</WINDOW>\n";
}

std::string screen(const char *name, const std::string &windows) {
	return std::string("<SCREEN><NAME>") + name +
	       "</NAME><WINDOW type=\"window\" name=\"MAIN\"><POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT>"
	       "<BOTTOM>600</BOTTOM></POSITION>\n" +
	       windows + "</WINDOW></SCREEN>\n";
}

const char *kPop = "<ACTION type=\"pop_screen\"></ACTION>";

std::map<std::string, std::string> menus() {
	std::map<std::string, std::string> out;
	out["main.mnu"] =
			screen("STARTUP", window("button", "GO", 0, "<ACTION type=\"screen\" file=\"sp.mnu\">SINGLE_PLAYER</ACTION>") +
			                          window("button", "SECOND_GO", 20, "<ACTION type=\"screen\" file=\"main.mnu\">SECOND</ACTION>") +
			                          window("button", "EXIT", 40) + window("button", "HIDDEN_BACK", 60) +
			                          window("button", "NW_MULTI_PLAYER", 80) + window("button", "LAN_MULTI_PLAYER", 100) +
			                          window("button", "UPDATE", 120) +
			                          window("button", "WEB", 140, "<ACTION type=\"url\" EXTERNAL_BROWSER>www.example.com</ACTION>") +
			                          window("button", "PAGE", 160, "<ACTION type=\"url\">news.html</ACTION>") +
			                          window("button", "NOWHERE", 180, "<ACTION type=\"screen\" file=\"missing.mnu\">X</ACTION>") +
			                          window("button", "NOSCREEN", 200, "<ACTION type=\"screen\" file=\"sp.mnu\">NOPE</ACTION>") +
			                          window("button", "MODS", 220, "<ACTION type=\"screen\" file=\"mods.mnu\">MODS</ACTION>") +
			                          window("button", "OPTS", 240, "<ACTION type=\"screen\" file=\"opt.mnu\">OPTIONS</ACTION>") +
			                          window("button", "PLAYER", 260, "<ACTION type=\"screen\" file=\"player.mnu\">PLAYER_INFO</ACTION>") +
			                          window("button", "APP", 280, "<ACTION type=\"appmsg\">UPDATE</ACTION>")) +
			screen("SECOND", window("button", "BACK", 0, kPop));
	out["sp.mnu"] = screen("SINGLE_PLAYER", window("list", "IA_LIST", 0) + window("static", "BRIEFING", 40) +
	                                                window("button", "ACCEPT", 80) + window("button", "BACK", 120, kPop));
	out["game.mnu"] = screen("INGAME", window("button", "HIDDEN_BACK", 0) + window("button", "CONFIRM_YES", 20) +
	                                           window("button", "RESTART", 40) + window("button", "EXIT", 60));
	out["mods.mnu"] = screen("MODS", window("list", "AVAIL_LIST", 0) + window("multiline_edit", "MOD_DESC", 40) +
	                                         window("button", "ACCEPT", 80));
	out["opt.mnu"] = screen("OPTIONS", window("scroll", "SOUNDFXVOLUME", 0) + window("button", "BACK", 40, kPop));
	out["dialog.mnu"] = screen("INGAME", window("scroll", "SOUNDFXVOLUME", 0) + window("button", "OPT_ACCEPT", 40) +
	                                             window("button", "OPT_CANCEL", 60));
	out["player.mnu"] = screen("PLAYER_INFO", window("combo", "NATIONALITY", 0) + window("combo", "COMBO_LIST", 40) +
	                                                  window("button", "ACCEPT", 80) + window("button", "BACK", 120, kPop));
	return out;
}

struct Host : MenuShellHost {
	std::map<std::string, std::string> files = menus();
	std::vector<std::string> log; // what the shell asked, in order
	std::string running;
	bool packed = true;
	bool waiting = false;
	bool update_starts = false;
	bool mount_ok = true;
	OptionsScreen::Words words{};
	bool has_words = true;

	std::shared_ptr<const mnu::Document> read_menu(const std::string &file) override {
		const auto found = files.find(file);
		if (found == files.end()) return nullptr;
		auto doc = std::make_shared<mnu::Document>();
		std::string error;
		if (!mnu::parse(found->second, *doc, error)) return nullptr;
		return doc;
	}
	std::vector<MissionChoice> missions() override {
		MissionChoice alpha;
		alpha.file = "alpha.bms";
		alpha.text = "Alpha";
		alpha.briefing = "Go north.";
		alpha.game_type = 0x10020u; // a single-player (waypoint family) mission
		return { alpha };
	}
	std::vector<ExpansionRecord> expansions() override {
		ExpansionRecord record;
		record.directory = "nifty";
		record.info.name = "OP: Nifty";
		record.info.description = "A storm.";
		return { record };
	}
	std::string expansion() override { return running; }
	bool packed_install() override { return packed; }
	bool update_waiting() override { return waiting; }
	bool player_words(OptionsScreen::Words &out) override {
		if (!has_words) return false;
		out = words;
		return true;
	}
	void set_player_words(const OptionsScreen::Words &in) override { words = in; }
	void leave(ShellExit exit, MenuCommand, const std::string &, const std::string &text) override {
		static const char *const kNames[] = { "start", "exit", "return", "restart", "resume", "novaworld" };
		log.push_back(std::string(kNames[int(exit)]) + (text.empty() ? "" : ":" + text));
	}
	bool launch_update(const std::string &) override {
		log.push_back("launch_update");
		return update_starts;
	}
	void open_browser(const std::string &url) override { log.push_back("browser:" + url); }
	void fetch_page(const std::string &url) override { log.push_back("page:" + url); }
	void notice(ShellNotice notice, const std::string &, const std::string &text, const std::string &) override {
		log.push_back("notice:" + std::to_string(int(notice)) + ":" + text);
	}
	void save_profile() override { log.push_back("save_profile"); }
	void save_cookie_jar() override { log.push_back("save_cookie_jar"); }
	void forget_gate_response() override { log.push_back("forget_gate"); }
	bool mount_game(const std::string &name, const std::string &previous) override {
		log.push_back("mount:" + name + "<-" + previous);
		if (mount_ok) running = name;
		return mount_ok;
	}
	void reload_shell_resources() override { log.push_back("resources"); }
	void game_reloaded() override { log.push_back("reloaded"); }

	bool logged(const std::string &line) const {
		for (const std::string &held : log)
			if (held == line) return true;
		return false;
	}
};

struct Controller : MenuScreenController {
	std::string name;
	std::vector<std::string> calls;
	explicit Controller(const char *owner) : name(owner) {}
	const char *owner() const override { return name.c_str(); }
	void open(MenuRuntime &, const std::string &file, const std::string &screen) override {
		calls.push_back("open:" + file + ":" + screen);
	}
	void on_event(MenuRuntime &menu, const MenuEvent &event) override {
		if (event.kind == MenuEvent::Kind::WidgetActivated) calls.push_back("activated:" + menu.widget_name_of(event.id));
	}
	void close(MenuRuntime &) override { calls.push_back("close"); }
};

struct Run {
	Host host;
	MenuShell shell;
	MenuStateFrame frame;

	Run() {
		frame.set_configure([this](const std::string &name, MenuFrameCompiler &compiler) {
			const mnu::Document *doc = shell.runtime().document();
			const mnu::Screen *found = !doc ? nullptr : name.empty() ? doc->first_screen() : doc->find_screen(name);
			compiler.configure(found);
			return found != nullptr;
		});
		frame.set_clicked([this](int index, int part) { shell.runtime().on_widget_clicked(index, part); });
		frame.set_scrolled([this](int index, int value) { shell.runtime().on_frame_scroll_value(index, value); });
		shell.runtime().set_frame(&frame);
		shell.set_host(&host);
	}
	MenuRuntime &rt() { return shell.runtime(); }
	int id(const char *name) { return rt().widget_id(name); }
	void press(const char *name) { rt().activate(id(name)); }
	std::string at() { return shell.file() + ":" + rt().current_screen(); }
	std::string last() { return host.log.empty() ? std::string() : host.log.back(); }
};

void test_menu_mode_and_the_history() {
	Run run;
	CHECK(run.shell.enter_menu_mode(true));
	CHECK(run.at() == "main.mnu:STARTUP");
	// A jump to another file: the screen it leaves goes on the history; its back pops it.
	run.press("GO");
	CHECK(run.at() == "sp.mnu:SINGLE_PLAYER" && run.rt().screen_history().size() == 1);
	run.press("BACK");
	CHECK(run.at() == "main.mnu:STARTUP" && run.rt().screen_history().size() == 0);
	// A file or a screen the game lacks changes nothing: the screen and the history stay.
	run.press("NOWHERE");
	CHECK(run.at() == "main.mnu:STARTUP" && run.rt().screen_history().size() == 0);
	CHECK(run.last() == "notice:" + std::to_string(int(ShellNotice::MenuMissing)) + ":missing.mnu");
	run.press("NOSCREEN");
	CHECK(run.at() == "main.mnu:STARTUP" && run.rt().screen_history().size() == 0);
	CHECK(run.last() == "notice:" + std::to_string(int(ShellNotice::ScreenMissing)) + ":sp.mnu");
	// A POP_SCREEN with nothing behind does nothing.
	run.press("SECOND_GO");
	CHECK(run.at() == "main.mnu:SECOND");
	run.press("BACK");
	CHECK(run.at() == "main.mnu:STARTUP");
	run.host.log.clear();
	run.shell.runtime().show_screen("SECOND");
	run.press("BACK");
	CHECK(run.at() == "main.mnu:SECOND" && run.host.log.empty());
	run.shell.runtime().show_screen("STARTUP");
	// A mission started from SINGLE_PLAYER: the return shows it again, the history under it kept.
	run.press("GO");
	run.shell.leave_menu_mode();
	run.shell.leave_menu_mode(); // once per stay
	CHECK(run.shell.enter_menu_mode(false));
	CHECK(run.at() == "sp.mnu:SINGLE_PLAYER" && run.rt().screen_history().size() == 1);
	// An enter with no mission left behind opens the main menu, its history empty.
	CHECK(run.shell.enter_menu_mode(false));
	CHECK(run.at() == "main.mnu:STARTUP" && run.rt().screen_history().size() == 0);
}

void test_quit_resume_and_the_missions_commands() {
	Run run;
	run.shell.enter_menu_mode(true);
	run.press("EXIT");
	CHECK(run.last() == "exit:the main menu's EXIT");
	run.press("HIDDEN_BACK");
	CHECK(run.last() == "exit:the main menu's BACK with no screen behind it");
	run.press("NW_MULTI_PLAYER");
	CHECK(run.last() == "novaworld");
	// The in-game menu: its back resumes; CONFIRM_YES returns to the menu, RESTART restarts.
	CHECK(run.shell.open_ingame() && run.shell.in_game() && run.at() == "game.mnu:INGAME");
	run.press("HIDDEN_BACK");
	CHECK(run.last() == "resume");
	run.press("CONFIRM_YES");
	CHECK(run.last() == "return");
	run.press("RESTART");
	CHECK(run.last() == "restart");
	// The front end binds neither.
	run.shell.enter_menu_mode(false);
	CHECK(!run.shell.in_game());
	run.host.log.clear();
	run.shell.open_menu("game.mnu", "");
	run.press("CONFIRM_YES");
	run.press("RESTART");
	CHECK(run.host.log.empty());
}

void test_the_single_player_launch() {
	Run run;
	run.shell.enter_menu_mode(true);
	run.press("GO");
	const int list = run.id("IA_LIST");
	CHECK(run.rt().item_count(list) == 1);
	CHECK(run.rt().is_widget_disabled(run.id("ACCEPT")));
	// ACCEPT with nothing picked starts nothing.
	run.press("ACCEPT");
	CHECK(run.last() == "notice:" + std::to_string(int(ShellNotice::NothingSelected)) + ":");
	// A pick selects its mission, shows its briefing and enables ACCEPT, which starts it.
	run.rt().select_row(list, 0, true);
	CHECK(run.shell.selected_mission() == "alpha.bms");
	CHECK(!run.rt().is_widget_disabled(run.id("ACCEPT")));
	run.press("ACCEPT");
	CHECK(run.last() == "start:alpha.bms");
	// The next open clears the pick.
	run.shell.open_menu("main.mnu", "");
	CHECK(run.shell.selected_mission().empty());
}

void test_the_mods_pick_and_its_reload() {
	Run run;
	run.shell.enter_menu_mode(true);
	run.press("MODS");
	const int list = run.id("AVAIL_LIST");
	CHECK(run.rt().item_count(list) == 2 && run.rt().selected_row(list) == 0);
	// ACCEPT on the game running takes nothing.
	run.press("ACCEPT");
	CHECK(run.last() == "notice:" + std::to_string(int(ShellNotice::ExpansionUnchanged)) + ":");
	CHECK(!run.shell.has_pending_reload());
	// A pick shows its description; ACCEPT saves the profile and queues the switch, nothing launched.
	run.rt().select_row(list, 1, true);
	CHECK(run.rt().get_widget_text(run.id("MOD_DESC")) == "A storm.");
	run.host.log.clear();
	run.press("ACCEPT");
	CHECK(run.shell.has_pending_reload());
	CHECK(run.host.log.size() == 2 && run.host.log[0] == "save_profile");
	// The frame's tail reloads in the original's order, the main menu shown with an empty history.
	run.host.log.clear();
	run.shell.frame_tail();
	CHECK(!run.shell.has_pending_reload());
	CHECK(run.host.log.size() == 5 && run.host.log[0] == "save_cookie_jar" && run.host.log[1] == "forget_gate" &&
	      run.host.log[2] == "mount:nifty<-" && run.host.log[3] == "resources" && run.host.log[4] == "reloaded");
	CHECK(run.at() == "main.mnu:STARTUP" && run.rt().screen_history().size() == 0);
	// A loose mount takes no expansion.
	Run loose;
	loose.host.packed = false;
	loose.shell.enter_menu_mode(true);
	loose.press("MODS");
	loose.rt().select_row(loose.id("AVAIL_LIST"), 1, true);
	loose.press("ACCEPT");
	CHECK(!loose.shell.has_pending_reload());
	CHECK(loose.last() == "notice:" + std::to_string(int(ShellNotice::ExpansionNeedsPackedInstall)) + ":nifty");
	// A failed mount keeps the menu where it is.
	Run failed;
	failed.host.mount_ok = false;
	failed.shell.enter_menu_mode(true);
	failed.press("MODS");
	failed.rt().select_row(failed.id("AVAIL_LIST"), 1, true);
	failed.press("ACCEPT");
	failed.shell.frame_tail();
	CHECK(failed.at() == "mods.mnu:MODS" && !failed.host.logged("reloaded"));
}

void test_urls_and_the_updater() {
	Run run;
	run.shell.enter_menu_mode(true);
	run.host.log.clear();
	run.press("WEB");
	CHECK(run.host.log.size() == 2 && run.host.log[0] == "save_cookie_jar" && run.host.log[1] == "browser:www.example.com");
	run.press("PAGE");
	CHECK(run.last() == "page:news.html");
	// No update waits: UPDATE is hidden and inert, the multiplayer entries untouched.
	const int update = run.id("UPDATE"), nw = run.id("NW_MULTI_PLAYER"), lan = run.id("LAN_MULTI_PLAYER");
	CHECK(!run.rt().is_widget_shown(update) && run.rt().is_widget_disabled(update));
	CHECK(!run.rt().is_widget_disabled(nw) && !run.rt().is_widget_disabled(lan));
	// One waits: UPDATE shows, the multiplayer entries wait for it, NovaWorld's press does nothing.
	run.host.waiting = true;
	run.shell.apply_update_gate();
	CHECK(run.rt().is_widget_shown(update) && !run.rt().is_widget_disabled(update));
	CHECK(run.rt().is_widget_disabled(nw) && run.rt().is_widget_disabled(lan));
	run.host.log.clear();
	run.shell.runtime().activate(nw);
	CHECK(!run.host.logged("novaworld"));
	// UPDATE starts the updater; the game quits once it started, a failed start keeps the menu.
	run.press("UPDATE");
	CHECK(run.last() == "notice:" + std::to_string(int(ShellNotice::UpdateNotStarted)) + ":");
	run.host.update_starts = true;
	run.press("UPDATE");
	CHECK(run.last() == "exit:the updater started");
	// An APPMSG row's UPDATE does the same.
	run.host.log.clear();
	run.press("APP");
	CHECK(run.host.logged("launch_update"));
	// The answer back to none: the gate's disables lift, and only its own.
	run.host.waiting = false;
	run.shell.apply_update_gate();
	CHECK(!run.rt().is_widget_disabled(nw) && !run.rt().is_widget_disabled(lan));
	CHECK(!run.rt().is_widget_shown(update));
}

void test_the_options_baseline() {
	Run run;
	run.host.words[size_t(OptionsWord::SoundFxVolume)] = 40;
	run.shell.enter_menu_mode(true);
	run.press("OPTS");
	CHECK(run.shell.options().is_surface() && run.at() == "opt.mnu:OPTIONS");
	const int volume = run.id("SOUNDFXVOLUME");
	// An edit applies at once (D-MNU-19).
	run.rt().on_frame_scroll_value(run.rt().frame_index(volume), 100);
	CHECK(run.host.words[size_t(OptionsWord::SoundFxVolume)] == 100);
	// The front BACK keeps no edit: the visit's baseline back.
	run.press("BACK");
	CHECK(run.host.words[size_t(OptionsWord::SoundFxVolume)] == 40);
	CHECK(run.at() == "main.mnu:STARTUP");
	// The in-game dialog: OK takes what it holds as the next baseline, Cancel puts the baseline back.
	run.shell.open_ingame();
	run.shell.open_menu("dialog.mnu", "");
	CHECK(run.shell.options().is_surface());
	run.rt().on_frame_scroll_value(run.rt().frame_index(run.id("SOUNDFXVOLUME")), 70);
	run.press("OPT_ACCEPT");
	CHECK(run.host.words[size_t(OptionsWord::SoundFxVolume)] == 70);
	run.rt().on_frame_scroll_value(run.rt().frame_index(run.id("SOUNDFXVOLUME")), 10);
	CHECK(run.host.words[size_t(OptionsWord::SoundFxVolume)] == 10);
	run.press("OPT_CANCEL");
	CHECK(run.host.words[size_t(OptionsWord::SoundFxVolume)] == 70);
}

void test_screen_controllers() {
	Run run;
	Controller lan("lan"), player("player_info");
	run.shell.add_controller(&lan);
	run.shell.add_controller(&player);
	run.shell.enter_menu_mode(true);
	CHECK(lan.calls.empty() && player.calls.empty());
	// The document whose owner names PLAYER_INFO is its controller's: opened after the wiring.
	run.press("PLAYER");
	CHECK(player.calls.size() == 1 && player.calls[0] == "open:player.mnu:PLAYER_INFO" && lan.calls.empty());
	CHECK(run.shell.controller() == &player);
	// It hears its document's events after the shell; the shell runs no delegate command of its own.
	run.host.log.clear();
	run.press("ACCEPT");
	CHECK(player.calls.back() == "activated:ACCEPT" && run.host.log.empty());
	// The back opens another document: released, and the jump's own events are not its.
	run.press("BACK");
	CHECK(player.calls.back() == "close" && run.shell.controller() == nullptr);
	const size_t heard = player.calls.size();
	run.press("EXIT");
	CHECK(player.calls.size() == heard);
	// A claim holds a document its embedder opened itself (no shell wiring answers it).
	mnu::Document weapon;
	std::string error;
	CHECK(mnu::parse(screen("WEAPON", window("button", "ACCEPT", 0) + window("button", "EXIT", 20)), weapon, error));
	Controller armory("");
	CHECK(run.shell.show_document(&weapon, "weapon.mnu", ""));
	CHECK(run.shell.claim(&armory) && armory.calls.size() == 1 && armory.calls[0] == "open:weapon.mnu:WEAPON");
	run.host.log.clear();
	run.press("EXIT");
	CHECK(armory.calls.back() == "activated:EXIT" && run.host.log.empty());
	run.shell.release();
	CHECK(armory.calls.back() == "close" && run.shell.controller() == nullptr);
}

} // namespace

int main() {
	test_menu_mode_and_the_history();
	test_quit_resume_and_the_missions_commands();
	test_the_single_player_launch();
	test_the_mods_pick_and_its_reload();
	test_urls_and_the_updater();
	test_the_options_baseline();
	test_screen_controllers();
	if (failures) {
		std::printf("menu_shell: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("menu_shell OK\n");
	return 0;
}
