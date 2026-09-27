#include "network/host_session_options.h"

#include "util/string_convert.h"

#include <runtime/inmatch/host_settings.h>

using namespace godot;
using opennova::to_gd;
using opennova::to_std;

#define HOST_SESSION_TEXT_IMPL(m_name)                                                 \
	String HostSessionOptions::get_##m_name() const { return to_gd(config_.m_name); }   \
	void HostSessionOptions::set_##m_name(const String &p_value) {                      \
		config_.m_name = to_std(p_value);                                               \
	}
HOST_SESSION_TEXT_IMPL(server_name)
HOST_SESSION_TEXT_IMPL(server_password)
HOST_SESSION_TEXT_IMPL(side_a_password)
HOST_SESSION_TEXT_IMPL(side_b_password)
HOST_SESSION_TEXT_IMPL(country)
HOST_SESSION_TEXT_IMPL(mission_name)
HOST_SESSION_TEXT_IMPL(mission_file)
HOST_SESSION_TEXT_IMPL(custom_text)
HOST_SESSION_TEXT_IMPL(player_name)
HOST_SESSION_TEXT_IMPL(expansion)
HOST_SESSION_TEXT_IMPL(integrity_profile)
HOST_SESSION_TEXT_IMPL(spectator_password)
#undef HOST_SESSION_TEXT_IMPL

String HostSessionOptions::get_channel() const {
	switch (config_.session_channel) {
		case opennova::inmatch::GameSessionChannel::NovaWorld:
			return "NovaWorld";
		case opennova::inmatch::GameSessionChannel::Lan:
			return "LAN";
		case opennova::inmatch::GameSessionChannel::SinglePlayer:
			return "SinglePlayer";
		default:
			return String();
	}
}

void HostSessionOptions::set_channel(const String &p_channel) {
	if (p_channel.nocasecmp_to("NovaWorld") == 0) {
		config_.session_channel = opennova::inmatch::GameSessionChannel::NovaWorld;
	} else if (p_channel.nocasecmp_to("SinglePlayer") == 0) {
		config_.session_channel = opennova::inmatch::GameSessionChannel::SinglePlayer;
	} else if (p_channel.is_empty()) {
		config_.session_channel = opennova::inmatch::GameSessionChannel::Automatic;
	} else {
		config_.session_channel = opennova::inmatch::GameSessionChannel::Lan;
	}
}

int HostSessionOptions::get_send_holdoff_ticks() const {
	return config_.send_holdoff_ticks.has_value()
			? static_cast<int>(*config_.send_holdoff_ticks)
			: -1;
}

void HostSessionOptions::set_send_holdoff_ticks(int p_ticks) {
	if (p_ticks < 0) {
		config_.send_holdoff_ticks.reset();
	} else {
		config_.send_holdoff_ticks = static_cast<uint32_t>(p_ticks);
	}
}

PackedStringArray HostSessionOptions::get_spawn_names() const {
	PackedStringArray out;
	for (const std::string &name : config_.spawn_names) {
		out.push_back(to_gd(name));
	}
	return out;
}

void HostSessionOptions::set_spawn_names(const PackedStringArray &p_names) {
	config_.spawn_names.clear();
	for (int64_t i = 0; i < p_names.size(); ++i) {
		if (!p_names[i].is_empty()) {
			config_.spawn_names.emplace_back(to_std(p_names[i]));
		}
	}
}

Dictionary HostSessionOptions::to_json_value() const {
	Dictionary out;
	out["bind_port"] = bind_port_;
	out["server_name"] = get_server_name();
	out["mission_name"] = get_mission_name();
	out["mission_file"] = get_mission_file();
	out["custom_text"] = get_custom_text();
	out["player_name"] = get_player_name();
	out["expansion"] = get_expansion();
	out["integrity_profile"] = get_integrity_profile();
	out["spectator_slots"] = get_spectator_slots();
	out["spectator_password"] = get_spectator_password();
	out["game_type"] = get_game_type();
	out["mp_attributes"] = get_mp_attributes();
	out["class_allow_mask"] = get_class_allow_mask();
	out["respawn_time"] = get_respawn_time();
	out["time_limit_minutes"] = get_time_limit_minutes();
	out["replay_enabled"] = get_replay_enabled();
	out["max_team_lives"] = get_max_team_lives();
	out["score_limit"] = get_score_limit();
	out["max_score"] = get_max_score();
	out["koth_delta"] = get_koth_delta();
	out["flag_return_ticks"] = get_flag_return_ticks();
	out["flag_reset_seconds"] = get_flag_reset_seconds();
	out["armory_reuse_time"] = get_armory_reuse_time();
	out["capture_duration_seconds"] = get_capture_duration_seconds();
	out["capture_speed_setting"] = get_capture_speed_setting();
	out["spawn_wave_time_base"] = get_spawn_wave_time_base();
	out["spawn_wave_time_zone"] = get_spawn_wave_time_zone();
	out["default_spawn_requires_no_team_zone"] = get_default_spawn_requires_no_team_zone();
	out["num_teams"] = get_num_teams();
	out["respawn_timeout"] = get_respawn_timeout();
	out["start_delay"] = get_start_delay();
	out["destroy_buildings"] = get_destroy_buildings();
	out["death_messages"] = get_death_messages();
	out["entity_send_budget"] = get_entity_send_budget();
	out["channel"] = get_channel();
	out["lan_mode"] = get_lan_mode();
	out["send_holdoff_ticks"] = get_send_holdoff_ticks();
	out["max_players"] = max_players_;
	out["serve_and_play"] = serve_and_play_;
	out["game_type_auto"] = game_type_auto_;
	out["fat_bullets"] = config_.fat_bullets;
	out["one_shot_kill"] = config_.one_shot_kill;
	out["unlimited_vehicles"] = config_.unlimited_vehicles;
	out["spawn_x"] = get_spawn_x();
	out["spawn_y"] = get_spawn_y();
	out["spawn_z"] = get_spawn_z();
	out["spawn_names"] = get_spawn_names();
	return out;
}

int HostSessionOptions::get_player_slot_limit() const {
	return static_cast<int>(opennova::inmatch::host_player_slot_limit(max_players_, serve_and_play_));
}

PackedStringArray HostSessionOptions::dialog_controls() {
	PackedStringArray result;
	for (const auto name : opennova::inmatch::host_dialog_controls())
		result.push_back(String::utf8(name.data(), static_cast<int64_t>(name.size())));
	return result;
}

bool HostSessionOptions::apply_dialog_control(const String &p_control, const String &p_value) {
	return opennova::inmatch::apply_host_dialog_control(config_, max_players_,
			serve_and_play_, to_std(p_control), to_std(p_value));
}

String HostSessionOptions::dialog_value(const String &p_control) const {
	return to_gd(opennova::inmatch::host_dialog_value(config_, max_players_,
			serve_and_play_, to_std(p_control)));
}

Ref<HostSessionOptions> HostSessionOptions::duplicate_options() const {
	Ref<HostSessionOptions> copy;
	copy.instantiate();
	copy->config_ = config_;
	copy->bind_port_ = bind_port_;
	copy->max_players_ = max_players_;
	copy->serve_and_play_ = serve_and_play_;
	copy->game_type_auto_ = game_type_auto_;
	copy->game_root_ = game_root_;
	copy->dir_ = dir_;
	copy->nw_gate_host_ = nw_gate_host_;
	copy->nw_gate_port_ = nw_gate_port_;
	copy->region_index_ = region_index_;
	copy->advertise_ = advertise_;
	return copy;
}

void HostSessionOptions::_bind_methods() {
#define HOST_SESSION_PROPERTY(m_variant, m_name)                                                    \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &HostSessionOptions::get_##m_name);              \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &HostSessionOptions::set_##m_name);     \
	ADD_PROPERTY(PropertyInfo(m_variant, #m_name), "set_" #m_name, "get_" #m_name);
	HOST_SESSION_PROPERTY(Variant::STRING, server_name)
	HOST_SESSION_PROPERTY(Variant::STRING, server_password)
	HOST_SESSION_PROPERTY(Variant::STRING, side_a_password)
	HOST_SESSION_PROPERTY(Variant::STRING, side_b_password)
	HOST_SESSION_PROPERTY(Variant::STRING, country)
	HOST_SESSION_PROPERTY(Variant::INT, server_punkbuster)
	HOST_SESSION_PROPERTY(Variant::INT, server_lan_only)
	HOST_SESSION_PROPERTY(Variant::INT, connection_speed)
	HOST_SESSION_PROPERTY(Variant::INT, max_friendly_kills)
	HOST_SESSION_PROPERTY(Variant::INT, allow_ai)
	HOST_SESSION_PROPERTY(Variant::INT, time_of_day_continuity)
	HOST_SESSION_PROPERTY(Variant::STRING, mission_name)
	HOST_SESSION_PROPERTY(Variant::STRING, mission_file)
	HOST_SESSION_PROPERTY(Variant::STRING, custom_text)
	HOST_SESSION_PROPERTY(Variant::STRING, player_name)
	HOST_SESSION_PROPERTY(Variant::STRING, expansion)
	HOST_SESSION_PROPERTY(Variant::STRING, integrity_profile)
	HOST_SESSION_PROPERTY(Variant::STRING, spectator_password)
	HOST_SESSION_PROPERTY(Variant::INT, spectator_slots)
	HOST_SESSION_PROPERTY(Variant::INT, game_type)
	HOST_SESSION_PROPERTY(Variant::INT, mp_attributes)
	HOST_SESSION_PROPERTY(Variant::INT, class_allow_mask)
	HOST_SESSION_PROPERTY(Variant::INT, respawn_time)
	HOST_SESSION_PROPERTY(Variant::INT, time_limit_minutes)
	HOST_SESSION_PROPERTY(Variant::INT, replay_enabled)
	HOST_SESSION_PROPERTY(Variant::INT, max_team_lives)
	HOST_SESSION_PROPERTY(Variant::INT, score_limit)
	HOST_SESSION_PROPERTY(Variant::INT, max_score)
	HOST_SESSION_PROPERTY(Variant::INT, koth_delta)
	HOST_SESSION_PROPERTY(Variant::INT, flag_return_ticks)
	HOST_SESSION_PROPERTY(Variant::INT, flag_reset_seconds)
	HOST_SESSION_PROPERTY(Variant::INT, armory_reuse_time)
	HOST_SESSION_PROPERTY(Variant::INT, capture_duration_seconds)
	HOST_SESSION_PROPERTY(Variant::INT, capture_speed_setting)
	HOST_SESSION_PROPERTY(Variant::INT, spawn_wave_time_base)
	HOST_SESSION_PROPERTY(Variant::INT, spawn_wave_time_zone)
	HOST_SESSION_PROPERTY(Variant::INT, default_spawn_requires_no_team_zone)
	HOST_SESSION_PROPERTY(Variant::INT, num_teams)
	HOST_SESSION_PROPERTY(Variant::INT, respawn_timeout)
	HOST_SESSION_PROPERTY(Variant::INT, start_delay)
	HOST_SESSION_PROPERTY(Variant::INT, destroy_buildings)
	HOST_SESSION_PROPERTY(Variant::INT, death_messages)
	HOST_SESSION_PROPERTY(Variant::INT, entity_send_budget)
	HOST_SESSION_PROPERTY(Variant::STRING, channel)
	HOST_SESSION_PROPERTY(Variant::INT, lan_mode)
	HOST_SESSION_PROPERTY(Variant::INT, send_holdoff_ticks)
	HOST_SESSION_PROPERTY(Variant::BOOL, fat_bullets)
	HOST_SESSION_PROPERTY(Variant::BOOL, one_shot_kill)
	HOST_SESSION_PROPERTY(Variant::BOOL, unlimited_vehicles)
	HOST_SESSION_PROPERTY(Variant::INT, spawn_x)
	HOST_SESSION_PROPERTY(Variant::INT, spawn_y)
	HOST_SESSION_PROPERTY(Variant::INT, spawn_z)
	HOST_SESSION_PROPERTY(Variant::PACKED_STRING_ARRAY, spawn_names)
	HOST_SESSION_PROPERTY(Variant::INT, bind_port)
	HOST_SESSION_PROPERTY(Variant::INT, max_players)
	HOST_SESSION_PROPERTY(Variant::BOOL, serve_and_play)
	HOST_SESSION_PROPERTY(Variant::BOOL, game_type_auto)
	HOST_SESSION_PROPERTY(Variant::STRING, game_root)
	HOST_SESSION_PROPERTY(Variant::STRING, dir)
	HOST_SESSION_PROPERTY(Variant::STRING, nw_gate_host)
	HOST_SESSION_PROPERTY(Variant::INT, nw_gate_port)
	HOST_SESSION_PROPERTY(Variant::INT, region_index)
	HOST_SESSION_PROPERTY(Variant::STRING, advertise)
#undef HOST_SESSION_PROPERTY
	ClassDB::bind_method(D_METHOD("to_json_value"), &HostSessionOptions::to_json_value);
	ClassDB::bind_static_method("HostSessionOptions", D_METHOD("dialog_controls"), &HostSessionOptions::dialog_controls);
	ClassDB::bind_method(D_METHOD("apply_dialog_control", "control", "value"), &HostSessionOptions::apply_dialog_control);
	ClassDB::bind_method(D_METHOD("dialog_value", "control"), &HostSessionOptions::dialog_value);
	ClassDB::bind_method(D_METHOD("duplicate_options"), &HostSessionOptions::duplicate_options);
	ClassDB::bind_method(D_METHOD("get_player_slot_limit"), &HostSessionOptions::get_player_slot_limit);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "player_slot_limit"), "", "get_player_slot_limit");
}
