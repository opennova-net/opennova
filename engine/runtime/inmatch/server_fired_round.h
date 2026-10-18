#pragma once

#include <cstddef>
#include <cstdint>

#include <runtime/inmatch/game_config.h>
#include <runtime/inmatch/napi_np_connection.h>

namespace opennova::world {
class World;
}

namespace opennova::inmatch {

// The host side of a client's fired round (C2S 0x06): the cease-fire, spectator,
// shooter and freshness gates, the first pass's owner checks and can-fire over
// the slot that fires (the personal slot of the fired combo, or the carrier's on
// a UseGun seat or an EWeap controller seat), the spend through the shooter's
// seat, the EquippedSlot and target stamps, the fire action's second pass (the
// owner checks and the claimed origin's reach on the fired weapon), the score,
// the spawn-protection clear, the ring append and the authoritative round, and
// the fire-tick floor. No reply: the echo rides the per-frame 0x0A fan.
// [orig: NapiNPServerMsg_0x006_ClientFiredRound @0x513310 ->
//  Server_ClientFiredRound @0x50BAA0]
void Server_HandleClientFiredRound(const GameConfig &config, NapiNPConnection &conn,
		const uint8_t *payload, size_t size, world::World *world);

} // namespace opennova::inmatch
