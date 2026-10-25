#pragma once

#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/world/world.h>

namespace opennova::inmatch {

// One slot's leg of the tick-head per-player walk: the host's per-recipient
// minimap overlay state (S2C 0x40 batches of up to 16 classified entities,
// opened by the recipient team's S2C 0x6B designations and closed, in
// FlagBall and Flag Me, by the long-carry carrier pass), one visit per client
// every 14 host ticks with a 0..127 pool-1 phase walk; split off
// server_tick.cpp, whose walk calls it per slot. `in_match` is the walk's state-6 test on
// the slot (a connection's own row, or an orphaned row whose transport is its net player's,
// null past the leave: the visit's words still step, nothing is sent; D-NET-NEW-L1-24).
// [orig: Server_UpdateAllActivePlayerSlots @0x518820, the visit @0x5188D6;
//  Server_BuildOverlayStateForPlayer @0x517FC0]
void emit_minimap_overlay_for_slot(NapiNPServerCtx &ctx, world::World &world,
		NapiNPConnection &conn, bool in_match);

} // namespace opennova::inmatch
