#pragma once

#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/world/world.h>

namespace opennova::inmatch {

// The host's per-recipient minimap overlay state (S2C 0x40 batches of up to
// 16 classified entities, opened by the recipient team's S2C 0x6B
// designations), one visit per client every 14 host ticks with a 0..127
// pool-1 phase walk; split off server_tick.cpp.
// [orig: Server_BuildOverlayStateForPlayer @0x517FC0]
void emit_minimap_overlay_state(NapiNPServerCtx &ctx, world::World &world);

} // namespace opennova::inmatch
