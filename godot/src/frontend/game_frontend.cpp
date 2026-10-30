#include "frontend/game_frontend.h"

#include "player/game_config_block.h"
#include "player/player_profiles.h"
#include "util/string_convert.h"

using namespace godot;

int GameFrontend::s_last_menu_updates = 0;

GameFrontend::GameFrontend() {
	install_legs();
}

Variant GameFrontend::call_leg(Leg p_leg) const {
	const Callable &leg = legs_[p_leg];
	return leg.is_valid() ? leg.call() : Variant();
}

void GameFrontend::set_leg(Leg p_leg, const Callable &p_callable) {
	if (p_leg < 0 || p_leg >= LEG_COUNT) return;
	legs_[p_leg] = p_callable;
}

// Every engine leg calls its Callable when one is set; an unset leg is work the shell has none
// of (a bool leg's default: go on).
void GameFrontend::install_legs() {
	opennova::frontend::FrontendLegs &l = frontend_.legs();
	const auto void_leg = [this](Leg p_leg) {
		return [this, p_leg]() { call_leg(p_leg); };
	};
	const auto bool_leg = [this](Leg p_leg) {
		return [this, p_leg]() {
			const Variant v = call_leg(p_leg);
			return v.get_type() == Variant::NIL || bool(v);
		};
	};
	l.profiles = [this]() -> opennova::profile::PlayerProfiles * {
		// The store outlives the call: the shell's process-lifetime owner holds it.
		const Variant v = call_leg(LEG_PROFILES);
		PlayerProfiles *p = Object::cast_to<PlayerProfiles>(static_cast<Object *>(v));
		return p != nullptr ? &p->native() : nullptr;
	};
	l.game_config = [this]() -> opennova::inmatch::GameConfigBlock * {
		const Variant v = call_leg(LEG_GAME_CONFIG);
		GameConfigBlock *b = Object::cast_to<GameConfigBlock>(static_cast<Object *>(v));
		return b != nullptr ? &b->native() : nullptr;
	};
	l.save_profile = void_leg(LEG_SAVE_PROFILE);
	l.save_game_config = void_leg(LEG_SAVE_GAME_CONFIG);
	l.read_menu_config = bool_leg(LEG_READ_MENU_CONFIG);
	l.reload_menu_config = bool_leg(LEG_RELOAD_MENU_CONFIG);
	l.load_profile = void_leg(LEG_LOAD_PROFILE);
	l.load_cookie_jar = void_leg(LEG_LOAD_COOKIE_JAR);
	l.save_cookie_jar = void_leg(LEG_SAVE_COOKIE_JAR);
	l.apply_controls = void_leg(LEG_APPLY_CONTROLS);
	l.reload_options = void_leg(LEG_RELOAD_OPTIONS);
	l.show_menu = void_leg(LEG_SHOW_MENU);
	l.reset_key_queue = void_leg(LEG_RESET_KEY_QUEUE);
	l.load_can_start = bool_leg(LEG_LOAD_CAN_START);
	l.stop_lan_browse = void_leg(LEG_STOP_LAN_BROWSE);
	l.stop_music = void_leg(LEG_STOP_MUSIC);
	l.remember_restart_entry = void_leg(LEG_REMEMBER_RESTART_ENTRY);
	l.open_join_screen = bool_leg(LEG_OPEN_JOIN_SCREEN);
	l.begin_world_load = void_leg(LEG_BEGIN_WORLD_LOAD);
	l.begin_load = [this](bool p_restart, bool p_join) {
		const Callable &leg = legs_[LEG_BEGIN_LOAD];
		return !leg.is_valid() || bool(leg.call(p_restart, p_join));
	};
	l.load_failed = [this](const std::string &p_reason) {
		const Callable &leg = legs_[LEG_LOAD_FAILED];
		if (leg.is_valid()) leg.call(opennova::to_gd(p_reason));
	};
	l.exit_process = [this](int p_code) {
		const Callable &leg = legs_[LEG_EXIT_PROCESS];
		if (leg.is_valid()) leg.call(p_code);
	};
}

bool GameFrontend::boot(const Callable &p_force_num_lock, const Callable &p_read_game_config) {
	opennova::frontend::BootLegs legs;
	legs.force_num_lock = [&p_force_num_lock]() {
		if (p_force_num_lock.is_valid()) p_force_num_lock.call();
	};
	legs.read_game_config = [&p_read_game_config]() {
		return p_read_game_config.is_valid() && bool(p_read_game_config.call());
	};
	return opennova::frontend::boot(legs);
}

void GameFrontend::set_launch_mission(const String &p_name) {
	frontend_.set_launch_mission(opennova::to_std(p_name));
}

String GameFrontend::take_launch_mission() {
	return opennova::to_gd(frontend_.take_launch_mission());
}

GameFrontend::ExitRoute GameFrontend::route_exit(bool p_has_session, int p_stored_reason,
		bool p_mp_session, bool p_joiner, bool p_mission_box_named) {
	opennova::frontend::MissionExit exit;
	exit.has_session = p_has_session;
	exit.stored_reason = p_stored_reason;
	exit.mp_session = p_mp_session;
	exit.joiner = p_joiner;
	exit.mission_box_named = p_mission_box_named;
	const opennova::frontend::ExitDecision decision = frontend_.route_exit(exit);
	exit_reason_ = decision.reason;
	return static_cast<ExitRoute>(decision.route);
}

bool GameFrontend::open_screen(int p_screen) {
	return frontend_.open_screen(static_cast<opennova::menu::MissionScreen>(p_screen));
}

GameFrontend::ScreenClose GameFrontend::close_screen(int p_screen) {
	return static_cast<ScreenClose>(
			frontend_.close_screen(static_cast<opennova::menu::MissionScreen>(p_screen)));
}

int GameFrontend::bank_menu_frame() {
	s_last_menu_updates = frontend_.bank_menu_frame();
	return s_last_menu_updates;
}

void GameFrontend::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_leg", "leg", "callable"), &GameFrontend::set_leg);
	ClassDB::bind_static_method("GameFrontend",
			D_METHOD("boot", "force_num_lock", "read_game_config"), &GameFrontend::boot);
	ClassDB::bind_method(D_METHOD("get_mode"), &GameFrontend::get_mode);
	ClassDB::bind_method(D_METHOD("state_name"), &GameFrontend::state_name);
	ClassDB::bind_method(D_METHOD("is_loading"), &GameFrontend::is_loading);
	ClassDB::bind_method(D_METHOD("in_menu"), &GameFrontend::in_menu);
	ClassDB::bind_method(D_METHOD("in_world"), &GameFrontend::in_world);
	ClassDB::bind_method(D_METHOD("get_screen"), &GameFrontend::get_screen);
	ClassDB::bind_method(D_METHOD("is_screen_open"), &GameFrontend::is_screen_open);
	ClassDB::bind_method(D_METHOD("screen_holds_input"), &GameFrontend::screen_holds_input);
	ClassDB::bind_method(D_METHOD("set_pass_through", "word"), &GameFrontend::set_pass_through);
	ClassDB::bind_method(D_METHOD("enter_main_menu"), &GameFrontend::enter_main_menu);
	ClassDB::bind_method(D_METHOD("reload_main_menu"), &GameFrontend::reload_main_menu);
	ClassDB::bind_method(D_METHOD("start_single_player"), &GameFrontend::start_single_player);
	ClassDB::bind_method(D_METHOD("start_mission", "in_session", "restart"),
			&GameFrontend::start_mission);
	ClassDB::bind_method(D_METHOD("start_join"), &GameFrontend::start_join);
	ClassDB::bind_method(D_METHOD("join_game_starting"), &GameFrontend::join_game_starting);
	ClassDB::bind_method(D_METHOD("set_launch_mission", "name"), &GameFrontend::set_launch_mission);
	ClassDB::bind_method(D_METHOD("take_launch_mission"), &GameFrontend::take_launch_mission);
	ClassDB::bind_method(D_METHOD("mission_loaded", "joiner", "joined_in_match"),
			&GameFrontend::mission_loaded);
	ClassDB::bind_method(D_METHOD("finish_load"), &GameFrontend::finish_load);
	ClassDB::bind_method(D_METHOD("load_failed", "join_screen_open"), &GameFrontend::load_failed);
	ClassDB::bind_method(D_METHOD("has_restart_entry"), &GameFrontend::has_restart_entry);
	ClassDB::bind_method(D_METHOD("route_exit", "has_session", "stored_reason", "mp_session",
								 "joiner", "mission_box_named"),
			&GameFrontend::route_exit);
	ClassDB::bind_method(D_METHOD("get_exit_reason"), &GameFrontend::get_exit_reason);
	ClassDB::bind_method(D_METHOD("record_round_end", "in_session", "round_over"),
			&GameFrontend::record_round_end);
	ClassDB::bind_method(D_METHOD("teardown_mission"), &GameFrontend::teardown_mission);
	ClassDB::bind_method(D_METHOD("enter_post_menu"), &GameFrontend::enter_post_menu);
	ClassDB::bind_method(D_METHOD("open_screen", "screen"), &GameFrontend::open_screen);
	ClassDB::bind_method(D_METHOD("close_screen", "screen"), &GameFrontend::close_screen);
	ClassDB::bind_method(D_METHOD("pause"), &GameFrontend::pause);
	ClassDB::bind_method(D_METHOD("resume"), &GameFrontend::resume);
	ClassDB::bind_method(D_METHOD("escape", "join_screen_open", "round_ended", "tools_interacting"),
			&GameFrontend::escape);
	ClassDB::bind_method(D_METHOD("use_item", "armory", "vehicle", "vehicle_team_matches"),
			&GameFrontend::use_item);
	ClassDB::bind_method(D_METHOD("weapon_screen_toggle"), &GameFrontend::weapon_screen_toggle);
	ClassDB::bind_method(D_METHOD("gameplay_input_active", "round_ended", "tools_interacting"),
			&GameFrontend::gameplay_input_active);
	ClassDB::bind_method(D_METHOD("keyboard_pass_owns_keys", "tools_interacting"),
			&GameFrontend::keyboard_pass_owns_keys);
	ClassDB::bind_method(D_METHOD("screen_input_frame", "tools_interacting"),
			&GameFrontend::screen_input_frame);
	ClassDB::bind_method(D_METHOD("input_frame_drains_keys", "tools_interacting"),
			&GameFrontend::input_frame_drains_keys);
	ClassDB::bind_method(D_METHOD("keyboard_captured", "chat_capturing", "tools_interacting"),
			&GameFrontend::keyboard_captured);
	ClassDB::bind_method(D_METHOD("hud_frame_runs"), &GameFrontend::hud_frame_runs);
	ClassDB::bind_method(D_METHOD("request_quit"), &GameFrontend::request_quit);
	ClassDB::bind_method(D_METHOD("begin_quit_drain"), &GameFrontend::begin_quit_drain);
	ClassDB::bind_method(D_METHOD("end_quit_drain"), &GameFrontend::end_quit_drain);
	ClassDB::bind_method(D_METHOD("is_quit_drain_pending"), &GameFrontend::is_quit_drain_pending);
	ClassDB::bind_method(D_METHOD("begin_process_exit"), &GameFrontend::begin_process_exit);
	ClassDB::bind_method(D_METHOD("is_process_exit_pending"),
			&GameFrontend::is_process_exit_pending);
	ClassDB::bind_method(D_METHOD("exit_to_desktop"), &GameFrontend::exit_to_desktop);
	ClassDB::bind_method(D_METHOD("fail_boot"), &GameFrontend::fail_boot);
	ClassDB::bind_method(D_METHOD("save_options"), &GameFrontend::save_options);
	ClassDB::bind_method(D_METHOD("bank_menu_frame"), &GameFrontend::bank_menu_frame);
	ClassDB::bind_static_method("GameFrontend", D_METHOD("last_menu_updates"),
			&GameFrontend::last_menu_updates);

	BIND_ENUM_CONSTANT(LEG_PROFILES);
	BIND_ENUM_CONSTANT(LEG_GAME_CONFIG);
	BIND_ENUM_CONSTANT(LEG_SAVE_PROFILE);
	BIND_ENUM_CONSTANT(LEG_SAVE_GAME_CONFIG);
	BIND_ENUM_CONSTANT(LEG_READ_MENU_CONFIG);
	BIND_ENUM_CONSTANT(LEG_RELOAD_MENU_CONFIG);
	BIND_ENUM_CONSTANT(LEG_LOAD_PROFILE);
	BIND_ENUM_CONSTANT(LEG_LOAD_COOKIE_JAR);
	BIND_ENUM_CONSTANT(LEG_SAVE_COOKIE_JAR);
	BIND_ENUM_CONSTANT(LEG_APPLY_CONTROLS);
	BIND_ENUM_CONSTANT(LEG_RELOAD_OPTIONS);
	BIND_ENUM_CONSTANT(LEG_SHOW_MENU);
	BIND_ENUM_CONSTANT(LEG_RESET_KEY_QUEUE);
	BIND_ENUM_CONSTANT(LEG_LOAD_CAN_START);
	BIND_ENUM_CONSTANT(LEG_STOP_LAN_BROWSE);
	BIND_ENUM_CONSTANT(LEG_STOP_MUSIC);
	BIND_ENUM_CONSTANT(LEG_REMEMBER_RESTART_ENTRY);
	BIND_ENUM_CONSTANT(LEG_OPEN_JOIN_SCREEN);
	BIND_ENUM_CONSTANT(LEG_BEGIN_WORLD_LOAD);
	BIND_ENUM_CONSTANT(LEG_BEGIN_LOAD);
	BIND_ENUM_CONSTANT(LEG_LOAD_FAILED);
	BIND_ENUM_CONSTANT(LEG_EXIT_PROCESS);
	BIND_ENUM_CONSTANT(MODE_NONE);
	BIND_ENUM_CONSTANT(MODE_MAIN_MENU);
	BIND_ENUM_CONSTANT(MODE_PRE_MENU);
	BIND_ENUM_CONSTANT(MODE_GAME_LOOP);
	BIND_ENUM_CONSTANT(MODE_POST_MENU);
	BIND_ENUM_CONSTANT(ESCAPE_NONE);
	BIND_ENUM_CONSTANT(ESCAPE_CANCEL_JOIN_SCREEN);
	BIND_ENUM_CONSTANT(ESCAPE_CANCEL_LOAD);
	BIND_ENUM_CONSTANT(ESCAPE_ACTION);
	BIND_ENUM_CONSTANT(ESCAPE_RESUME);
	BIND_ENUM_CONSTANT(ESCAPE_CLOSE_SCREEN);
	BIND_ENUM_CONSTANT(USE_NONE);
	BIND_ENUM_CONSTANT(USE_CLOSE_SCREEN);
	BIND_ENUM_CONSTANT(USE_OPEN_ARMORY);
	BIND_ENUM_CONSTANT(USE_OPEN_VEHICLE);
	BIND_ENUM_CONSTANT(EXIT_NONE);
	BIND_ENUM_CONSTANT(EXIT_RESTART_ROUND);
	BIND_ENUM_CONSTANT(EXIT_MISSION_BOX);
	BIND_ENUM_CONSTANT(EXIT_LEAVE_TO_MENU);
	BIND_ENUM_CONSTANT(EXIT_JOINER_RELOAD);
	BIND_ENUM_CONSTANT(EXIT_MAP_CHANGE);
	BIND_ENUM_CONSTANT(EXIT_ABORT);
	BIND_ENUM_CONSTANT(STARTED_AWAIT_ADMISSION);
	BIND_ENUM_CONSTANT(STARTED_REVEAL);
	BIND_ENUM_CONSTANT(STARTED_RESET_AND_REVEAL);
	BIND_ENUM_CONSTANT(QUIT_BEGIN);
	BIND_ENUM_CONSTANT(QUIT_FINISH_DRAIN);
	BIND_ENUM_CONSTANT(QUIT_IGNORED);
	BIND_ENUM_CONSTANT(CLOSE_NONE);
	BIND_ENUM_CONSTANT(CLOSE_CLOSED);
	BIND_ENUM_CONSTANT(CLOSE_PASS_THROUGH);
}
