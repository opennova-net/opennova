// The frontend (runtime/frontend/frontend.h) driven headless: every leg records itself, so the
// mode table's transitions, the save points' order and the routing are pinned as the engine runs
// them, with no device behind the legs and the stores the engine's own (profile::PlayerProfiles,
// inmatch::GameConfigBlock). A mission start's load runs through the load executor's headless legs
// (mission::run_mission_load over a session-only arm). Formerly the GDScript shell's (main_game.gd,
// game_runtime_root.gd) and its GUT lifecycle reads:
//   - the start: Num Lock forced ahead of game.cfg's read, mpreset ending the process there;
//   - the MainMenu's init: game.cfg, the options' copy, the profile, the first start's intro tail
//     and the menu scene's cookie jar, the menu shown; a read's mpreset exit; the Mods reload;
//   - single player's start: the campaign index, game.cfg's save ahead of the attribute word, the
//     profile's save, then the load and its reveal;
//   - a session's start and a join's: the class clamp and the PreMenu's save, the join's wait
//     under its screen, the game start's Game Loop, the failures' routes;
//   - the in-mission screens: the pause, the resume, the open refusal, the pass-through close,
//     the Esc routes, the use-item routes, the input frame's gates;
//   - the mission's exits: the SP restart, the Mission box, the joiner's reload, the host's map
//     change, the abort, the round end's profile step;
//   - the main loop's ends: the quit's saves and its drain, the mpreset exit's latch, a quit
//     past it saving nothing (formerly GUT's game_cfg_process_block_test
//     "the shell process exit saves nothing");
//   - the menu mode's bank.
#include <runtime/frontend/frontend.h>
#include <runtime/inmatch/game_config_block.h>
#include <runtime/inmatch/mission_exit.h>
#include <runtime/mission/mission_load.h>
#include <runtime/profile/player_profiles.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace opennova;
using namespace opennova::frontend;
using menu::MissionScreen;

namespace {

int failures = 0;

bool check(bool ok, const char *what) {
	if (!ok) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		++failures;
	}
	return ok;
}

using Trace = std::vector<std::string>;

void expect_trace(const Trace &got, const Trace &want, const char *what) {
	if (check(got == want, what)) return;
	std::fprintf(stderr, "  got:\n");
	for (const std::string &row : got) std::fprintf(stderr, "    %s\n", row.c_str());
	std::fprintf(stderr, "  want:\n");
	for (const std::string &row : want) std::fprintf(stderr, "    %s\n", row.c_str());
}

// The embedder: every leg records itself; the answers are the test's.
struct Shell {
	Trace trace;
	profile::PlayerProfiles profiles;
	inmatch::GameConfigBlock config;
	bool menu_config_ok = true;
	bool can_start = true;
	bool join_screen_opens = true;
	bool load_ok = true;
	int exit_code = -1;
	std::string failure;
	int loads_run = 0;

	// The mission start's load through the executor's headless legs: the session's legs alone,
	// as opennova-serve's Serve Only host runs it.
	bool run_headless_load() {
		mission::MissionLoadArm arm;
		arm.fresh_authority = true;
		mission::MissionLoadLegs legs;
		legs.session.create_session = [this]() {
			trace.push_back("load:create_session");
			return 0;
		};
		legs.session.start_runtime = [this]() {
			trace.push_back("load:start_runtime");
			return 0;
		};
		legs.session.role = []() { return mission::MissionLoadRole::Authority; };
		legs.session.play = [this]() { trace.push_back("load:play"); };
		const int error = mission::run_mission_load(arm, legs);
		if (error == 0) ++loads_run;
		return error == 0;
	}

	FrontendLegs legs() {
		FrontendLegs l;
		const auto leg = [this](const char *name) {
			return [this, name]() { trace.push_back(name); };
		};
		l.profiles = [this]() { return &profiles; };
		l.game_config = [this]() { return &config; };
		l.save_profile = leg("save_profile");
		l.save_game_config = [this]() {
			trace.push_back(config.cfg().mpattrib == inmatch::kSinglePlayerMpAttrib
							? "save_game_config(sp word)"
							: "save_game_config");
		};
		l.read_menu_config = [this]() {
			trace.push_back("read_menu_config");
			return menu_config_ok;
		};
		l.reload_menu_config = [this]() {
			trace.push_back("reload_menu_config");
			return menu_config_ok;
		};
		l.load_profile = leg("load_profile");
		l.load_cookie_jar = leg("load_cookie_jar");
		l.save_cookie_jar = leg("save_cookie_jar");
		l.apply_controls = leg("apply_controls");
		l.reload_options = leg("reload_options");
		l.show_menu = leg("show_menu");
		l.reset_key_queue = leg("reset_key_queue");
		l.load_can_start = [this]() { return can_start; };
		l.stop_lan_browse = leg("stop_lan_browse");
		l.stop_music = leg("stop_music");
		l.remember_restart_entry = leg("remember_restart_entry");
		l.open_join_screen = [this]() {
			trace.push_back("open_join_screen");
			return join_screen_opens;
		};
		l.begin_world_load = leg("begin_world_load");
		l.begin_load = [this](bool restart, bool join) {
			trace.push_back(std::string("begin_load") + (restart ? " restart" : "") +
					(join ? " join" : ""));
			return load_ok && run_headless_load();
		};
		l.load_failed = [this](const std::string &reason) {
			trace.push_back("load_failed");
			failure = reason;
		};
		l.exit_process = [this](int code) {
			trace.push_back("exit_process");
			exit_code = code;
		};
		return l;
	}
};

void the_start() {
	Trace trace;
	BootLegs legs;
	legs.force_num_lock = [&]() { trace.push_back("num_lock"); };
	legs.read_game_config = [&]() {
		trace.push_back("read_game_config");
		return false;
	};
	check(boot(legs), "a clean game.cfg starts the game");
	expect_trace(trace, {"num_lock", "read_game_config"}, "Num Lock is forced ahead of the read");
	legs.read_game_config = []() { return true; };
	check(!boot(legs), "an mpreset read ends the process at the start");
}

void the_main_menu() {
	Shell shell;
	Frontend fe(shell.legs());
	check(fe.mode() == Mode::None && fe.in_menu(), "no mode before the loop");
	shell.profiles.current().intro_pending = -1;
	check(fe.enter_main_menu(), "the first menu entry");
	expect_trace(shell.trace,
			{"reset_key_queue", "read_menu_config", "reload_options", "load_profile",
					"save_profile", "load_cookie_jar", "show_menu"},
			"the MainMenu's init: game.cfg, the options, the profile, the intro tail, the jar");
	check(shell.profiles.current().intro_pending == 0, "the intro tail clears +1412");
	check(fe.mode() == Mode::MainMenu && std::strcmp(fe.state_name(), "menu") == 0, "MainMenu");
	check(std::strcmp(mode_name(fe.mode()), "MainMenu") == 0, "the descriptor's name");
	shell.trace.clear();
	check(fe.enter_main_menu(), "a later menu entry");
	expect_trace(shell.trace,
			{"reset_key_queue", "read_menu_config", "reload_options", "load_profile", "show_menu"},
			"a later entry: no intro tail, the scene kept with its jar");
	shell.trace.clear();
	check(fe.reload_main_menu(), "the Mods reload");
	expect_trace(shell.trace,
			{"reload_menu_config", "load_cookie_jar", "reload_options", "load_profile"},
			"the reload: game.cfg saved and read, the menu made anew with its jar");
	shell.trace.clear();
	shell.menu_config_ok = false;
	check(!fe.enter_main_menu(), "an entry whose read sets mpreset");
	expect_trace(shell.trace, {"reset_key_queue", "read_menu_config", "exit_process"},
			"the mpreset read ends the process, nothing after it");
	check(shell.exit_code == 0, "with exit code 0");
	shell.trace.clear();
	check(!fe.reload_main_menu(), "a reload whose read sets mpreset");
	expect_trace(shell.trace, {"reload_menu_config", "exit_process"}, "the reload's exit");
}

void single_player_start() {
	Shell shell;
	Frontend fe(shell.legs());
	fe.enter_main_menu();
	shell.trace.clear();
	shell.profiles.current().word_1460 = 7;
	check(fe.start_single_player(), "the SP start");
	expect_trace(shell.trace,
			{"save_game_config", "save_profile", "stop_lan_browse", "remember_restart_entry",
					"apply_controls", "begin_world_load", "reset_key_queue", "begin_load",
					"load:create_session", "load:start_runtime", "load:play"},
			"SP: game.cfg ahead of its word, the profile, then the load through the executor");
	check(shell.profiles.current().last_campaign == -1, "the campaign index: none");
	check(shell.config.cfg().mpattrib == inmatch::kSinglePlayerMpAttrib,
			"the attribute word stands after the save");
	check(shell.loads_run == 1, "the headless load ran");
	check(fe.mode() == Mode::GameLoop && fe.loading() && fe.has_restart_entry(),
			"the Game Loop's load is up, the restart's entry kept");
	check(std::strcmp(fe.state_name(), "world") == 0, "the load is the world's");
	check(fe.escape(false, false, false) == EscapeRoute::CancelLoad, "a load's Esc is the load's");
	fe.set_launch_mission("mnml.bms");
	check(fe.mission_loaded(false, false) == MissionStarted::ResetAndReveal,
			"a single player's start resets the input, then reveals");
	check(fe.take_launch_mission().empty(), "a loaded launch mission is no failure");
	check(fe.finish_load() && !fe.loading() && !fe.finish_load(), "the reveal ends the load once");
	check(fe.in_world() && fe.gameplay_input_active(false, false), "live play");
	check(!fe.gameplay_input_active(true, false), "a finished round holds the gameplay input");
	check(!fe.gameplay_input_active(false, true), "the tools' interact mode keeps the input");
	// The profile's word, copied at the start and written back at the round end.
	shell.profiles.current().word_1460 = 9;
	shell.trace.clear();
	fe.record_round_end(false, false);
	check(shell.trace.empty(), "outside a session an unspawned round end saves nothing");
	fe.record_round_end(false, true);
	expect_trace(shell.trace, {"save_profile"}, "a round end saves the profile");
	check(shell.profiles.current().word_1460 == 7, "the start's +1460 goes back");
}

void session_and_join_starts() {
	Shell shell;
	Frontend fe(shell.legs());
	fe.enter_main_menu();
	shell.trace.clear();
	shell.profiles.current_weapons().blue.player_class = 2;
	check(fe.start_mission(true), "a host's start");
	expect_trace(shell.trace,
			{"stop_lan_browse", "save_game_config", "apply_controls", "begin_world_load",
					"reset_key_queue", "begin_load", "load:create_session", "load:start_runtime",
					"load:play"},
			"a session's start: the class clamp and game.cfg's save, no restart entry");
	check(shell.profiles.current_weapons().blue.player_class == 5, "the class byte clamps to 5");
	check(!fe.has_restart_entry() && fe.mode() == Mode::GameLoop, "no SP restart entry");
	shell.can_start = false;
	shell.trace.clear();
	check(!fe.start_mission(false), "the coordinator refuses a second start");
	check(shell.trace.empty(), "a refused start changes nothing");

	Shell joiner;
	Frontend fj(joiner.legs());
	fj.enter_main_menu();
	joiner.trace.clear();
	check(fj.start_join(), "a join's start");
	expect_trace(joiner.trace,
			{"open_join_screen", "stop_music", "stop_lan_browse", "save_game_config",
					"apply_controls", "begin_world_load", "reset_key_queue",
					"begin_load restart join", "load:create_session", "load:start_runtime",
					"load:play"},
			"a join: its screen up, the music stopped, the load under the screen");
	check(fj.mode() == Mode::PreMenu && fj.loading() && !fj.in_menu(), "the PreMenu's wait");
	check(std::strcmp(fj.state_name(), "world") == 0, "the wait is the mission's load");
	check(fj.escape(true, false, false) == EscapeRoute::CancelJoinScreen,
			"the join screen's Cancel answers Esc");
	check(fj.load_failed(true) == LoadFailure::JoinScreen && !fj.loading(),
			"a failed join keeps its screen");
	check(fj.mode() == Mode::PreMenu, "the PreMenu stands under the failure");
	fj.join_game_starting();
	check(fj.mode() == Mode::GameLoop, "the host's game start pushes the Game Loop");
	check(fj.mission_loaded(true, false) == MissionStarted::AwaitAdmission,
			"a joiner's world waits for its admission");
	check(fj.mission_loaded(true, true) == MissionStarted::Reveal,
			"a joiner past its wait reveals with no reset");

	Shell fallback;
	fallback.join_screen_opens = false;
	Frontend ff(fallback.legs());
	ff.enter_main_menu();
	fallback.trace.clear();
	check(ff.start_join(), "a join whose screen cannot open");
	check(fallback.trace.size() > 1 && fallback.trace[1] == "stop_lan_browse" &&
					ff.mode() == Mode::GameLoop,
			"the common session start instead");

	Shell failing;
	failing.load_ok = false;
	Frontend fl(failing.legs());
	fl.enter_main_menu();
	fl.set_launch_mission("launch.bms");
	check(fl.start_mission(false), "a start whose handoff fails");
	check(failing.failure == "mission load handoff could not start", "the failure leg ran");
	check(fl.load_failed(false) == LoadFailure::Abort, "an SP failure aborts");
	check(fl.take_launch_mission() == "launch.bms" && fl.take_launch_mission().empty(),
			"the launch's mission is named once");
}

void the_screens() {
	Shell shell;
	Frontend fe(shell.legs());
	check(!fe.pause() && !fe.open_screen(MissionScreen::Death), "no screen opens in the menu");
	fe.enter_main_menu();
	check(fe.escape(false, false, false) == EscapeRoute::None, "the menu's Esc is the menu's");
	fe.start_single_player();
	fe.finish_load();
	check(fe.escape(false, false, false) == EscapeRoute::None,
			"over live play Esc is the keyboard pass's");
	check(fe.escape(false, false, true) == EscapeRoute::EscapeAction,
			"the tools' interact mode: the HUD's chain, else the pause");
	check(fe.escape(false, true, true) == EscapeRoute::None, "a finished round owns Esc");
	check(fe.pause() && fe.screen() == MissionScreen::Ingame, "the pause opens INGAME");
	check(std::strcmp(fe.state_name(), "paused") == 0, "paused");
	check(!fe.pause() && !fe.open_screen(MissionScreen::Death),
			"no second screen over INGAME (UI_OpenMenuScreen's refusal)");
	check(fe.screens().holds_input() && !fe.input_frame_drains_keys(false),
			"INGAME holds the input frame");
	check(fe.escape(false, false, false) == EscapeRoute::Resume, "INGAME's Esc resumes");
	check(!fe.hud_frame_runs(), "no HUD frame under INGAME");
	check(fe.resume() && fe.in_world() && !fe.resume(), "the resume, once");
	check(fe.use_item(true, false, true) == UseItemRoute::OpenArmory, "the USE armory arm");
	check(fe.use_item(false, true, false) == UseItemRoute::None, "the vehicle's team test");
	check(fe.use_item(false, true, true) == UseItemRoute::OpenVehicle, "the USE vehicle arm");
	check(fe.weapon_screen_toggle() == UseItemRoute::OpenArmory, "the toggle opens WEAPON");
	check(fe.open_screen(MissionScreen::Weapon) && fe.hud_frame_runs(),
			"WEAPON over live play, the HUD's frame running");
	check(fe.escape(false, false, false) == EscapeRoute::Resume, "WEAPON's Esc resumes");
	check(fe.use_item(true, false, true) == UseItemRoute::CloseScreen,
			"a USE arm under a screen closes it");
	check(fe.close_screen(MissionScreen::Weapon) == ScreenClose::Closed, "WEAPON's close");
	check(fe.open_screen(MissionScreen::CommandMap) &&
					fe.escape(false, false, true) == EscapeRoute::CloseScreen,
			"CMAP's Esc closes it");
	fe.close_screen(MissionScreen::CommandMap);
	// DEATH under its word passes the input through; its close then takes the cursor's offset.
	check(fe.open_screen(MissionScreen::Death), "DEATH over the mission");
	fe.screens().set_pass_through(true);
	check(std::strcmp(fe.state_name(), "deploy") == 0, "deploy");
	check(fe.keyboard_pass_owns_keys(false) && fe.input_frame_drains_keys(false) &&
					fe.screen_input_frame(false),
			"DEATH under its word: the keyboard pass runs under it");
	check(fe.escape(false, false, false) == EscapeRoute::None, "DEATH's Esc is its own key leg");
	check(fe.keyboard_captured(true, false) && !fe.keyboard_captured(false, false),
			"the chat line's capture under a passing screen");
	check(fe.weapon_screen_toggle() == UseItemRoute::CloseScreen, "the toggle closes DEATH");
	fe.screens().set_pass_through(false);
	check(!fe.input_frame_drains_keys(false) && fe.screens().holds_input(),
			"the exit confirmation's lowered word holds the input");
	fe.screens().set_pass_through(true);
	check(fe.close_screen(MissionScreen::Death) == ScreenClose::ClosedPassThrough,
			"DEATH's close under its word");
	check(fe.open_screen(MissionScreen::Death), "DEATH again");
	fe.screens().set_pass_through(false);
	check(fe.close_screen(MissionScreen::Death) == ScreenClose::Closed,
			"DEATH's close under the lowered word: no cursor offset");
	check(fe.close_screen(MissionScreen::Death) == ScreenClose::None, "a stale close edge");
	fe.open_screen(MissionScreen::Stat);
	fe.teardown_mission();
	check(!fe.screens().is_open(), "the mission's teardown closes the screen up");
}

void the_exits() {
	Shell shell;
	Frontend fe(shell.legs());
	fe.enter_main_menu();
	MissionExit exit;
	exit.has_session = true;
	check(fe.route_exit(exit).route == ExitRoute::None, "back in the menu: the teardown ran");
	fe.start_single_player();
	fe.finish_load();
	// The SP restart: reason 4 out of a session.
	exit.stored_reason = inmatch::kMissionExitRoundOver;
	ExitDecision d = fe.route_exit(exit);
	check(d.route == ExitRoute::RestartRound && d.reason == inmatch::kMissionExitRoundOver,
			"the SP restart runs Game_RestartRoundSP");
	exit.stored_reason = inmatch::kMissionExitMissionBox;
	exit.mission_box_named = true;
	check(fe.route_exit(exit).route == ExitRoute::MissionBox, "a named Mission box starts its mission");
	exit.mission_box_named = false;
	check(fe.route_exit(exit).route == ExitRoute::LeaveToMenu, "an unnamed box leaves for the menu");
	exit.stored_reason = 0;
	d = fe.route_exit(exit);
	check(d.route == ExitRoute::LeaveToMenu && d.reason == inmatch::kMissionExitQuit,
			"an unmapped SP exit is the player's own quit");
	// A session's.
	exit.mp_session = true;
	exit.joiner = true;
	exit.stored_reason = inmatch::kMissionExitRoundOver;
	check(fe.route_exit(exit).route == ExitRoute::JoinerReload, "a joiner's round over reloads");
	exit.joiner = false;
	exit.stored_reason = inmatch::kMissionExitMapCycle;
	check(fe.route_exit(exit).route == ExitRoute::MapChange, "a host's round end changes the map");
	exit.stored_reason = inmatch::kMissionExitQuit;
	check(fe.route_exit(exit).route == ExitRoute::Abort, "a host's quit aborts to the menu");
	MissionExit gone;
	d = fe.route_exit(gone);
	check(d.route == ExitRoute::Abort && d.reason == inmatch::kMissionExitQuit,
			"no session left: the abort with the quit's reason");
	// A restart with no SP entry leaves for the menu.
	Shell host;
	Frontend fh(host.legs());
	fh.enter_main_menu();
	fh.start_mission(true);
	MissionExit sp;
	sp.has_session = true;
	sp.stored_reason = inmatch::kMissionExitRoundOver;
	d = fh.route_exit(sp);
	check(d.route == ExitRoute::LeaveToMenu && d.reason == inmatch::kMissionExitQuit,
			"a restart with no entry leaves as a quit");
	fh.enter_post_menu();
	check(fh.mode() == Mode::PostMenu && fh.in_menu() && !fh.loading(), "the Post Menu");
	check(fh.route_exit(sp).route == ExitRoute::None, "the Post Menu's route sees the menu");
}

void the_loop_ends() {
	Shell shell;
	Frontend fe(shell.legs());
	check(fe.request_quit() == QuitRequest::Begin, "the first quit");
	expect_trace(shell.trace, {"save_game_config", "save_cookie_jar"},
			"the main loop's save, then the menu's jar");
	check(fe.request_quit() == QuitRequest::Ignored, "a repeat with nothing to hurry");
	fe.begin_quit_drain();
	check(fe.request_quit() == QuitRequest::FinishDrain && !fe.end_quit_drain(),
			"a repeat ends the drain");
	shell.trace.clear();
	fe.exit_to_desktop();
	expect_trace(shell.trace, {"save_profile", "save_game_config"}, "EXIT: the profile, game.cfg");
	Shell other;
	Frontend fo(other.legs());
	check(fo.begin_process_exit() && !fo.begin_process_exit(), "the mpreset exit latches once");
	check(fo.process_exit_pending() && fo.quit_requested() &&
					fo.request_quit() == QuitRequest::Ignored,
			"a quit past the exit does nothing");
	check(other.trace.empty(), "the exit and the quit past it save nothing");
}

void the_menu_bank() {
	Shell shell;
	shell.config.cfg().lock_framerate = 0;
	Frontend fe(shell.legs());
	fe.enter_main_menu();
	check(fe.bank_menu_frame() == 0 && fe.menu_updates() == 0,
			"the mode's first frame only stamps the clock");
	check(fe.bank_menu_frame() >= 0, "later frames drain the bank");
}

} // namespace

int main() {
	the_start();
	the_main_menu();
	single_player_start();
	session_and_join_starts();
	the_screens();
	the_exits();
	the_loop_ends();
	the_menu_bank();
	if (failures != 0) {
		std::fprintf(stderr, "frontend: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("frontend: OK\n");
	return 0;
}
