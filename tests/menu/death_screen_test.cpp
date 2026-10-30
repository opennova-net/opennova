// The DEATH screen's controller (menu/death_screen.h) run headless over the runtime and the state
// frame, a death.mnu-shaped document and a fake session (the DeathScreenHost).
//
// Pinned: the show (the team-service pair by the session's rule, the shroud hidden, the pass-through
// word raised); the frame's verdicts (a dead session's teardown, the close only on a drawn scene
// frame once the triggers fell, else the reveal) and the refresh inside the reveal on a 16-tick
// boundary (the Default row's Menu tokens, the zone names through WPNames, the rows' tags
// stripped); the pick kept across a refill by its parameter when the rows reshuffle; a list pick
// under the initial overlay closing the screen and a death re-pick keeping it; an occupant row
// (node -1) never picking; HIDDEN_BACK and CONFIRM_NO lowering and raising the word; CONFIRM_YES's
// close then exit; SWAP_TEAMS; the statics (the instructions, the queued line, the hold, the medic
// pair, permanent death's round status and no refill).
// [orig: UI_RegisterDeathScreenCallbacks @0x554610; DeathScreen_UpdateShroudReveal @0x554730;
//  UI_UpdateDeathScreenContent @0x5536a0; DeathScreen_OnSpawnListSelect @0x553630]
#include <formats/mnu/mnu.h>
#include <runtime/menu/death_screen.h>
#include <runtime/menu/menu_runtime.h>
#include <runtime/menu/menu_state_frame.h>

#include <cstdio>
#include <cstring>
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

std::string wnd(const char *type, const char *name, int top, const char *inner = "") {
	char position[200];
	std::snprintf(position, sizeof(position),
			"<POSITION><LEFT>20</LEFT><TOP>%d</TOP><RIGHT>280</RIGHT><BOTTOM>%d</BOTTOM></POSITION>", top,
			top + 18);
	return std::string("<WINDOW type=\"") + type + "\" name=\"" + name + "\">" + inner + position + "</WINDOW>\n";
}

// death.mnu's named controls with its exit confirmation: HIDDEN_BACK in DEATH_SHROUD (Escape shows
// CONFIRM_EXIT and hides the shroud), the HIDDEN MODAL CONFIRM_EXIT holding CONFIRM_YES and CONFIRM_NO.
std::string death_menu() {
	std::string body = wnd("list", "SPAWNPOINTS_LIST", 40);
	int top = 200;
	for (const char *name : { "STATIC_LIST_TITLE", "STATIC_RESPAWN_MSG1", "STATIC_PSPRESPAWN_MSG1",
			     "STATIC_MEDIC_MSG1", "STATIC_CALLMEDIC_MSG", "STATIC_INSTRUCTIONS_MSG",
			     "STATIC_INSTRUCTIONS2_MSG", "SWAP_TEAMS", "BUTTON_TEAMLIST" }) {
		body += wnd(std::strcmp(name, "SWAP_TEAMS") == 0 ? "button" : "static", name, top);
		top += 20;
	}
	body += "<WINDOW type=\"window\" name=\"DEATH_SHROUD\"><POSITION><LEFT>300</LEFT><TOP>20</TOP><RIGHT>780</RIGHT>"
	        "<BOTTOM>545</BOTTOM></POSITION>" +
	        wnd("window", "MAP", 10) +
	        wnd("button", "HIDDEN_BACK", 480,
	                "<HOTKEY VIRTUAL>VK_ESCAPE</HOTKEY><ACTION type=\"WINDOW\" state=\"SHOW\">CONFIRM_EXIT</ACTION>"
	                "<ACTION type=\"WINDOW\" state=\"HIDE\">DEATH_SHROUD</ACTION>") +
	        "</WINDOW>\n";
	body += "<WINDOW type=\"window\" name=\"CONFIRM_EXIT\" HIDDEN MODAL><POSITION><LEFT>250</LEFT><TOP>200</TOP>"
	        "<RIGHT>550</RIGHT><BOTTOM>300</BOTTOM></POSITION>" +
	        wnd("button", "CONFIRM_YES", 10,
	                "<HOTKEY VIRTUAL>VK_RETURN</HOTKEY><ACTION type=\"WINDOW\" state=\"HIDE\">CONFIRM_EXIT</ACTION>") +
	        wnd("button", "CONFIRM_NO", 40,
	                "<HOTKEY VIRTUAL>VK_ESCAPE</HOTKEY><ACTION type=\"WINDOW\" state=\"SHOW\">DEATH_SHROUD</ACTION>"
	                "<ACTION type=\"WINDOW\" state=\"HIDE\">CONFIRM_EXIT</ACTION>") +
	        "</WINDOW>\n";
	return "<SCREEN><NAME>DEATH</NAME><WINDOW type=\"window\" name=\"MAIN\"><POSITION><LEFT>0</LEFT><TOP>0</TOP>"
	       "<RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>\n" +
	       body + "</WINDOW></SCREEN>\n";
}

struct FakeSession final : DeathScreenHost {
	bool is_held = true, pending = false, dead = false, lost = false, revealed = false, sends = true;
	int64_t tick = 0;
	world::DeployScreenStatus st;
	std::vector<world::DeployListRow> compiled;
	std::string default_key, default_home;
	std::vector<std::string> zone_names; // the zone_name answers the builder asked for
	std::vector<int> picks;
	int team_changes = 0;
	std::vector<std::string> requests; // "close", "exit" in order

	bool held() override { return is_held; }
	bool pick_pending() override { return pending; }
	bool local_player_dead() override { return dead; }
	bool session_lost() override { return lost; }
	bool shroud_revealed() override { return revealed; }
	int64_t logic_tick() override { return tick; }
	world::DeployScreenStatus status() override { return st; }
	std::vector<world::DeployListRow> list_rows(const std::string &key, const std::string &home,
			const std::function<std::string(const std::string &)> &zone_name) override {
		default_key = key;
		default_home = home;
		zone_names = { zone_name("STRWPNAME001"), zone_name("STRWPNAME009"), zone_name("") };
		return compiled;
	}
	bool send_pick(int param) override {
		picks.push_back(param);
		return sends;
	}
	void send_team_change() override { ++team_changes; }
	void close_screen() override { requests.push_back("close"); }
	void exit_confirmed() override { requests.push_back("exit"); }
};

// The tables' strings: WPNames in gametext, DEFAULT_SPAWN_KEY in menutxt, HOME only in Game.bin.
hud::GameTextLookup table(const char *section, const char *key, const char *text) {
	return [section, key, text](const char *s, const char *k, const char *fallback) {
		return std::strcmp(s, section) == 0 && std::strcmp(k, key) == 0 ? std::string(text) : std::string(fallback);
	};
}

struct Run {
	mnu::Document doc;
	MenuStateFrame frame;
	MenuRuntime rt;
	FakeSession session;
	DeathScreen screen;
	bool claimed = false;

	Run() { screen.set_host(&session); }
	bool open() {
		std::string error;
		if (!mnu::parse(death_menu(), doc, error)) return false;
		frame.set_configure([this](const std::string &name, MenuFrameCompiler &compiler) {
			const mnu::Screen *found = name.empty() ? doc.first_screen() : doc.find_screen(name);
			compiler.configure(found);
			return found != nullptr;
		});
		frame.set_clicked([this](int index, int part) { rt.on_widget_clicked(index, part); });
		rt.set_frame(&frame);
		// The shell's forwarding while the controller holds the document.
		rt.set_sink([this](const MenuEvent &e) {
			if (claimed) screen.on_event(rt, e);
		});
		screen.set_text(table("WPNames", "STRWPNAME001", "Bridge"), table("Menu", "DEFAULT_SPAWN_KEY", "X"),
				table("Menu", "HOME", "Base"));
		if (!rt.open_document(&doc, DeathScreen::kFile, DeathScreen::kScreen)) return false;
		claimed = true;
		screen.open(rt, DeathScreen::kFile, rt.current_screen());
		return true;
	}
	int id(const char *name) const { return rt.widget_id(name); }
	bool shown(const char *name) const { return rt.is_widget_shown(id(name)); }
};

world::DeployListRow row(const char *text, int value) {
	world::DeployListRow out;
	out.text = text;
	out.value = value;
	return out;
}

void test_the_show_hides_the_shroud_and_raises_the_word() {
	Run run;
	run.session.st.team_buttons_shown = true;
	run.session.tick = 5;
	CHECK(run.screen.can_open());
	CHECK(!run.screen.pass_through());
	CHECK(run.open());
	CHECK(!run.shown("DEATH_SHROUD"));
	CHECK(run.shown("SWAP_TEAMS") && run.shown("BUTTON_TEAMLIST"));
	CHECK(run.screen.pass_through());
	CHECK(run.screen.rows().empty());
	run.session.is_held = false;
	CHECK(!run.screen.can_open());
	// Off the session's rule the pair hides at the next show.
	Run other;
	other.session.st.team_buttons_shown = false;
	CHECK(other.open());
	CHECK(!other.shown("SWAP_TEAMS") && !other.shown("BUTTON_TEAMLIST"));
}

void test_the_reveal_and_the_refresh() {
	Run run;
	run.session.tick = 5;
	run.session.compiled = { row("<c4040FF>'X' Base", 0), row("<c4040FF>'A' Bridge", 1), row("", -1) };
	CHECK(run.open());
	// Not revealed: nothing shows, nothing fills.
	run.session.tick = 20;
	CHECK(run.screen.frame(run.rt, true) == DeathScreen::Frame::Keep);
	CHECK(!run.shown("DEATH_SHROUD") && run.screen.rows().empty());
	// Revealed between boundaries: the shroud shows, the content waits for the 16-tick boundary.
	run.session.revealed = true;
	run.session.tick = 21;
	CHECK(run.screen.frame(run.rt, true) == DeathScreen::Frame::Keep);
	CHECK(run.shown("DEATH_SHROUD") && run.screen.rows().empty());
	run.session.tick = 32;
	CHECK(run.screen.frame(run.rt, true) == DeathScreen::Frame::Keep);
	CHECK(run.session.default_key == "X" && run.session.default_home == "Base");
	CHECK(run.session.zone_names.size() == 3 && run.session.zone_names[0] == "Bridge" &&
	      run.session.zone_names[1] == "Spawn Point" && run.session.zone_names[2] == "Spawn Point");
	const std::vector<DeathScreen::SpawnRow> &rows = run.screen.rows();
	CHECK(rows.size() == 3 && rows[0].label == "'X' Base" && rows[0].param == 0 && rows[1].label == "'A' Bridge" &&
	      rows[1].param == 1 && rows[2].param == -1);
	const std::vector<std::string> items = run.rt.get_widget_items(run.id("SPAWNPOINTS_LIST"));
	CHECK(items.size() == 3 && items[1] == "'A' Bridge");
	CHECK(run.shown("STATIC_LIST_TITLE"));
}

void test_the_pick_follows_its_param_across_the_refill() {
	Run run;
	run.session.revealed = true;
	run.session.compiled = { row("'D' Home", 0), row("'A' Bridge", 1), row("'B' Hill", 2) };
	CHECK(run.open());
	run.session.tick = 16;
	run.screen.frame(run.rt, true);
	const int list = run.id("SPAWNPOINTS_LIST");
	run.rt.select_row(list, 2, false); // the highlight alone: no pick
	CHECK(run.session.picks.empty());
	// A zone's change reshuffles the rows: the pick stays on its parameter.
	run.session.compiled = { row("'B' Hill", 2), row("'D' Home", 0), row("'A' Bridge", 1) };
	run.session.tick = 32;
	run.screen.frame(run.rt, true);
	CHECK(run.rt.selected_row(list) == 0 && run.screen.rows()[0].param == 2);
	CHECK(run.session.picks.empty());
}

void test_a_pick_under_the_initial_overlay_closes_and_a_repick_keeps() {
	Run run;
	run.session.revealed = true;
	run.session.compiled = { row("'D' Home", 0), row("'A' Bridge", 1), row("Ace", -1), row("", -1) };
	CHECK(run.open());
	run.session.tick = 16;
	run.screen.frame(run.rt, true);
	const int list = run.id("SPAWNPOINTS_LIST");
	// An occupant row and the blank separator never pick.
	run.rt.select_row(list, 2, true);
	run.rt.select_row(list, 3, true);
	CHECK(run.session.picks.empty() && run.session.requests.empty());
	// A death re-pick (a pick owed): sent, the screen kept until the host releases the hold.
	run.session.pending = true;
	run.rt.select_row(list, 1, true);
	CHECK(run.session.picks == std::vector<int>{ 1 } && run.session.requests.empty());
	// The same row again re-sends: the select callback is a command.
	run.rt.select_row(list, 1, true);
	CHECK(run.session.picks.size() == 2);
	// A dead player's pick keeps it too.
	run.session.pending = false;
	run.session.dead = true;
	run.rt.select_row(list, 0, true);
	CHECK(run.session.picks.back() == 0 && run.session.requests.empty());
	// Under the initial overlay the send closes the screen; a send that fails keeps it.
	run.session.dead = false;
	run.session.sends = false;
	run.rt.select_row(list, 1, true);
	CHECK(run.session.requests.empty());
	run.session.sends = true;
	run.rt.select_row(list, 1, true);
	CHECK(run.session.requests == std::vector<std::string>{ "close" });
	// A deploy key's pick, the screen open or not, takes the same rule.
	CHECK(run.screen.pick(0));
	run.session.pending = true;
	CHECK(!run.screen.pick(0));
}

void test_the_exit_confirmation_and_the_team_swap() {
	Run run;
	CHECK(run.open());
	CHECK(run.screen.pass_through());
	run.rt.activate(run.id("HIDDEN_BACK"));
	CHECK(!run.screen.pass_through());
	run.rt.activate(run.id("CONFIRM_NO"));
	CHECK(run.screen.pass_through());
	run.rt.activate(run.id("SWAP_TEAMS"));
	CHECK(run.session.team_changes == 1);
	CHECK(run.session.requests.empty());
	run.rt.activate(run.id("CONFIRM_YES"));
	CHECK((run.session.requests == std::vector<std::string>{ "close", "exit" }));
	// The close leaves the word as it stands; the next show raises it again.
	run.rt.activate(run.id("HIDDEN_BACK"));
	run.claimed = false;
	CHECK(!run.screen.pass_through());
	run.claimed = true;
	run.screen.open(run.rt, DeathScreen::kFile, run.rt.current_screen());
	CHECK(run.screen.pass_through());
}

void test_the_frame_verdicts() {
	Run run;
	CHECK(run.open());
	// The triggers fell on a frame a listen host's status page stood in for: nothing closes.
	run.session.is_held = false;
	CHECK(run.screen.frame(run.rt, false) == DeathScreen::Frame::Keep);
	CHECK(run.screen.frame(run.rt, true) == DeathScreen::Frame::Close);
	// A dead session tears the screen down whatever holds it.
	run.session.is_held = true;
	run.session.lost = true;
	CHECK(run.screen.frame(run.rt, true) == DeathScreen::Frame::Teardown);
}

void test_the_statics() {
	Run run;
	run.session.revealed = true;
	world::DeployScreenStatus &st = run.session.st;
	st.instructions.show_first = false;
	st.instructions.show_second = true;
	st.instructions.replace_first = false;
	st.instructions.first_text = "unused";
	st.instructions.second_text = "Choose a spawn point.";
	st.line.kind = world::DeployStatusLine::Kind::Wave;
	st.respawn_text = "A:  <cFF4040>7";
	st.statics.psp_respawn = true;
	st.statics_text.psp_respawn = "Hold  <cFF4040>3";
	st.statics.medic = true;
	st.statics_text.medic_timer = "Medic  <cFF4040>90";
	st.statics_text.call_medic = "Press M";
	run.session.compiled = { row("'D' Home", 0) };
	CHECK(run.open());
	run.rt.set_widget_text(run.id("STATIC_INSTRUCTIONS_MSG"), "Ace was killed.");
	run.session.tick = 16;
	run.screen.frame(run.rt, true);
	CHECK(!run.shown("STATIC_INSTRUCTIONS_MSG") && run.shown("STATIC_INSTRUCTIONS2_MSG"));
	// A secured spawn keeps the first widget's prior text.
	CHECK(run.rt.get_widget_text(run.id("STATIC_INSTRUCTIONS_MSG")) == "Ace was killed.");
	CHECK(run.rt.get_widget_text(run.id("STATIC_INSTRUCTIONS2_MSG")) == "Choose a spawn point.");
	CHECK(run.shown("STATIC_RESPAWN_MSG1") && run.rt.get_widget_text(run.id("STATIC_RESPAWN_MSG1")) == st.respawn_text);
	CHECK(run.shown("STATIC_PSPRESPAWN_MSG1"));
	CHECK(run.shown("STATIC_MEDIC_MSG1") && run.shown("STATIC_CALLMEDIC_MSG"));
	CHECK(run.rt.get_widget_text(run.id("STATIC_CALLMEDIC_MSG")) == "Press M");
	CHECK(run.screen.rows().size() == 1);
	// No line, no hold, no revive window: the three hide.
	st.line.kind = world::DeployStatusLine::Kind::None;
	st.statics.psp_respawn = false;
	st.statics.medic = false;
	run.session.tick = 32;
	run.screen.frame(run.rt, true);
	CHECK(!run.shown("STATIC_RESPAWN_MSG1") && !run.shown("STATIC_PSPRESPAWN_MSG1"));
	CHECK(!run.shown("STATIC_MEDIC_MSG1") && !run.shown("STATIC_CALLMEDIC_MSG"));
	// Permanent death: the round status pair takes the two statics, and the list is not refilled.
	st.instructions.permanent_death = true;
	st.instructions.show_round_status = true;
	st.instructions.round_text = "Round 2";
	st.instructions.remaining_players_text = "3 left";
	run.session.compiled = { row("'D' Home", 0), row("'A' Bridge", 1) };
	run.session.tick = 48;
	run.screen.frame(run.rt, true);
	CHECK(run.shown("STATIC_RESPAWN_MSG1") && run.rt.get_widget_text(run.id("STATIC_RESPAWN_MSG1")) == "Round 2");
	CHECK(run.shown("STATIC_PSPRESPAWN_MSG1") &&
	      run.rt.get_widget_text(run.id("STATIC_PSPRESPAWN_MSG1")) == "3 left");
	CHECK(run.screen.rows().size() == 1);
	// The teardown drops the row model.
	run.screen.reset();
	CHECK(run.screen.rows().empty());
}

} // namespace

int main() {
	test_the_show_hides_the_shroud_and_raises_the_word();
	test_the_reveal_and_the_refresh();
	test_the_pick_follows_its_param_across_the_refill();
	test_a_pick_under_the_initial_overlay_closes_and_a_repick_keeps();
	test_the_exit_confirmation_and_the_team_swap();
	test_the_frame_verdicts();
	test_the_statics();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("death_screen_test OK\n");
	return 0;
}
