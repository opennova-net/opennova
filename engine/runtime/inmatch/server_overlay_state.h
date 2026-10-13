#pragma once

#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/world/world.h>

namespace opennova::inmatch {

// One slot's leg of the tick-head per-player walk: the host's per-recipient
// minimap overlay state (S2C 0x40 batches of up to 16 classified entities,
// opened by the recipient team's S2C 0x6B designations and closed, in
// FlagBall and Flag Me, by the long-carry carrier pass), one visit per client
// every 14 host ticks with a 0..127 pool-1 phase walk; split off
// server_tick.cpp, whose walk calls it per slot.
// [orig: Server_UpdateAllActivePlayerSlots @0x518820, the visit @0x5188D6;
//  Server_BuildOverlayStateForPlayer @0x517FC0]
void emit_minimap_overlay_for_slot(NapiNPServerCtx &ctx, world::World &world,
		NapiNPConnection &conn);

} // namespace opennova::inmatch
