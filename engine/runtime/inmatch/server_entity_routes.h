#pragma once

// The host tick's routes for the records the world produced: the item and
// removal events with the HUD relays, the water-surface crossings and the
// guided-round updates. Server_TickUpdate orders them (server_tick.cpp).

#include <runtime/inmatch/napi_np_server_ctx.h>

namespace opennova::world {
class World;
}

namespace opennova::inmatch {

// Item callbacks' state packets, explosion effects, authoritative removals
// and the HUD relays, fanned to the active remote slots, then released.
void Server_FanEntityEvents(NapiNPServerCtx &ctx, world::World &world);
// The water-surface crossings the motor recorded, one S2C 0x34 each to every
// alive in-match remote player, then released.
void Server_RouteWaterCrossings(NapiNPServerCtx &ctx, world::World &world);
// The guided-round updates, queued behind the tick's 0x0A so each follows the
// frame that carried its round's birth.
void Server_RouteGuidance(NapiNPServerCtx &ctx, world::World &world);

} // namespace opennova::inmatch
