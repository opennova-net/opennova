#include "network/net_protocol.h"

#include <net/npruntime/game_config.h>
#include <net/npwire/game_type.h>
#include <net/npwire/net_ports.h>
#include <net/npwire/session_hello.h>
#include <net/npwire/wire_handle.h>

namespace godot {

// Pin every bound name to its engine home so the surface can never drift —
// the header aliases the engine constants, and these asserts keep any future
// literal rewrite honest. The witnesses live at the engine headers.
static_assert(NetProtocol::GAME_TYPE_DEATHMATCH == opennova::game_type::kDeathmatch,
		"GAME_TYPE_DEATHMATCH drifted from npwire game_type.h");
static_assert(NetProtocol::GAME_TYPE_KING_OF_THE_HILL == opennova::game_type::kKingOfTheHill,
		"GAME_TYPE_KING_OF_THE_HILL drifted from npwire game_type.h");
static_assert(NetProtocol::GAME_TYPE_FLAG_ME == opennova::game_type::kFlagMe,
		"GAME_TYPE_FLAG_ME drifted from npwire game_type.h");
static_assert(NetProtocol::GAME_TYPE_TEAM_DEATHMATCH == opennova::game_type::kTeamDeathmatch,
		"GAME_TYPE_TEAM_DEATHMATCH drifted from npwire game_type.h");
static_assert(NetProtocol::GAME_TYPE_TEAM_KING_OF_THE_HILL == opennova::game_type::kTeamKingOfTheHill,
		"GAME_TYPE_TEAM_KING_OF_THE_HILL drifted from npwire game_type.h");
static_assert(NetProtocol::GAME_TYPE_ATTACK_AND_DEFEND == opennova::game_type::kAttackDefend,
		"GAME_TYPE_ATTACK_AND_DEFEND drifted from npwire game_type.h");
static_assert(NetProtocol::GAME_TYPE_CAPTURE_THE_FLAG == opennova::game_type::kCaptureTheFlag,
		"GAME_TYPE_CAPTURE_THE_FLAG drifted from npwire game_type.h");
static_assert(NetProtocol::GAME_TYPE_FLAGBALL == opennova::game_type::kFlagBall,
		"GAME_TYPE_FLAGBALL drifted from npwire game_type.h");
static_assert(NetProtocol::GAME_TYPE_ADVANCE_AND_SECURE == opennova::game_type::kAdvanceAndSecure,
		"GAME_TYPE_ADVANCE_AND_SECURE drifted from npwire game_type.h");
static_assert(NetProtocol::GAME_TYPE_SEARCH_AND_DESTROY == opennova::game_type::kSearchAndDestroy,
		"GAME_TYPE_SEARCH_AND_DESTROY drifted from npwire game_type.h");
static_assert(NetProtocol::GAME_TYPE_CONQUER_AND_CONTROL == opennova::game_type::kConquerAndControl,
		"GAME_TYPE_CONQUER_AND_CONTROL drifted from npwire game_type.h");
static_assert(NetProtocol::GAME_TYPE_TRAINING_COOP == opennova::game_type::kCoop,
		"GAME_TYPE_TRAINING_COOP drifted from npwire game_type.h");
static_assert(NetProtocol::GAME_TYPE_COOP == opennova::game_type::kObjectiveCoop,
		"GAME_TYPE_COOP drifted from npwire game_type.h");
static_assert(NetProtocol::GAME_TYPE_OBJECTIVE_BIT == opennova::game_type::kObjectiveBit,
		"GAME_TYPE_OBJECTIVE_BIT drifted from npwire game_type.h");

static_assert(NetProtocol::DEFAULT_LAN_PORT == opennova::kRetailLanPortMin,
		"DEFAULT_LAN_PORT drifted from npwire net_ports.h");
static_assert(NetProtocol::RETAIL_LAN_PORT_MIN == opennova::kRetailLanPortMin,
		"RETAIL_LAN_PORT_MIN drifted from npwire net_ports.h");
static_assert(NetProtocol::RETAIL_LAN_PORT_MAX == opennova::kRetailLanPortMax,
		"RETAIL_LAN_PORT_MAX drifted from npwire net_ports.h");
static_assert(NetProtocol::DEFAULT_GATE_PORT == opennova::kNovaWorldGatePort,
		"DEFAULT_GATE_PORT drifted from npwire net_ports.h");

static_assert(NetProtocol::MAX_PLAYERS_CAP == opennova::np::kMaxPlayersCap,
		"MAX_PLAYERS_CAP drifted from npruntime game_config.h");
static_assert(NetProtocol::MAX_CALLSIGN_LENGTH == opennova::game_rules::kMaxCallsignLength,
		"MAX_CALLSIGN_LENGTH drifted from npwire game_type.h");

static_assert(NetProtocol::DEFAULT_RESPAWN_TIME == opennova::game_rules::kDefaultRespawnTime,
		"DEFAULT_RESPAWN_TIME drifted from npwire game_type.h");
static_assert(NetProtocol::DEFAULT_TIME_LIMIT_MINUTES == opennova::game_rules::kDefaultTimeLimitMinutes,
		"DEFAULT_TIME_LIMIT_MINUTES drifted from npwire game_type.h");
static_assert(NetProtocol::DEFAULT_REPLAY_ENABLED == opennova::game_rules::kDefaultReplayEnabled,
		"DEFAULT_REPLAY_ENABLED drifted from npwire game_type.h");
static_assert(NetProtocol::DEFAULT_MAX_TEAM_LIVES == opennova::game_rules::kDefaultMaxTeamLives,
		"DEFAULT_MAX_TEAM_LIVES drifted from npwire game_type.h");
static_assert(NetProtocol::DEFAULT_SCORE_LIMIT == opennova::game_rules::kDefaultScoreLimit,
		"DEFAULT_SCORE_LIMIT drifted from npwire game_type.h");
static_assert(NetProtocol::DEFAULT_MAX_SCORE == opennova::game_rules::kDefaultMaxScore,
		"DEFAULT_MAX_SCORE drifted from npwire game_type.h");
static_assert(NetProtocol::DEFAULT_KOTH_DELTA == opennova::game_rules::kDefaultKothDelta,
		"DEFAULT_KOTH_DELTA drifted from npwire game_type.h");
static_assert(NetProtocol::DEFAULT_FLAG_RETURN_TICKS == opennova::game_rules::kDefaultFlagReturnTicks,
		"DEFAULT_FLAG_RETURN_TICKS drifted from npwire game_type.h");
static_assert(NetProtocol::DEFAULT_CAPTURE_DURATION_SECONDS ==
		opennova::game_rules::kDefaultCaptureDurationSeconds,
		"DEFAULT_CAPTURE_DURATION_SECONDS drifted from npwire game_type.h");
static_assert(NetProtocol::DEFAULT_CAPTURE_SPEED_SETTING ==
		opennova::game_rules::kDefaultCaptureSpeedSetting,
		"DEFAULT_CAPTURE_SPEED_SETTING drifted from npwire game_type.h");
static_assert(NetProtocol::DEFAULT_SPAWN_WAVE_TIME_BASE ==
		opennova::game_rules::kDefaultSpawnWaveTimeBase,
		"DEFAULT_SPAWN_WAVE_TIME_BASE drifted from npwire game_type.h");
static_assert(NetProtocol::DEFAULT_SPAWN_WAVE_TIME_ZONE ==
		opennova::game_rules::kDefaultSpawnWaveTimeZone,
		"DEFAULT_SPAWN_WAVE_TIME_ZONE drifted from npwire game_type.h");
static_assert(NetProtocol::DEFAULT_SPAWN_REQUIRES_NO_TEAM_ZONE ==
		opennova::game_rules::kDefaultSpawnRequiresNoTeamZone,
		"DEFAULT_SPAWN_REQUIRES_NO_TEAM_ZONE drifted from npwire game_type.h");
static_assert(NetProtocol::DEFAULT_NUM_TEAMS == opennova::game_rules::kDefaultNumTeams,
		"DEFAULT_NUM_TEAMS drifted from npwire game_type.h");
static_assert(NetProtocol::DEFAULT_RESPAWN_TIMEOUT == opennova::game_rules::kDefaultRespawnTimeout,
		"DEFAULT_RESPAWN_TIMEOUT drifted from npwire game_type.h");
static_assert(NetProtocol::DEFAULT_START_DELAY == opennova::game_rules::kDefaultStartDelay,
		"DEFAULT_START_DELAY drifted from npwire game_type.h");
static_assert(NetProtocol::DEFAULT_DESTROY_BUILDINGS == opennova::game_rules::kDefaultDestroyBuildings,
		"DEFAULT_DESTROY_BUILDINGS drifted from npwire game_type.h");
static_assert(NetProtocol::DEFAULT_DEATH_MESSAGES == opennova::game_rules::kDefaultDeathMessages,
		"DEFAULT_DEATH_MESSAGES drifted from npwire game_type.h");

static_assert(NetProtocol::WIRE_HANDLE_INVALID == opennova::wire_handle::kInvalid,
		"WIRE_HANDLE_INVALID drifted from npwire wire_handle.h");
static_assert(NetProtocol::WIRE_HANDLE_POOL_SHIFT == opennova::wire_handle::kPoolShift,
		"WIRE_HANDLE_POOL_SHIFT drifted from npwire wire_handle.h");
static_assert(NetProtocol::WIRE_HANDLE_POOL_MASK == opennova::wire_handle::kPoolMask,
		"WIRE_HANDLE_POOL_MASK drifted from npwire wire_handle.h");
static_assert(NetProtocol::WIRE_HANDLE_SLOT_MASK == opennova::wire_handle::kSlotMask,
		"WIRE_HANDLE_SLOT_MASK drifted from npwire wire_handle.h");
static_assert(NetProtocol::WIRE_HANDLE_POOL_COUNT == opennova::wire_handle::kPoolCount,
		"WIRE_HANDLE_POOL_COUNT drifted from npwire wire_handle.h");

void NetProtocol::_bind_methods() {
	ClassDB::bind_static_method("NetProtocol",
			D_METHOD("game_type_for_mission_mode", "attrib_mode"),
			&NetProtocol::game_type_for_mission_mode);
	ClassDB::bind_static_method("NetProtocol",
			D_METHOD("game_type_host_list_visible", "game_type"),
			&NetProtocol::game_type_host_list_visible);
	ClassDB::bind_static_method("NetProtocol",
			D_METHOD("game_type_host_filter_category", "game_type"),
			&NetProtocol::game_type_host_filter_category);
	ClassDB::bind_static_method("NetProtocol",
			D_METHOD("game_type_host_abbreviation_key", "game_type"),
			&NetProtocol::game_type_host_abbreviation_key);
	ClassDB::bind_static_method("NetProtocol",
			D_METHOD("game_type_overlay_label_key", "game_type"),
			&NetProtocol::game_type_overlay_label_key);
	ClassDB::bind_static_method("NetProtocol",
			D_METHOD("game_type_host_rotation_default", "game_type"),
			&NetProtocol::game_type_host_rotation_default);
	ClassDB::bind_static_method("NetProtocol", D_METHOD("custom_text_default"),
			&NetProtocol::custom_text_default);
	ClassDB::bind_static_method("NetProtocol", D_METHOD("wire_handle_pool", "handle"),
			&NetProtocol::wire_handle_pool);
	ClassDB::bind_static_method("NetProtocol", D_METHOD("wire_handle_slot", "handle"),
			&NetProtocol::wire_handle_slot);
	ClassDB::bind_static_method("NetProtocol",
			D_METHOD("pack_character_id", "nationality", "division", "combo", "alignment"),
			&NetProtocol::pack_character_id);

	BIND_CONSTANT(GAME_TYPE_DEATHMATCH);
	BIND_CONSTANT(GAME_TYPE_KING_OF_THE_HILL);
	BIND_CONSTANT(GAME_TYPE_FLAG_ME);
	BIND_CONSTANT(GAME_TYPE_TEAM_DEATHMATCH);
	BIND_CONSTANT(GAME_TYPE_TEAM_KING_OF_THE_HILL);
	BIND_CONSTANT(GAME_TYPE_ATTACK_AND_DEFEND);
	BIND_CONSTANT(GAME_TYPE_CAPTURE_THE_FLAG);
	BIND_CONSTANT(GAME_TYPE_FLAGBALL);
	BIND_CONSTANT(GAME_TYPE_ADVANCE_AND_SECURE);
	BIND_CONSTANT(GAME_TYPE_SEARCH_AND_DESTROY);
	BIND_CONSTANT(GAME_TYPE_CONQUER_AND_CONTROL);
	BIND_CONSTANT(GAME_TYPE_TRAINING_COOP);
	BIND_CONSTANT(GAME_TYPE_COOP);
	BIND_CONSTANT(GAME_TYPE_OBJECTIVE_BIT);
	BIND_CONSTANT(DEFAULT_LAN_PORT);
	BIND_CONSTANT(DEFAULT_GATE_PORT);
	BIND_CONSTANT(MAX_CALLSIGN_LENGTH);
	BIND_CONSTANT(DEFAULT_RESPAWN_TIME);
	BIND_CONSTANT(DEFAULT_TIME_LIMIT_MINUTES);
	BIND_CONSTANT(DEFAULT_REPLAY_ENABLED);
	BIND_CONSTANT(DEFAULT_MAX_TEAM_LIVES);
	BIND_CONSTANT(DEFAULT_SCORE_LIMIT);
	BIND_CONSTANT(DEFAULT_MAX_SCORE);
	BIND_CONSTANT(DEFAULT_KOTH_DELTA);
	BIND_CONSTANT(DEFAULT_FLAG_RETURN_TICKS);
	BIND_CONSTANT(DEFAULT_CAPTURE_DURATION_SECONDS);
	BIND_CONSTANT(DEFAULT_CAPTURE_SPEED_SETTING);
	BIND_CONSTANT(DEFAULT_SPAWN_WAVE_TIME_BASE);
	BIND_CONSTANT(DEFAULT_SPAWN_WAVE_TIME_ZONE);
	BIND_CONSTANT(DEFAULT_SPAWN_REQUIRES_NO_TEAM_ZONE);
	BIND_CONSTANT(DEFAULT_NUM_TEAMS);
	BIND_CONSTANT(DEFAULT_RESPAWN_TIMEOUT);
	BIND_CONSTANT(DEFAULT_START_DELAY);
	BIND_CONSTANT(DEFAULT_DESTROY_BUILDINGS);
	BIND_CONSTANT(DEFAULT_DEATH_MESSAGES);
	BIND_CONSTANT(WIRE_HANDLE_INVALID);
	BIND_CONSTANT(WIRE_HANDLE_POOL_SHIFT);
	BIND_CONSTANT(WIRE_HANDLE_POOL_MASK);
	BIND_CONSTANT(WIRE_HANDLE_SLOT_MASK);
	BIND_CONSTANT(WIRE_HANDLE_POOL_COUNT);
}

int NetProtocol::game_type_for_mission_mode(int p_attrib_mode) {
	return static_cast<int>(opennova::game_type::for_mission_mode(
			static_cast<uint32_t>(p_attrib_mode)));
}

bool NetProtocol::game_type_host_list_visible(int p_game_type) {
	return opennova::game_type::host_list_visible(
			static_cast<uint32_t>(p_game_type));
}

int NetProtocol::game_type_host_filter_category(int p_game_type) {
	return opennova::game_type::host_filter_category(
			static_cast<uint32_t>(p_game_type));
}

String NetProtocol::game_type_host_abbreviation_key(int p_game_type) {
	return String(opennova::game_type::host_abbreviation_key(
			static_cast<uint32_t>(p_game_type)));
}

bool NetProtocol::game_type_host_rotation_default(int p_game_type) {
	return opennova::game_type::host_rotation_default(
			static_cast<uint32_t>(p_game_type));
}

String NetProtocol::game_type_overlay_label_key(int p_game_type) {
	return String(opennova::game_type::overlay_label_key(
			static_cast<uint32_t>(p_game_type)));
}

String NetProtocol::custom_text_default() {
	return String(opennova::game_rules::kCustomTextDefault);
}

int NetProtocol::wire_handle_pool(int p_handle) {
	return opennova::wire_handle::pool(static_cast<uint16_t>(p_handle));
}

int NetProtocol::wire_handle_slot(int p_handle) {
	return opennova::wire_handle::slot(static_cast<uint16_t>(p_handle));
}

int NetProtocol::pack_character_id(int p_nationality, int p_division, int p_combo,
		int p_alignment) {
	return opennova::character_id::pack(p_nationality, p_division, p_combo, p_alignment);
}

} // namespace godot
