// The in-mission screen stack (runtime/menu/screen_stack.h): one screen up at a time, an open
// while one is up refused, the close request the screens' commands latch and the next screen
// pass runs, the close reported so its embedder trims the history and resets the input, and the
// pass-through word's input law under DEATH and STAT (menu/mission_screen.h). Formerly the shell's
// state-to-screen map (main_game.gd _mission_screen) and its GUT reads.
#include <runtime/menu/screen_stack.h>

#include <cstdio>
#include <cstring>
#include <initializer_list>

using namespace opennova::menu;

namespace {

int failures = 0;

void check(bool ok, const char *what) {
	if (!ok) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		++failures;
	}
}

void one_screen_at_a_time() {
	ScreenStack stack;
	check(!stack.is_open() && stack.current() == MissionScreen::None, "nothing up at the start");
	check(!stack.open(MissionScreen::None), "None is no screen");
	check(stack.open(MissionScreen::Death), "DEATH comes up");
	check(!stack.open(MissionScreen::Ingame), "the open word refuses INGAME over DEATH");
	check(stack.current() == MissionScreen::Death, "DEATH stays the screen up");
	check(!stack.close(MissionScreen::Ingame), "a close edge of a screen not up does nothing");
	check(stack.close(MissionScreen::Death), "DEATH's own close");
	check(!stack.close(), "a close with none up reports nothing to run");
	check(stack.open(MissionScreen::Ingame), "INGAME comes up once DEATH is gone");
}

void the_close_request() {
	ScreenStack stack;
	stack.open(MissionScreen::Weapon);
	stack.request_close(true);
	check(!stack.close_requested(), "the options dialog's modal hold refuses the request");
	stack.request_close();
	check(stack.close_requested() && stack.is_open(), "the request waits for the screen pass");
	check(stack.run_requested_close(), "the screen pass closes it");
	check(!stack.is_open() && !stack.close_requested(), "the close clears the word and the request");
	check(!stack.run_requested_close(), "no request, no close");
	stack.request_close();
	check(!stack.run_requested_close() && stack.close_requested(),
			"a request with no screen up closes nothing and stands");
	check(stack.open(MissionScreen::CommandMap) && !stack.close_requested(),
			"the open clears a standing request");
}

void the_pass_through_word() {
	ScreenStack stack;
	stack.set_pass_through(true);
	check(!stack.holds_input() && !stack.passes_input(), "no screen: neither holds nor passes");
	for (MissionScreen s : {MissionScreen::Ingame, MissionScreen::Weapon, MissionScreen::CommandMap,
				 MissionScreen::Vehicle}) {
		stack.open(s);
		check(stack.holds_input() && !stack.passes_input(),
				"INGAME, WEAPON, CMAP and VEHICLE hold the input frame whatever the word");
		stack.close();
	}
	for (MissionScreen s : {MissionScreen::Death, MissionScreen::Stat}) {
		stack.open(s);
		stack.set_pass_through(true);
		check(stack.passes_input() && !stack.holds_input(), "DEATH and STAT pass under the word");
		stack.set_pass_through(false);
		check(stack.holds_input() && !stack.passes_input(), "the lowered word holds the input");
		stack.set_pass_through(true);
		stack.close();
		check(stack.pass_through(), "a close leaves the word as it stands");
	}
}

void the_state_names() {
	check(mission_screen_state_name(MissionScreen::None) == nullptr, "None has no name");
	check(std::strcmp(mission_screen_state_name(MissionScreen::Ingame), "paused") == 0, "paused");
	check(std::strcmp(mission_screen_state_name(MissionScreen::Weapon), "armory") == 0, "armory");
	check(std::strcmp(mission_screen_state_name(MissionScreen::Death), "deploy") == 0, "deploy");
	check(std::strcmp(mission_screen_state_name(MissionScreen::Stat), "end_round") == 0,
			"end_round");
	check(std::strcmp(mission_screen_state_name(MissionScreen::CommandMap), "command_map") == 0,
			"command_map");
	check(std::strcmp(mission_screen_state_name(MissionScreen::Vehicle), "vehicle") == 0,
			"vehicle");
}

} // namespace

int main() {
	one_screen_at_a_time();
	the_close_request();
	the_pass_through_word();
	the_state_names();
	if (failures != 0) {
		std::fprintf(stderr, "screen_stack: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("screen_stack: OK\n");
	return 0;
}
