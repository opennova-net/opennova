#include "hud/hud_toggles.h"

using namespace godot;

int HudToggles::poll(bool p_huddetail, bool p_hudcolor, bool p_rows_share_key, bool p_showhud,
		bool p_dotsize, bool p_goals, bool p_view1st, bool p_viewwithgun, bool p_viewchase,
		bool p_playerlist, bool p_old_messages, bool p_show_score, bool p_chorded, bool p_active,
		bool p_in_session) {
	opennova::hud::HudKeyPoll keys;
	keys.huddetail = p_huddetail;
	keys.hudcolor = p_hudcolor;
	keys.rows_share_key = p_rows_share_key;
	keys.showhud = p_showhud;
	keys.dotsize = p_dotsize;
	keys.goals = p_goals;
	keys.view1st = p_view1st;
	keys.viewwithgun = p_viewwithgun;
	keys.viewchase = p_viewchase;
	keys.playerlist = p_playerlist;
	keys.old_messages = p_old_messages;
	keys.show_score = p_show_score;
	keys.chorded = p_chorded;
	keys.active = p_active;
	keys.in_session = p_in_session;
	return static_cast<int>(opennova::hud::hud_toggles_poll(state_, keys));
}

void HudToggles::reset_mission() { opennova::hud::hud_toggles_reset_mission(state_); }
void HudToggles::force_death_screen_hud_detail() { opennova::hud::hud_toggles_death_screen(state_); }
String HudToggles::cycle_friendly_tags() {
	return String(opennova::hud::hud_toggles_cycle_friendly_tags(state_));
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
void HudToggles::close_message_log() { state_.message_log_open = false; }

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
void HudToggles::set_objectives_visible(bool p_visible) { state_.objectives_visible = p_visible; }
bool HudToggles::is_scoreboard_open() const { return state_.scoreboard_open; }
bool HudToggles::is_message_log_open() const { return state_.message_log_open; }
bool HudToggles::is_end_round_stats_open() const { return state_.end_round_stats_open; }

void HudToggles::_bind_methods() {
	ClassDB::bind_method(D_METHOD("poll", "huddetail", "hudcolor", "rows_share_key", "showhud",
								 "dotsize", "goals", "view1st", "viewwithgun", "viewchase",
								 "playerlist", "old_messages", "show_score", "chorded", "active",
								 "in_session"),
			&HudToggles::poll);
	ClassDB::bind_method(D_METHOD("reset_mission"), &HudToggles::reset_mission);
	ClassDB::bind_method(D_METHOD("force_death_screen_hud_detail"),
			&HudToggles::force_death_screen_hud_detail);
	ClassDB::bind_method(D_METHOD("cycle_friendly_tags"), &HudToggles::cycle_friendly_tags);
	ClassDB::bind_method(D_METHOD("cycle_hud_color"), &HudToggles::cycle_hud_color);
	ClassDB::bind_method(D_METHOD("cycle_hud_detail"), &HudToggles::cycle_hud_detail);
	ClassDB::bind_method(D_METHOD("cycle_showhud"), &HudToggles::cycle_showhud);
	ClassDB::bind_method(D_METHOD("toggle_objectives"), &HudToggles::toggle_objectives);
	ClassDB::bind_method(D_METHOD("close_message_log"), &HudToggles::close_message_log);
	ClassDB::bind_method(D_METHOD("get_hud_color_index"), &HudToggles::get_hud_color_index);
	ClassDB::bind_method(D_METHOD("set_hud_color_index", "index"), &HudToggles::set_hud_color_index);
	ClassDB::bind_method(D_METHOD("get_hud_detail_level"), &HudToggles::get_hud_detail_level);
	ClassDB::bind_method(D_METHOD("set_hud_detail_level", "level"), &HudToggles::set_hud_detail_level);
	ClassDB::bind_method(D_METHOD("get_showhud_flags"), &HudToggles::get_showhud_flags);
	ClassDB::bind_method(D_METHOD("set_showhud_flags", "flags"), &HudToggles::set_showhud_flags);
	ClassDB::bind_method(D_METHOD("get_friendly_tag_mode"), &HudToggles::get_friendly_tag_mode);
	ClassDB::bind_method(D_METHOD("is_objectives_visible"), &HudToggles::is_objectives_visible);
	ClassDB::bind_method(D_METHOD("set_objectives_visible", "visible"), &HudToggles::set_objectives_visible);
	ClassDB::bind_method(D_METHOD("is_scoreboard_open"), &HudToggles::is_scoreboard_open);
	ClassDB::bind_method(D_METHOD("is_message_log_open"), &HudToggles::is_message_log_open);
	ClassDB::bind_method(D_METHOD("is_end_round_stats_open"), &HudToggles::is_end_round_stats_open);
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
}
