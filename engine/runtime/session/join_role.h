#pragma once

#include <cstdint>

namespace opennova::np {

// [orig: ClientAuth CU JSR consumed by Server_ValidatePlayerJoinRequest
// @0x512100; decimal 1 selects the spectator admission path]
enum class JoinRole : uint8_t {
	Player = 0,
	Spectator = 1,
};

} // namespace opennova::np
