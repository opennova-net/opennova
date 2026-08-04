#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "npruntime/game_config.h"

namespace opennova::np {

// Apply retail score.ini VERSION 40's VAR table for config.game_type to the
// 39-value S2C 0x58 source. Parsing is transactional: false leaves config
// untouched. [orig: ScoreConfig_LoadFile @0x52D8A0]
bool load_session_score_config(GameConfig &config, std::string_view score_ini);

// Build the exact requester-only S2C 0x58 body. uptime_ms is the wrapping
// GetTickCount delta from session start; active_players is the live roster
// count used by Co-op's key-9 option. The retail serializer writes one extra
// zero key/value sentinel after the advertised option count, which the client
// deliberately does not parse. [orig: Server_BuildStatusReport @0x530A60 ->
// SessionStatus_SerializeToBuffer @0x5310C0]
std::vector<uint8_t> serialize_session_status(
		const GameConfig &config, uint32_t uptime_ms,
		uint32_t active_players);

} // namespace opennova::np
