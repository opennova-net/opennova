#include "frontend.h"

#include <runtime/inmatch/game_config_block.h>
#include <runtime/inmatch/mission_exit.h>
#include <runtime/profile/player_profiles.h>

namespace opennova::frontend {

using menu::MissionScreen;

const char *mode_name(Mode mode) {
	switch (mode) {
	case Mode::MainMenu: return "MainMenu";
	case Mode::PreMenu: return "PreMenu";
	case Mode::GameLoop: return "Game Loop";
	case Mode::PostMenu: return "Post Menu";
	case Mode::None: break;
	}
	return "";
}

bool boot(const BootLegs &legs) {
	if (legs.force_num_lock) legs.force_num_lock();
	return !(legs.read_game_config && legs.read_game_config());
}

Frontend::Frontend(FrontendLegs legs) : legs_(std::move(legs)) {}

profile::PlayerProfiles *Frontend::profiles() const {
	return legs_.profiles ? legs_.profiles() : nullptr;
}

inmatch::GameConfigBlock *Frontend::game_config() const {
	return legs_.game_config ? legs_.game_config() : nullptr;
}

void Frontend::push_mode(Mode mode) {
	// The push resets the key queue ahead of the switch [orig: UI_NavHistoryPush @0x52BB30].
	call(legs_.reset_key_queue);
	mode_ = mode;
}

bool Frontend::in_menu() const {
	return mode_ != Mode::GameLoop && mode_ != Mode::PreMenu;
}

bool Frontend::in_world() const {
	return !in_menu() && !screens_.is_open();
}

const char *Frontend::state_name() const {
	if (in_menu()) return "menu";
	const char *screen = menu::mission_screen_state_name(screens_.current());
	return screen != nullptr ? screen : "world";
}

// --- the MainMenu -----------------------------------------------------------------------------

bool Frontend::enter_main_menu() {
	push_mode(Mode::MainMenu);
	loading_ = false;
	screens_.clear();
	if (legs_.read_menu_config && !legs_.read_menu_config()) {
		if (legs_.exit_process) legs_.exit_process(0);
		return false;
	}
	call(legs_.reload_options);
	call(legs_.load_profile);
	// The first start's intro step without its videos (OpenNova plays none): every record's
	// +1412 cleared and the profile saved, so a first run writes player.sav and weapon.sav.
	if (intro_pending_) {
		intro_pending_ = false;
		if (profile::PlayerProfiles *store = profiles()) store->clear_intro_pending();
		call(legs_.save_profile);
	}
	if (!menu_scene_made_) {
		menu_scene_made_ = true;
		call(legs_.load_cookie_jar);
	}
	call(legs_.show_menu);
	// The mode's init is done: the loop resets its bank and re-reads its clock.
	menu_bank_.reset();
	menu_clock_.restart();
	menu_updates_ = 0;
	return true;
}

bool Frontend::reload_main_menu() {
	if (legs_.reload_menu_config && !legs_.reload_menu_config()) {
		if (legs_.exit_process) legs_.exit_process(0);
		return false;
	}
	// The reload destroyed the menu scene (its teardown saved the jar); the init makes it anew.
	call(legs_.load_cookie_jar);
	menu_scene_made_ = true;
	call(legs_.reload_options);
	call(legs_.load_profile);
	return true;
}

// --- mission starts ---------------------------------------------------------------------------

bool Frontend::start_single_player() {
	if (profile::PlayerProfiles *store = profiles()) store->record_mission_start(-1);
	call(legs_.save_game_config);
	// The attribute word after the save, which a later host of this process advertises and the
	// next save point writes (D-NET-1014; inmatch::GameConfigBlock carries the witness).
	if (inmatch::GameConfigBlock *config = game_config()) config->single_player_mission_start();
	call(legs_.save_profile);
	return start_mission(false);
}

void Frontend::begin_load(bool join, bool restart) {
	loading_ = true;
	// Every mission start's session apply copies the record's +1460 word, which the round end
	// writes back (profile::PlayerProfiles::begin_session carries the witness); the record's
	// controls follow onto the live set ahead of the load.
	if (profile::PlayerProfiles *store = profiles()) store->begin_session();
	call(legs_.apply_controls);
	call(legs_.begin_world_load);
	push_mode(join ? Mode::PreMenu : Mode::GameLoop);
	if (legs_.begin_load && !legs_.begin_load(restart, join) && legs_.load_failed)
		legs_.load_failed("mission load handoff could not start");
}

bool Frontend::start_mission(bool in_session, bool restart) {
	if (legs_.load_can_start && !legs_.load_can_start()) return false;
	call(legs_.stop_lan_browse);
	if (!in_session) {
		restart_entry_ = true;
		call(legs_.remember_restart_entry);
	} else {
		// A session start clamps the record's class bytes in place (D-PLAYERINFO-34), and every
		// multiplayer start saves game.cfg, the first and each map change's.
		if (profile::PlayerProfiles *store = profiles()) store->clamp_session_classes();
		call(legs_.save_game_config);
	}
	begin_load(false, restart);
	return true;
}

bool Frontend::start_join() {
	if (legs_.load_can_start && !legs_.load_can_start()) return false;
	if (legs_.open_join_screen && !legs_.open_join_screen()) return start_mission(true);
	// The join screen's mode stops the menu music on entry.
	call(legs_.stop_music);
	call(legs_.stop_lan_browse);
	if (profile::PlayerProfiles *store = profiles()) store->clamp_session_classes();
	call(legs_.save_game_config);
	begin_load(true, true);
	return true;
}

void Frontend::join_game_starting() {
	if (mode_ == Mode::PreMenu) push_mode(Mode::GameLoop);
}

MissionStarted Frontend::mission_loaded(bool joiner, bool joined_in_match) {
	launch_mission_.clear();
	if (joiner && !joined_in_match) return MissionStarted::AwaitAdmission;
	return joiner ? MissionStarted::Reveal : MissionStarted::ResetAndReveal;
}

bool Frontend::finish_load() {
	if (!loading_) return false;
	loading_ = false;
	return true;
}

LoadFailure Frontend::load_failed(bool join_screen_open) {
	loading_ = false;
	return join_screen_open ? LoadFailure::JoinScreen : LoadFailure::Abort;
}

std::string Frontend::take_launch_mission() {
	std::string out;
	out.swap(launch_mission_);
	return out;
}

// --- the mission's exits ----------------------------------------------------------------------

ExitDecision Frontend::route_exit(const MissionExit &exit) const {
	ExitDecision out;
	// Back in the menu with nothing loading: the teardown ran (the double-notification guard).
	if (in_menu() && !loading_) return out;
	out.reason = exit.has_session ? exit.stored_reason : 0;
	if (out.reason == inmatch::kMissionExitNone) out.reason = inmatch::kMissionExitQuit;
	const inmatch::MainFrameExit verdict = inmatch::main_frame_exit(
			out.reason, exit.has_session && exit.mp_session, !exit.has_session || !exit.joiner);
	if (verdict == inmatch::MainFrameExit::RestartRoundSP) {
		if (restart_entry_) {
			out.route = ExitRoute::RestartRound;
			return out;
		}
		out.reason = inmatch::kMissionExitQuit;
		out.route = ExitRoute::LeaveToMenu;
		return out;
	}
	if (exit.has_session && !exit.mp_session) {
		// A Mission box's exit out of a session starts the mission the box names; any other
		// single-player exit (the end screens' ESC and timeout) is the ordinary way out.
		out.route = out.reason == inmatch::kMissionExitMissionBox && exit.mission_box_named
				? ExitRoute::MissionBox
				: ExitRoute::LeaveToMenu;
		return out;
	}
	// The in-session round end's routes (inmatch/map_change.h): a joiner's round over is the
	// Game Loop's reload on its kept connection; an in-session host's round end (3, or 4 under
	// REPLAY with LASTGAME off) the Post Menu's map change.
	if (exit.has_session && !loading_) {
		if (verdict == inmatch::MainFrameExit::GameLoop) {
			out.route = ExitRoute::JoinerReload;
			return out;
		}
		if (verdict == inmatch::MainFrameExit::PostMenu && !exit.joiner &&
				(out.reason == inmatch::kMissionExitMapCycle ||
						out.reason == inmatch::kMissionExitRoundOver)) {
			out.route = ExitRoute::MapChange;
			return out;
		}
	}
	out.route = ExitRoute::Abort;
	return out;
}

void Frontend::record_round_end(bool in_session, bool round_over) {
	profile::PlayerProfiles *store = profiles();
	// No catalog row carries a campaign, so no completion byte is written.
	if (store != nullptr && store->record_round_end(in_session, round_over, -1, -1, false))
		call(legs_.save_profile);
}

void Frontend::teardown_mission() {
	screens_.close();
}

void Frontend::enter_post_menu() {
	push_mode(Mode::PostMenu);
	loading_ = false;
	screens_.close();
}

// --- the in-mission screens -------------------------------------------------------------------

bool Frontend::open_screen(MissionScreen screen) {
	return !in_menu() && screens_.open(screen);
}

ScreenClose Frontend::close_screen(MissionScreen screen) {
	// DEATH's and STAT's close under their word: the close's input frame takes the menu cursor's
	// offset from the re-centre point (world::pass_through_close_axis carries the witness).
	const bool pass_through = screens_.passes_input();
	if (!screens_.close(screen)) return ScreenClose::None;
	return pass_through ? ScreenClose::ClosedPassThrough : ScreenClose::Closed;
}

bool Frontend::pause() {
	return in_world() && screens_.open(MissionScreen::Ingame);
}

bool Frontend::resume() {
	return screens_.close(MissionScreen::Ingame) || screens_.close(MissionScreen::Weapon);
}

EscapeRoute Frontend::escape(bool join_screen_open, bool round_ended, bool tools_interacting) const {
	// The join screen's Cancel answers it.
	if (join_screen_open) return EscapeRoute::CancelJoinScreen;
	// A join's load: the game-start wait's ESC aborts the join, the final wait's leaves that
	// wait for the game loop [orig: NapiClient_WaitForGameStart @0x42CC10 - the ESC @0x42CC77,
	// return 3 @0x42CCC7]. The mission start's own load is one synchronous call nothing
	// interrupts (docs/interface/loading-screen-re.md D-LOADSCR-7).
	if (loading_) return EscapeRoute::CancelLoad;
	if (in_menu()) return EscapeRoute::None;
	// Round over: the special-key chain's round-over leg owns ESC (hud::hud_round_over_key).
	if (round_ended) return EscapeRoute::None;
	// Over live play and under a pass-through screen the key is the key queue's: the escape
	// row's case runs at its record in the update's drain.
	if (keyboard_pass_owns_keys(tools_interacting)) return EscapeRoute::None;
	if (in_world()) return EscapeRoute::EscapeAction;
	switch (screens_.current()) {
	case MissionScreen::Ingame:
	case MissionScreen::Weapon: return EscapeRoute::Resume;
	case MissionScreen::CommandMap:
	case MissionScreen::Vehicle: return EscapeRoute::CloseScreen;
	default: return EscapeRoute::None;
	}
}

UseItemRoute Frontend::use_item(bool armory, bool vehicle, bool vehicle_team_matches) const {
	if ((armory || vehicle) && screens_.is_open()) return UseItemRoute::CloseScreen;
	if (armory && in_world()) return UseItemRoute::OpenArmory;
	if (vehicle && in_world() && vehicle_team_matches) return UseItemRoute::OpenVehicle;
	return UseItemRoute::None;
}

UseItemRoute Frontend::weapon_screen_toggle() const {
	return screens_.is_open() ? UseItemRoute::CloseScreen : UseItemRoute::OpenArmory;
}

// --- the input frame's gates ------------------------------------------------------------------

bool Frontend::gameplay_input_active(bool round_ended, bool tools_interacting) const {
	return in_world() && !round_ended && !tools_interacting;
}

bool Frontend::keyboard_pass_owns_keys(bool tools_interacting) const {
	if (tools_interacting) return false;
	const MissionScreen screen = screens_.current();
	return in_world() ||
			(!in_menu() && (screen == MissionScreen::Death || screen == MissionScreen::Stat));
}

bool Frontend::screen_input_frame(bool tools_interacting) const {
	return !loading_ && !tools_interacting && screens_.passes_input();
}

bool Frontend::input_frame_drains_keys(bool tools_interacting) const {
	if (loading_ || tools_interacting) return false;
	return in_world() || screens_.passes_input();
}

bool Frontend::keyboard_captured(bool chat_capturing, bool tools_interacting) const {
	return chat_capturing && (in_world() || screen_input_frame(tools_interacting));
}

bool Frontend::hud_frame_runs() const {
	return in_world() || (screens_.is_open() && screens_.current() != MissionScreen::Ingame);
}

// --- the main loop's ends ---------------------------------------------------------------------

QuitRequest Frontend::request_quit() {
	if (quit_requested_) {
		if (!quit_drain_pending_) return QuitRequest::Ignored;
		quit_drain_pending_ = false;
		return QuitRequest::FinishDrain;
	}
	quit_requested_ = true;
	call(legs_.save_game_config);
	call(legs_.save_cookie_jar);
	return QuitRequest::Begin;
}

bool Frontend::end_quit_drain() {
	const bool pending = quit_drain_pending_;
	quit_drain_pending_ = false;
	return pending;
}

bool Frontend::begin_process_exit() {
	if (process_exit_pending_) return false;
	process_exit_pending_ = true;
	quit_requested_ = true;
	return true;
}

void Frontend::exit_to_desktop() {
	call(legs_.save_profile);
	call(legs_.save_game_config);
}

void Frontend::fail_boot() {
	call(legs_.save_game_config);
}

void Frontend::save_options() {
	call(legs_.save_profile);
	call(legs_.save_game_config);
}

// --- the menu mode's bank ---------------------------------------------------------------------

int Frontend::bank_menu_frame() {
	const double elapsed = menu_clock_.take_frame_elapsed_seconds();
	const inmatch::GameConfigBlock *config = game_config();
	world::FrameLock lock;
	lock.clock = &inmatch::main_loop_tick_count_clock();
	lock.stamp_ms = menu_clock_.frame_stamp_ms();
	const bool locked = config != nullptr && inmatch::main_loop_frame_lock(&config->cfg()) &&
			lock.stamp_ms != 0;
	menu_updates_ = menu_bank_.bank(elapsed, locked ? &lock : nullptr);
	menu_clock_.end_frame(locked && lock.sleeps > 0 ? lock.stamp_ms : 0);
	return menu_updates_;
}

} // namespace opennova::frontend
