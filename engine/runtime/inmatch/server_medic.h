#pragma once

#include <runtime/inmatch/napi_np_server_ctx.h>

namespace opennova::world {
class World;
}

namespace opennova::inmatch {

// Drain the kill-zone pass's admitted medic interactions in their sweep order
// through the two transactions GameEvent_HandleMedicInteraction routes to: a
// dead target's revive and a live, hurt target's heal.
//
// The revive, in order: the victim's "being revived" latch, the victim slot's
// revive pose armed and its revive window and respawn penalty closed, its
// spawn-wave removal, S2C 0x54 [handle][0] to the victim team's Medic set
// (mask 0x580), the healer's scoring event 6 (Match::record_revive), the
// victim's revive pose save (position raised 0x4000 plus its attitude,
// consumed by the deploy that follows), the window and the medic request
// cleared, S2C 0x3A (empty, mask 160) to the victim, its 0x61 seed reroll,
// and, unless the healer's slot is hidden (the spectator latch), S2C 0x1E
// event 38 [victim][healer][0xFF][x][y] (mask 128) to every active player.
// Gates: both entities live with a player slot, the healer of class 5
// (Medic), the victim's revive window open (slot+368), the auto-medic
// opt-out (automedic on, or a live medic request), and a last attacker that
// is neither the healer nor the victim.
// [orig: GameEvent_RevivePlayer @0x517CD0 — gates @0x517D07..0x517D44, latch
//  @0x517D4F, slot writes @0x517D5B..0x517D67, wave @0x517D6D, 0x54
//  @0x517D72..0x517DB4, scoring @0x517DC5, pose @0x517DCD..0x517E09, slot
//  clears @0x517E14/@0x517E1A, 0x3A @0x517E29..0x517E3F, seed @0x517E47,
//  hide bytes @0x517E4C..0x517E61, 0x1E @0x517E63..0x517EB6]
//
// The heal: the victim needs a def and a player slot, the healer a player
// slot of class 5 (Medic). The victim's health goes back to its max; then,
// unless the healer's slot is hidden (+97536 tracks the spectator latch,
// +97537 is never set), the healer's scoring event 5 (Match::record_heal) and
// S2C 0x1E event 45 [victim][healer][0xFF][x][y] (mask 128) to every active
// player.
// [orig: GameEvent_HealPlayer @0x50DE30 — the def @0x50DE35, the healer's
//  slot @0x50DE46..0x50DE52 and class @0x50DE58, the victim's slot
//  @0x50DE66..0x50DE70, the restore @0x50DE77..0x50DE7F, the hide bytes
//  @0x50DE86..0x50DE96, scoring @0x50DEA4, 0x1E @0x50DEAC..0x50DF02]
void Server_RouteMedicInteractions(NapiNPServerCtx &ctx, world::World &world);

} // namespace opennova::inmatch
