#pragma once

#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_server_ctx.h>

namespace opennova::world {
class World;
}

namespace opennova::inmatch {

// The Respawn key's request (C2S 0x41, no body; the IDB's name is a misnomer),
// on the authority. For a sender whose player slot has an entity it zeroes the
// entity's Health word and its last attacker (+0x178), stores the configured
// respawn timeout itself, unfloored, into the slot's respawn delay (+360) and
// spawn-target hold (+364), and closes the revive window (+368). Nothing is
// sent. Health 0 is then the player body's death edge on its next update, for
// an unhidden body whose dead bit is clear, which on the authority raises the
// player-death transaction over these holds; ours raises a player body's
// transaction from the death queue (D-AI-9), so a body still alive here queues
// it, with no killer, marked for the edge (RoundDeath::player_edge). The tick
// tests the edge's predicate at its head, ahead of the previous frame's medic
// heals (laying Health 0 back over a heal they made), and routes the record
// after the receive pump's spectator converts unless the body is hidden or dead
// by then, ahead of that frame's fan where retail's edge runs after it
// (D-NET-477). The edge's anim-slot and death-screen
// gates are not mirrored: every player body ours spawns carries its infantry
// body, and the listen host's own never sends this request (D-NET-475).
// [orig: NapiNPServerMsg_DeathTimeoutReset @0x510540 (the C2S table's 0x41 row
//  @0x82B948) -- the authority @0x510542, the connection @0x51054E, its slot
//  @0x510558, the entity @0x510562, +0x11E @0x510568, +0x178 @0x51056F,
//  g_RespawnTimeout into +360 @0x51057B and +364 @0x510587, +368 @0x51058D;
//  the edge Entity_UpdateInfantryPlayerBody @0x4B40E0 (the hidden bit
//  @0x4B411E..0x4B4127, Health <= 0 @0x4B4BF1, the dead bit @0x4B4BFE, the
//  authority's Entity_CheckAndProcessDeath call @0x4B4CEA)]
void Server_HandleDeathTimeoutReset(NapiNPServerCtx &ctx, NapiNPConnection &conn,
		world::World &world);

} // namespace opennova::inmatch
