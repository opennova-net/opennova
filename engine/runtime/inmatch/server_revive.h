#pragma once

#include <runtime/inmatch/napi_np_server_ctx.h>

namespace opennova::world {
class World;
}

namespace opennova::inmatch {

// Drain the kill-zone pass's admitted medic revives through the one retail
// revive transaction, in order: the victim's "being revived" latch, its
// spawn-wave removal, S2C 0x54 [handle][0] to the victim team's Medic set
// (mask 0x580), the healer's scoring event 6 (no Match scorer exists for it
// yet), the victim's revive pose save (position raised 0x4000 plus its
// attitude, consumed by the deploy that follows), S2C 0x3A (empty, mask 160)
// to the victim, its 0x61 seed reroll, and S2C 0x1E event 38
// [victim][healer][0xFF][x][y] (mask 128) to every active player.
// Gates: both entities live, the healer of class 5 (Medic), the victim dead.
// The victim's IDB `occupantEntity` / `pad_174` / `[99].parentSlot` words and
// the healer's two `[0x6B].pad_324` bytes are unmodeled (read as clear).
// [orig: GameEvent_RevivePlayer @0x517CD0 — gates @0x517D28..0x517D44, latch
//  @0x517D4F, wave @0x517D6D, 0x54 @0x517D72..0x517DB4, scoring @0x517DC5,
//  pose @0x517DCD..0x517E09, 0x3A @0x517E29..0x517E3F, seed @0x517E47,
//  0x1E @0x517E63..0x517EB6]
void Server_RouteMedicRevives(NapiNPServerCtx &ctx, world::World &world);

} // namespace opennova::inmatch
