#include "hud/hud_toggles.h"

using namespace godot;

int HudToggles::poll(int p_rows_down, bool p_rows_share_key, bool p_chorded, bool p_active,
		bool p_in_session, bool p_objective_game, bool p_local_alive, bool p_authority,
		bool p_mp_session_peer) {
	opennova::hud::HudKeyPoll keys;
	opennova::hud::hud_key_poll_set_rows(keys, static_cast<uint32_t>(p_rows_down));
	keys.rows_share_key = p_rows_share_key;
	keys.chorded = p_chorded;
	keys.active = p_active;
	keys.in_session = p_in_session;
	keys.objective_game = p_objective_game;
	keys.local_alive = p_local_alive;
	keys.authority = p_authority;
	keys.mp_session_peer = p_mp_session_peer;
	return static_cast<int>(opennova::hud::hud_toggles_poll(state_, keys));
}

int HudToggles::escape(bool p_in_session, bool p_spawn_gate, bool p_authority) {
	opennova::hud::HudEscapeInput input;
	input.in_session = p_in_session;
	input.spawn_gate = p_spawn_gate;
	input.authority = p_authority;
	return static_cast<int>(opennova::hud::hud_toggles_escape(state_, input));
}

void HudToggles::session_init(bool p_in_session, bool p_mp_session_peer) {
	opennova::hud::hud_toggles_session_init(state_, p_in_session, p_mp_session_peer);
}

int HudToggles::quit_dialog_key(int p_vk, bool p_in_session, int p_yes_vk, int p_no_vk,
		int p_restart_vk) {
	opennova::hud::HudQuitDialogKeyInput input;
	input.vk = p_vk;
	input.in_session = p_in_session;
	input.yes_vk = p_yes_vk;
	input.no_vk = p_no_vk;
	input.restart_vk = p_restart_vk;
	return static_cast<int>(opennova::hud::hud_toggles_quit_dialog_key(state_, input));
}

bool HudToggles::server_status_page_key(int p_vk, bool p_in_session, bool p_authority,
		bool p_mp_session_peer) const {
	return opennova::hud::hud_toggles_server_status_page_key(state_, p_vk, p_in_session,
			p_authority, p_mp_session_peer);
}

String HudToggles::row_token(int p_row) { return String(opennova::hud::hud_toggle_row_token(p_row)); }

void HudToggles::reset_mission() { opennova::hud::hud_toggles_reset_mission(state_); }
int HudToggles::cine_start(bool p_in_session) {
	return static_cast<int>(opennova::hud::hud_toggles_cine_start(state_, p_in_session));
}
void HudToggles::force_death_screen_hud_detail() { opennova::hud::hud_toggles_death_screen(state_); }
String HudToggles::cycle_friendly_tags() {
	return String(opennova::hud::hud_toggles_cycle_friendly_tags(state_));
}
String HudToggles::friendly_tag_toast_key() const {
	return String(opennova::hud::friendly_tag_toast_key(state_.friendly_tag_mode));
}
String HudToggles::verbose_toast_key() const {
	return String(opennova::hud::verbose_toast_key(state_.mp_verbose));
}
void HudToggles::cycle_hud_color() {
	state_.hud_color_index = opennova::hud::next_hud_color_index(state_.hud_color_index);
}
void HudToggles::cycle_hud_detail() {
	state_.hud_detail_level = opennova::hud::next_hud_detail_level(state_.hud_detail_level);
}
void HudToggles::cycle_showhud() {
	state_.showhud_flags = opennova::hud::next_showhud_flags(state_.showhud_flags);
}
void HudToggles::toggle_objectives() { state_.objectives_visible = !state_.objectives_visible; }

int HudToggles::get_hud_color_index() const { return state_.hud_color_index; }
void HudToggles::set_hud_color_index(int p_index) {
	state_.hud_color_index = opennova::hud::clamp_hud_color_index(p_index);
}
int HudToggles::get_hud_detail_level() const { return state_.hud_detail_level; }
// Stored verbatim: retail applies the cfg token raw (hud_config_tokens.h).
void HudToggles::set_hud_detail_level(int p_level) { state_.hud_detail_level = p_level; }
int HudToggles::get_showhud_flags() const { return static_cast<int>(state_.showhud_flags); }
void HudToggles::set_showhud_flags(int p_flags) {
	state_.showhud_flags = static_cast<uint32_t>(p_flags) & 3u;
}
int HudToggles::get_friendly_tag_mode() const { return static_cast<int>(state_.friendly_tag_mode); }
bool HudToggles::is_objectives_visible() const { return state_.objectives_visible; }
bool HudToggles::is_scoreboard_open() const { return state_.scoreboard_open; }
bool HudToggles::is_message_log_open() const { return state_.message_log_open; }
bool HudToggles::is_end_round_stats_open() const { return state_.end_round_stats_open; }
bool HudToggles::is_help_open() const { return state_.help_open; }
bool HudToggles::is_map_legend_open() const { return state_.map_legend_open; }
int HudToggles::get_briefing_mode() const { return state_.briefing_mode; }
bool HudToggles::is_mp_verbose() const { return state_.mp_verbose; }
bool HudToggles::is_emotes_menu_open() const { return state_.emotes_menu_open; }
bool HudToggles::is_radio_menu_open() const { return state_.radio_menu_open; }
void HudToggles::close_voice_menu(bool p_radio) {
	opennova::hud::hud_toggles_close_voice_menu(state_, p_radio);
}
bool HudToggles::is_paused() const { return state_.paused; }
bool HudToggles::is_server_status_view() const { return state_.server_status_view; }
bool HudToggles::is_server_status_score_list_open() const {
	return state_.server_status_score_list;
}
bool HudToggles::is_quit_dialog_open() const { return state_.quit_dialog_open; }
void HudToggles::set_paused(bool p_paused) { state_.paused = p_paused; }
void HudToggles::apply_tip_events(const PackedByteArray &p_events) {
	opennova::hud::hud_toggles_tip_events(state_, p_events.ptr(),
			static_cast<size_t>(p_events.size()));
}
void HudToggles::advance_tip_frames(int p_frames) {
	opennova::hud::hud_toggles_tip_frames(state_, p_frames);
}
void HudToggles::restart_round() { opennova::hud::hud_toggles_restart_round(state_); }
void HudToggles::set_tip_options(bool p_keyboard_tips, bool p_gameplay_tips) {
	state_.tips.options.keyboard_tips = p_keyboard_tips;
	state_.tips.options.gameplay_tips = p_gameplay_tips;
}
int HudToggles::get_tip() const { return state_.tips.tip; }
int HudToggles::get_tip_countdown() const { return state_.tips.countdown; }
bool HudToggles::is_tip_showing() const { return opennova::hud::tip_is_showing(state_.tips); }

void HudToggles::_bind_methods() {
	ClassDB::bind_method(D_METHOD("poll", "rows_down", "rows_share_key", "chorded", "active",
								 "in_session", "objective_game", "local_alive", "authority",
								 "mp_session_peer"),
			&HudToggles::poll, DEFVAL(true), DEFVAL(false), DEFVAL(false));
	ClassDB::bind_method(D_METHOD("escape", "in_session", "spawn_gate", "authority"),
			&HudToggles::escape, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("session_init", "in_session", "mp_session_peer"),
			&HudToggles::session_init);
	ClassDB::bind_method(D_METHOD("quit_dialog_key", "vk", "in_session", "yes_vk", "no_vk",
								 "restart_vk"),
			&HudToggles::quit_dialog_key);
	ClassDB::bind_method(D_METHOD("server_status_page_key", "vk", "in_session", "authority",
								 "mp_session_peer"),
			&HudToggles::server_status_page_key);
	ClassDB::bind_method(D_METHOD("is_server_status_view"), &HudToggles::is_server_status_view);
	ClassDB::bind_method(D_METHOD("is_server_status_score_list_open"),
			&HudToggles::is_server_status_score_list_open);
	ClassDB::bind_method(D_METHOD("is_quit_dialog_open"), &HudToggles::is_quit_dialog_open);
	ClassDB::bind_static_method("HudToggles", D_METHOD("row_token", "row"), &HudToggles::row_token);
	ClassDB::bind_method(D_METHOD("reset_mission"), &HudToggles::reset_mission);
	ClassDB::bind_method(D_METHOD("cine_start", "in_session"), &HudToggles::cine_start);
	ClassDB::bind_method(D_METHOD("force_death_screen_hud_detail"),
			&HudToggles::force_death_screen_hud_detail);
	ClassDB::bind_method(D_METHOD("cycle_friendly_tags"), &HudToggles::cycle_friendly_tags);
	ClassDB::bind_method(D_METHOD("friendly_tag_toast_key"), &HudToggles::friendly_tag_toast_key);
	ClassDB::bind_method(D_METHOD("verbose_toast_key"), &HudToggles::verbose_toast_key);
	ClassDB::bind_method(D_METHOD("cycle_hud_color"), &HudToggles::cycle_hud_color);
	ClassDB::bind_method(D_METHOD("cycle_hud_detail"), &HudToggles::cycle_hud_detail);
	ClassDB::bind_method(D_METHOD("cycle_showhud"), &HudToggles::cycle_showhud);
	ClassDB::bind_method(D_METHOD("toggle_objectives"), &HudToggles::toggle_objectives);
	ClassDB::bind_method(D_METHOD("get_hud_color_index"), &HudToggles::get_hud_color_index);
	ClassDB::bind_method(D_METHOD("set_hud_color_index", "index"), &HudToggles::set_hud_color_index);
	ClassDB::bind_method(D_METHOD("get_hud_detail_level"), &HudToggles::get_hud_detail_level);
	ClassDB::bind_method(D_METHOD("set_hud_detail_level", "level"), &HudToggles::set_hud_detail_level);
	ClassDB::bind_method(D_METHOD("get_showhud_flags"), &HudToggles::get_showhud_flags);
	ClassDB::bind_method(D_METHOD("set_showhud_flags", "flags"), &HudToggles::set_showhud_flags);
	ClassDB::bind_method(D_METHOD("get_friendly_tag_mode"), &HudToggles::get_friendly_tag_mode);
	ClassDB::bind_method(D_METHOD("is_objectives_visible"), &HudToggles::is_objectives_visible);
	ClassDB::bind_method(D_METHOD("is_scoreboard_open"), &HudToggles::is_scoreboard_open);
	ClassDB::bind_method(D_METHOD("is_message_log_open"), &HudToggles::is_message_log_open);
	ClassDB::bind_method(D_METHOD("is_end_round_stats_open"), &HudToggles::is_end_round_stats_open);
	ClassDB::bind_method(D_METHOD("is_help_open"), &HudToggles::is_help_open);
	ClassDB::bind_method(D_METHOD("is_map_legend_open"), &HudToggles::is_map_legend_open);
	ClassDB::bind_method(D_METHOD("get_briefing_mode"), &HudToggles::get_briefing_mode);
	ClassDB::bind_method(D_METHOD("is_mp_verbose"), &HudToggles::is_mp_verbose);
	ClassDB::bind_method(D_METHOD("is_emotes_menu_open"), &HudToggles::is_emotes_menu_open);
	ClassDB::bind_method(D_METHOD("is_radio_menu_open"), &HudToggles::is_radio_menu_open);
	ClassDB::bind_method(D_METHOD("close_voice_menu", "radio"), &HudToggles::close_voice_menu);
	ClassDB::bind_method(D_METHOD("is_paused"), &HudToggles::is_paused);
	ClassDB::bind_method(D_METHOD("set_paused", "paused"), &HudToggles::set_paused);
	ClassDB::bind_method(D_METHOD("apply_tip_events", "events"), &HudToggles::apply_tip_events);
	ClassDB::bind_method(D_METHOD("advance_tip_frames", "frames"), &HudToggles::advance_tip_frames);
	ClassDB::bind_method(D_METHOD("restart_round"), &HudToggles::restart_round);
	ClassDB::bind_method(D_METHOD("set_tip_options", "keyboard_tips", "gameplay_tips"),
			&HudToggles::set_tip_options);
	ClassDB::bind_method(D_METHOD("get_tip"), &HudToggles::get_tip);
	ClassDB::bind_method(D_METHOD("get_tip_countdown"), &HudToggles::get_tip_countdown);
	ClassDB::bind_method(D_METHOD("is_tip_showing"), &HudToggles::is_tip_showing);
	BIND_ENUM_CONSTANT(EVENT_HUD_DETAIL_CYCLED);
	BIND_ENUM_CONSTANT(EVENT_HUD_COLOR_CYCLED);
	BIND_ENUM_CONSTANT(EVENT_SHOWHUD_CYCLED);
	BIND_ENUM_CONSTANT(EVENT_DOTSIZE_CYCLED);
	BIND_ENUM_CONSTANT(EVENT_OBJECTIVES_TOGGLED);
	BIND_ENUM_CONSTANT(EVENT_GUN_BIT_CHANGED);
	BIND_ENUM_CONSTANT(EVENT_FIRST_PERSON_SELECTED);
	BIND_ENUM_CONSTANT(EVENT_THIRD_PERSON_SELECTED);
	BIND_ENUM_CONSTANT(EVENT_GUN_VIEW_SELECTED);
	BIND_ENUM_CONSTANT(EVENT_SCOREBOARD_TOGGLED);
	BIND_ENUM_CONSTANT(EVENT_MESSAGE_LOG_TOGGLED);
	BIND_ENUM_CONSTANT(EVENT_SHOW_SCORE_TOGGLED);
	BIND_ENUM_CONSTANT(EVENT_OVERLAY_WINDOWS_CLEARED);
	BIND_ENUM_CONSTANT(EVENT_FRIENDLY_TAGS_CYCLED);
	BIND_ENUM_CONSTANT(EVENT_HELP_TOGGLED);
	BIND_ENUM_CONSTANT(EVENT_MAP_LEGEND_TOGGLED);
	BIND_ENUM_CONSTANT(EVENT_BRIEFING_TOGGLED);
	BIND_ENUM_CONSTANT(EVENT_BRIEFING_PAGES_RESET);
	BIND_ENUM_CONSTANT(EVENT_VERBOSE_TOGGLED);
	BIND_ENUM_CONSTANT(EVENT_ESCAPE_CLOSED_WINDOW);
	BIND_ENUM_CONSTANT(EVENT_ESCAPE_OPEN_MENU);
	BIND_ENUM_CONSTANT(EVENT_COMMAND_MAP_OPENED);
	BIND_ENUM_CONSTANT(EVENT_SCOREBOARD_PAGE_RESET);
	BIND_ENUM_CONSTANT(EVENT_PAUSE_TOGGLED);
	BIND_ENUM_CONSTANT(EVENT_PAUSE_CLEARED);
	BIND_ENUM_CONSTANT(EVENT_SERVER_STATUS_VIEW_TOGGLED);
	BIND_ENUM_CONSTANT(EVENT_SERVER_STATUS_SCORE_LIST_TOGGLED);
	BIND_ENUM_CONSTANT(EVENT_QUIT_DIALOG_OPENED);
	BIND_ENUM_CONSTANT(SPECIAL_KEY_CONSUMED);
	BIND_ENUM_CONSTANT(SPECIAL_KEY_CHAIN_TAKEN);
	BIND_ENUM_CONSTANT(SPECIAL_KEY_QUIT_CONFIRMED);
	BIND_ENUM_CONSTANT(SPECIAL_KEY_RESTART_QUEUED);
	BIND_ENUM_CONSTANT(ROUND_OVER_CONSUMED);
	BIND_ENUM_CONSTANT(ROUND_OVER_RESTART);
	BIND_ENUM_CONSTANT(ROUND_OVER_EXIT);
	BIND_ENUM_CONSTANT(ROW_HUD_DETAIL);
	BIND_ENUM_CONSTANT(ROW_HUD_COLOR);
	BIND_ENUM_CONSTANT(ROW_SHOWHUD);
	BIND_ENUM_CONSTANT(ROW_DOTSIZE);
	BIND_ENUM_CONSTANT(ROW_GOALS);
	BIND_ENUM_CONSTANT(ROW_VIEW1ST);
	BIND_ENUM_CONSTANT(ROW_VIEW_WITH_GUN);
	BIND_ENUM_CONSTANT(ROW_VIEW_CHASE);
	BIND_ENUM_CONSTANT(ROW_PLAYER_LIST);
	BIND_ENUM_CONSTANT(ROW_OLD_MESSAGES);
	BIND_ENUM_CONSTANT(ROW_SHOW_SCORE);
	BIND_ENUM_CONSTANT(ROW_FRIENDLY_TAGS);
	BIND_ENUM_CONSTANT(ROW_HELP);
	BIND_ENUM_CONSTANT(ROW_HELP_MAP);
	BIND_ENUM_CONSTANT(ROW_BRIEFING);
	BIND_ENUM_CONSTANT(ROW_VERBOSE);
	BIND_ENUM_CONSTANT(ROW_COMMANDER_MENU);
	BIND_ENUM_CONSTANT(ROW_PAUSE);
	BIND_ENUM_CONSTANT(ROW_AUDIO_EMOTE);
	BIND_ENUM_CONSTANT(ROW_RADIO_MACRO);
	BIND_ENUM_CONSTANT(ROW_TOGGLE_SERVER);
	BIND_ENUM_CONSTANT(ROW_COUNT);
}
