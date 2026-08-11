#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>

#include <npruntime/game_config.h>
#include <npwire/game_type.h>
#include <npwire/net_ports.h>
#include <npwire/wire_handle.h>

namespace godot {

// The net-protocol constant surface: a statics-only re-export of the engine's
// witnessed session vocabulary so no GDScript ever restates a wire constant.
// Every enum value ALIASES its engine constant (no literal mirrors — the
// promoted-literal lint keeps the values in their canonical homes), and the
// .cpp static_asserts pin each bound name besides. The witnesses live at the
// engine homes, never here:
//   engine/net/npwire/game_type.h      — g_GameType code words, the objective
//                                        bit, the mission-attrib map, the
//                                        Config_SetDefaults rule baseline,
//                                        custom-text default, callsign cap
//   engine/net/npwire/net_ports.h      — the retail LAN port range + gate port
//   engine/net/npwire/wire_handle.h    — the pool<<12|slot handle layout
//   engine/net/npwire/session_hello.h  — the ClientAuth character_id bit-pack
//   engine/net/npruntime/game_config.h — the 1..65 lobby player cap
class NetProtocol : public RefCounted {
	GDCLASS(NetProtocol, RefCounted)

protected:
	static void _bind_methods();

public:
	enum {
		// The witnessed g_GameType code words (npwire game_type.h; opaque
		// beyond the objective bit — never decompose them).
		GAME_TYPE_DEATHMATCH = opennova::game_type::kDeathmatch,
		GAME_TYPE_KING_OF_THE_HILL = opennova::game_type::kKingOfTheHill,
		GAME_TYPE_TEAM_DEATHMATCH = opennova::game_type::kTeamDeathmatch,
		GAME_TYPE_TEAM_KING_OF_THE_HILL = opennova::game_type::kTeamKingOfTheHill,
		GAME_TYPE_ATTACK_AND_DEFEND = opennova::game_type::kAttackDefend,
		GAME_TYPE_CAPTURE_THE_FLAG = opennova::game_type::kCaptureTheFlag,
		GAME_TYPE_FLAGBALL = opennova::game_type::kFlagBall,
		GAME_TYPE_ADVANCE_AND_SECURE = opennova::game_type::kAdvanceAndSecure,
		GAME_TYPE_SEARCH_AND_DESTROY = opennova::game_type::kSearchAndDestroy,
		GAME_TYPE_CONQUER_AND_CONTROL = opennova::game_type::kConquerAndControl,
		// Stock/training Co-op vs the shipped objective Co-op:
		// GAME_TYPE_COOP == GAME_TYPE_TRAINING_COOP | GAME_TYPE_OBJECTIVE_BIT.
		GAME_TYPE_TRAINING_COOP = opennova::game_type::kCoop,
		GAME_TYPE_COOP = opennova::game_type::kObjectiveCoop,
		GAME_TYPE_OBJECTIVE_BIT = opennova::game_type::kObjectiveBit,

		// The retail LAN host port range + the NovaWorld gate UDP port
		// (npwire net_ports.h). DEFAULT_LAN_PORT is the range's first port —
		// the default a host binds.
		DEFAULT_LAN_PORT = opennova::kRetailLanPortMin,
		RETAIL_LAN_PORT_MIN = opennova::kRetailLanPortMin,
		RETAIL_LAN_PORT_MAX = opennova::kRetailLanPortMax,
		DEFAULT_GATE_PORT = opennova::kNovaWorldGatePort,

		// The lobby player-cap ceiling (npruntime game_config.h) and the wire
		// callsign cap (Name[16] cstring; npwire game_type.h).
		MAX_PLAYERS_CAP = opennova::np::kMaxPlayersCap,
		MAX_CALLSIGN_LENGTH = opennova::game_rules::kMaxCallsignLength,

		// The Config_SetDefaults session-rule baseline (npwire game_type.h).
		DEFAULT_RESPAWN_TIME = opennova::game_rules::kDefaultRespawnTime,
		DEFAULT_TIME_LIMIT_MINUTES = opennova::game_rules::kDefaultTimeLimitMinutes,
		DEFAULT_REPLAY_ENABLED = opennova::game_rules::kDefaultReplayEnabled,
		DEFAULT_MAX_TEAM_LIVES = opennova::game_rules::kDefaultMaxTeamLives,
		DEFAULT_SCORE_LIMIT = opennova::game_rules::kDefaultScoreLimit,
		DEFAULT_RESPAWN_TIMEOUT = opennova::game_rules::kDefaultRespawnTimeout,
		DEFAULT_START_DELAY = opennova::game_rules::kDefaultStartDelay,
		DEFAULT_DESTROY_BUILDINGS = opennova::game_rules::kDefaultDestroyBuildings,
		DEFAULT_DEATH_MESSAGES = opennova::game_rules::kDefaultDeathMessages,

		// The wire entity-handle bit layout: handle = pool << 12 | slot
		// (npwire wire_handle.h).
		WIRE_HANDLE_INVALID = opennova::wire_handle::kInvalid,
		WIRE_HANDLE_POOL_SHIFT = opennova::wire_handle::kPoolShift,
		WIRE_HANDLE_POOL_MASK = opennova::wire_handle::kPoolMask,
		WIRE_HANDLE_SLOT_MASK = opennova::wire_handle::kSlotMask,
		WIRE_HANDLE_POOL_COUNT = opennova::wire_handle::kPoolCount,
	};

	// Retail's mission-attrib -> g_GameType selection: the mission header's
	// single-select game-mode bit picks the session code word; 0 / unknown
	// resolves to GAME_TYPE_TRAINING_COOP (npwire game_type::for_mission_mode).
	static int game_type_for_mission_mode(int p_attrib_mode);
	// The MULTI_PLAYER_HOST dialog's witnessed game-type rules (D-MNU-17);
	// the logic lives in engine/net/npwire game_type.h.
	static bool game_type_host_list_visible(int p_game_type);
	static int game_type_host_filter_category(int p_game_type);
	static String game_type_host_abbreviation_key(int p_game_type);
	static bool game_type_host_rotation_default(int p_game_type);

	// The retail host's custom-message default (npwire game_rules::kCustomTextDefault).
	static String custom_text_default();

	// Wire entity-handle decode (npwire wire_handle.h).
	static int wire_handle_pool(int p_handle);
	static int wire_handle_slot(int p_handle);
	// The pool taxonomy label ("organics"/"items"/"buildings"/"markers"/
	// "effects"; "?" out of range) by POOL INDEX, not handle.
	static String wire_handle_pool_label(int p_pool_index);

	// The ClientAuth character_id bit-pack [alignment:1|combo:6|div:4|nat:5]
	// (npwire session_hello.h character_id). alignment: 0 good / nonzero evil.
	static int pack_character_id(int p_nationality, int p_division, int p_combo,
			int p_alignment);
	static int character_id_nationality(int p_packed);
	static int character_id_division(int p_packed);
	static int character_id_combo(int p_packed);
	static int character_id_alignment(int p_packed);
};

} // namespace godot
