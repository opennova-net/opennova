#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <runtime/inmatch/game_config.h>

namespace godot {

// The hosted-session request as the Simulation takes it
// (Simulation.configure_host_session) and reports it back
// (Simulation.get_host_session_config): the user-facing half of the engine's
// inmatch::GameConfig — identity, the g_GameType word, the S2C 0x08 rule globals,
// the send cadence, the projectile options, the advertised spawn — plus the
// binding-side bind port, lobby cap, serve-and-play selector and the install
// root the expansion checksum is CRC'd from. Every field is read-write and
// defaults to the engine's GameConfig default; the sim-owned fields (the
// mission header blob, the score tables, the PCID) are not carried here.
// Field witnesses live on inmatch::GameConfig.
class HostSessionOptions : public RefCounted {
	GDCLASS(HostSessionOptions, RefCounted)

	opennova::inmatch::GameConfig config_;
	int bind_port_ = 64220;
	int max_players_ = 16;
	bool serve_and_play_ = true;
	bool game_type_auto_ = false;
	String game_root_;

protected:
	static void _bind_methods();

public:
	// The config-shaped half (the sim copies the user-facing fields out of it).
	const opennova::inmatch::GameConfig &config() const { return config_; }
	void assign_config(const opennova::inmatch::GameConfig &p_config) { config_ = p_config; }

#define HOST_SESSION_TEXT(m_name)                 \
	String get_##m_name() const;                  \
	void set_##m_name(const String &p_value);
	HOST_SESSION_TEXT(server_name)
	HOST_SESSION_TEXT(mission_name)
	HOST_SESSION_TEXT(mission_file)
	HOST_SESSION_TEXT(custom_text)
	HOST_SESSION_TEXT(player_name)
	HOST_SESSION_TEXT(expansion)
	HOST_SESSION_TEXT(integrity_profile)
	HOST_SESSION_TEXT(spectator_password)
#undef HOST_SESSION_TEXT

#define HOST_SESSION_INT(m_name, m_field)                                              \
	int get_##m_name() const { return static_cast<int>(config_.m_field); }              \
	void set_##m_name(int p_value) {                                                   \
		config_.m_field = static_cast<decltype(config_.m_field)>(p_value);              \
	}
	// Retail's signed spectator setting: 0 disables, -1 shares max_players, a
	// positive value adds that many spectator-only slots.
	HOST_SESSION_INT(spectator_slots, spectator_slots)
	// The g_GameType code word (NetProtocol.GAME_TYPE_*).
	HOST_SESSION_INT(game_type, game_type)
	HOST_SESSION_INT(mp_attributes, mp_attributes)
	HOST_SESSION_INT(class_allow_mask, class_allow_mask)
	HOST_SESSION_INT(respawn_time, respawn_time)
	HOST_SESSION_INT(time_limit_minutes, time_limit_minutes)
	HOST_SESSION_INT(replay_enabled, replay_enabled)
	HOST_SESSION_INT(max_team_lives, max_team_lives)
	HOST_SESSION_INT(score_limit, score_limit)
	HOST_SESSION_INT(max_score, max_score)
	HOST_SESSION_INT(koth_delta, koth_delta)
	HOST_SESSION_INT(flag_return_ticks, flag_return_ticks)
	HOST_SESSION_INT(capture_duration_seconds, capture_duration_seconds)
	HOST_SESSION_INT(capture_speed_setting, capture_speed_setting)
	HOST_SESSION_INT(spawn_wave_time_base, spawn_wave_time_base)
	HOST_SESSION_INT(spawn_wave_time_zone, spawn_wave_time_zone)
	HOST_SESSION_INT(default_spawn_requires_no_team_zone, default_spawn_requires_no_team_zone)
	HOST_SESSION_INT(num_teams, num_teams)
	HOST_SESSION_INT(respawn_timeout, respawn_timeout)
	HOST_SESSION_INT(start_delay, start_delay)
	HOST_SESSION_INT(destroy_buildings, destroy_buildings)
	HOST_SESSION_INT(death_messages, death_messages)
	// The BANDWIDTH server command (100..1600 bytes per 0x0A frame, clamped at apply).
	HOST_SESSION_INT(entity_send_budget, entity_send_budget)
	// Retail g_LanMode 1..4 -> the authority LAN send divider.
	HOST_SESSION_INT(lan_mode, lan_mode)
	HOST_SESSION_INT(spawn_x, spawn_x)
	HOST_SESSION_INT(spawn_y, spawn_y)
	HOST_SESSION_INT(spawn_z, spawn_z)
#undef HOST_SESSION_INT

	// The session family that selects the default send divider: "LAN",
	// "NovaWorld", or "" (resolved from the transport at bring-up).
	String get_channel() const;
	void set_channel(const String &p_channel);
	// Explicit send-holdoff override in ticks; -1 = derive from channel/lan_mode.
	int get_send_holdoff_ticks() const;
	void set_send_holdoff_ticks(int p_ticks);
	bool get_fat_bullets() const { return config_.fat_bullets; }
	void set_fat_bullets(bool p_value) { config_.fat_bullets = p_value; }
	bool get_one_shot_kill() const { return config_.one_shot_kill; }
	void set_one_shot_kill(bool p_value) { config_.one_shot_kill = p_value; }
	PackedStringArray get_spawn_names() const;
	void set_spawn_names(const PackedStringArray &p_names);

	// Binding-side host facts (not GameConfig): the UDP bind port, the lobby
	// player cap (clamped 1..65 at apply), serve-and-play vs a dedicated host,
	// and the install root whose loose expansion/<name>/version.txt the join
	// gate's expansion checksum is CRC'd from (empty = no checksum, D-NET-166).
	int get_bind_port() const { return bind_port_; }
	void set_bind_port(int p_port) { bind_port_ = p_port; }
	int get_max_players() const { return max_players_; }
	void set_max_players(int p_value) { max_players_ = p_value; }
	bool get_serve_and_play() const { return serve_and_play_; }
	void set_serve_and_play(bool p_value) { serve_and_play_ = p_value; }
	// Derive g_GameType from the loaded mission's attrib mode at load
	// (HostSessionConfig.game_type_auto): the mission root resolves it through
	// NetProtocol.game_type_for_mission_mode before configure_host_session,
	// which never reads it.
	bool get_game_type_auto() const { return game_type_auto_; }
	void set_game_type_auto(bool p_value) { game_type_auto_ = p_value; }
	String get_game_root() const { return game_root_; }
	void set_game_root(const String &p_root) { game_root_ = p_root; }

	// The MCP status boundary's JSON shape (one key per field).
	Dictionary to_json_value() const;
};

} // namespace godot
