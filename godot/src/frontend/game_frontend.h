#pragma once

// The frontend binding: the one engine frontend::Frontend (the mode table, the in-mission screen
// stack, the save points and the routing; runtime/frontend/frontend.h carries every rule and its
// witness) with its legs filled from the GDScript shell's Callables. Nothing here decides: this
// class marshals.

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/string.hpp>

#include <runtime/frontend/frontend.h>

namespace godot {

class GameFrontend : public RefCounted {
	GDCLASS(GameFrontend, RefCounted)

public:
	enum Leg {
		LEG_PROFILES,         // () -> PlayerProfiles
		LEG_GAME_CONFIG,      // () -> GameConfigBlock
		LEG_SAVE_PROFILE,     // ()
		LEG_SAVE_GAME_CONFIG, // ()
		LEG_READ_MENU_CONFIG, // () -> bool (false: mpreset)
		LEG_RELOAD_MENU_CONFIG, // () -> bool (false: mpreset)
		LEG_LOAD_PROFILE,     // ()
		LEG_LOAD_COOKIE_JAR,  // ()
		LEG_SAVE_COOKIE_JAR,  // ()
		LEG_SAVE_HIGH_SCORES, // ()
		LEG_APPLY_CONTROLS,   // ()
		LEG_RELOAD_OPTIONS,   // ()
		LEG_SHOW_MENU,        // ()
		LEG_RESET_KEY_QUEUE,  // ()
		LEG_LOAD_CAN_START,   // () -> bool
		LEG_STOP_LAN_BROWSE,  // ()
		LEG_STOP_MUSIC,       // ()
		LEG_REMEMBER_RESTART_ENTRY, // ()
		LEG_OPEN_JOIN_SCREEN, // () -> bool
		LEG_BEGIN_WORLD_LOAD, // ()
		LEG_BEGIN_LOAD,       // (restart: bool, join: bool) -> bool
		LEG_LOAD_FAILED,      // (reason: String)
		LEG_EXIT_PROCESS,     // (code: int)
		LEG_COUNT,
	};

	enum Mode {
		MODE_NONE = static_cast<int>(opennova::frontend::Mode::None),
		MODE_MAIN_MENU = static_cast<int>(opennova::frontend::Mode::MainMenu),
		MODE_PRE_MENU = static_cast<int>(opennova::frontend::Mode::PreMenu),
		MODE_GAME_LOOP = static_cast<int>(opennova::frontend::Mode::GameLoop),
		MODE_POST_MENU = static_cast<int>(opennova::frontend::Mode::PostMenu),
	};

	enum EscapeRoute {
		ESCAPE_NONE = static_cast<int>(opennova::frontend::EscapeRoute::None),
		ESCAPE_CANCEL_JOIN_SCREEN = static_cast<int>(opennova::frontend::EscapeRoute::CancelJoinScreen),
		ESCAPE_CANCEL_LOAD = static_cast<int>(opennova::frontend::EscapeRoute::CancelLoad),
		ESCAPE_ACTION = static_cast<int>(opennova::frontend::EscapeRoute::EscapeAction),
		ESCAPE_RESUME = static_cast<int>(opennova::frontend::EscapeRoute::Resume),
		ESCAPE_CLOSE_SCREEN = static_cast<int>(opennova::frontend::EscapeRoute::CloseScreen),
	};

	enum UseItemRoute {
		USE_NONE = static_cast<int>(opennova::frontend::UseItemRoute::None),
		USE_CLOSE_SCREEN = static_cast<int>(opennova::frontend::UseItemRoute::CloseScreen),
		USE_OPEN_ARMORY = static_cast<int>(opennova::frontend::UseItemRoute::OpenArmory),
		USE_OPEN_VEHICLE = static_cast<int>(opennova::frontend::UseItemRoute::OpenVehicle),
	};

	enum ExitRoute {
		EXIT_NONE = static_cast<int>(opennova::frontend::ExitRoute::None),
		EXIT_RESTART_ROUND = static_cast<int>(opennova::frontend::ExitRoute::RestartRound),
		EXIT_MISSION_BOX = static_cast<int>(opennova::frontend::ExitRoute::MissionBox),
		EXIT_LEAVE_TO_MENU = static_cast<int>(opennova::frontend::ExitRoute::LeaveToMenu),
		EXIT_JOINER_RELOAD = static_cast<int>(opennova::frontend::ExitRoute::JoinerReload),
		EXIT_MAP_CHANGE = static_cast<int>(opennova::frontend::ExitRoute::MapChange),
		EXIT_ABORT = static_cast<int>(opennova::frontend::ExitRoute::Abort),
	};

	enum MissionStarted {
		STARTED_AWAIT_ADMISSION = static_cast<int>(opennova::frontend::MissionStarted::AwaitAdmission),
		STARTED_REVEAL = static_cast<int>(opennova::frontend::MissionStarted::Reveal),
		STARTED_RESET_AND_REVEAL = static_cast<int>(opennova::frontend::MissionStarted::ResetAndReveal),
	};

	enum ScreenClose {
		CLOSE_NONE = static_cast<int>(opennova::frontend::ScreenClose::None),
		CLOSE_CLOSED = static_cast<int>(opennova::frontend::ScreenClose::Closed),
		CLOSE_PASS_THROUGH = static_cast<int>(opennova::frontend::ScreenClose::ClosedPassThrough),
	};

	GameFrontend();

	opennova::frontend::Frontend &native() { return frontend_; }
	const opennova::frontend::Frontend &native() const { return frontend_; }

	void set_leg(Leg p_leg, const Callable &p_callable);

	// The process's start (frontend::boot): false when game.cfg's mpreset ends it.
	static bool boot(const Callable &p_force_num_lock, const Callable &p_read_game_config);

	Mode get_mode() const { return static_cast<Mode>(frontend_.mode()); }
	String state_name() const { return String(frontend_.state_name()); }
	bool is_loading() const { return frontend_.loading(); }
	bool in_menu() const { return frontend_.in_menu(); }
	bool in_world() const { return frontend_.in_world(); }
	// The screen up, as MenuDriver.MissionScreen.
	int get_screen() const { return static_cast<int>(frontend_.screen()); }
	bool is_screen_open() const { return frontend_.screens().is_open(); }
	bool screen_holds_input() const { return frontend_.screens().holds_input(); }
	void set_pass_through(bool p_word) { frontend_.screens().set_pass_through(p_word); }

	bool enter_main_menu() { return frontend_.enter_main_menu(); }
	bool reload_main_menu() { return frontend_.reload_main_menu(); }
	bool start_single_player(int p_campaign, int p_campaign_mission) {
		return frontend_.start_single_player(p_campaign, p_campaign_mission);
	}
	bool start_mission(bool p_in_session, bool p_restart) {
		return frontend_.start_mission(p_in_session, p_restart);
	}
	bool start_join() { return frontend_.start_join(); }
	void join_game_starting() { frontend_.join_game_starting(); }
	void set_launch_mission(const String &p_name);
	String take_launch_mission();
	MissionStarted mission_loaded(bool p_joiner, bool p_joined_in_match) {
		return static_cast<MissionStarted>(frontend_.mission_loaded(p_joiner, p_joined_in_match));
	}
	bool finish_load() { return frontend_.finish_load(); }
	// True when the failed join keeps its join screen (LoadFailure::JoinScreen).
	bool load_failed(bool p_join_screen_open) {
		return frontend_.load_failed(p_join_screen_open) ==
				opennova::frontend::LoadFailure::JoinScreen;
	}
	bool has_restart_entry() const { return frontend_.has_restart_entry(); }

	// The route of a session's exit; its reason is get_exit_reason's.
	ExitRoute route_exit(bool p_has_session, int p_stored_reason, bool p_mp_session, bool p_joiner,
			bool p_mission_box_named);
	int get_exit_reason() const { return exit_reason_; }
	void record_round_end(bool p_in_session, bool p_round_over, int p_winner_team) {
		frontend_.record_round_end(p_in_session, p_round_over, p_winner_team);
	}
	void teardown_mission() { frontend_.teardown_mission(); }
	void enter_post_menu() { frontend_.enter_post_menu(); }

	bool open_screen(int p_screen);
	ScreenClose close_screen(int p_screen);
	bool pause() { return frontend_.pause(); }
	bool resume() { return frontend_.resume(); }
	EscapeRoute escape(bool p_join_screen_open, bool p_round_ended, bool p_tools_interacting) const {
		return static_cast<EscapeRoute>(
				frontend_.escape(p_join_screen_open, p_round_ended, p_tools_interacting));
	}
	UseItemRoute use_item(bool p_armory, bool p_vehicle, bool p_vehicle_team_matches) const {
		return static_cast<UseItemRoute>(
				frontend_.use_item(p_armory, p_vehicle, p_vehicle_team_matches));
	}
	UseItemRoute weapon_screen_toggle() const {
		return static_cast<UseItemRoute>(frontend_.weapon_screen_toggle());
	}

	bool gameplay_input_active(bool p_round_ended, bool p_tools_interacting) const {
		return frontend_.gameplay_input_active(p_round_ended, p_tools_interacting);
	}
	bool keyboard_pass_owns_keys(bool p_tools_interacting) const {
		return frontend_.keyboard_pass_owns_keys(p_tools_interacting);
	}
	bool screen_input_frame(bool p_tools_interacting) const {
		return frontend_.screen_input_frame(p_tools_interacting);
	}
	bool input_frame_drains_keys(bool p_tools_interacting) const {
		return frontend_.input_frame_drains_keys(p_tools_interacting);
	}
	bool keyboard_captured(bool p_chat_capturing, bool p_tools_interacting) const {
		return frontend_.keyboard_captured(p_chat_capturing, p_tools_interacting);
	}
	bool hud_frame_runs() const { return frontend_.hud_frame_runs(); }

	bool request_quit() { return frontend_.request_quit(); }
	bool begin_process_exit() { return frontend_.begin_process_exit(); }
	bool is_process_exit_pending() const { return frontend_.process_exit_pending(); }
	void exit_to_desktop() { frontend_.exit_to_desktop(); }
	void fail_boot() { frontend_.fail_boot(); }
	void save_options() { frontend_.save_options(); }

	// One menu frame on the main loop's bank: the menu updates it drained, which the process's
	// menu-update readers (the PLAYER_INFO portrait) take this frame (last_menu_updates).
	int bank_menu_frame();
	static int last_menu_updates() { return s_last_menu_updates; }

protected:
	static void _bind_methods();

private:
	Variant call_leg(Leg p_leg) const;
	void install_legs();

	opennova::frontend::Frontend frontend_;
	Callable legs_[LEG_COUNT];
	int exit_reason_ = 0;
	static int s_last_menu_updates;
};

} // namespace godot

VARIANT_ENUM_CAST(godot::GameFrontend::Leg);
VARIANT_ENUM_CAST(godot::GameFrontend::Mode);
VARIANT_ENUM_CAST(godot::GameFrontend::EscapeRoute);
VARIANT_ENUM_CAST(godot::GameFrontend::UseItemRoute);
VARIANT_ENUM_CAST(godot::GameFrontend::ExitRoute);
VARIANT_ENUM_CAST(godot::GameFrontend::MissionStarted);
VARIANT_ENUM_CAST(godot::GameFrontend::ScreenClose);
