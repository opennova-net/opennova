#pragma once

#include <cstdint>
#include <vector>

#include <net/npwire/ingame_decode.h>   // VehicleSpawnRequest
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_server_ctx.h>

namespace opennova::world {
class World;
}

namespace opennova::inmatch {

// Live pool-1 entities of `type_id` on `team`: has a def, entity+36 bit 0
// clear, bit 0x1000 (the deployable stamp) set. [orig: Server_CountEntitiesByTypeAndTeam @0x510460]
int Server_CountEntitiesByTypeAndTeam(const world::World &world, uint16_t type_id, uint8_t team);

// The S2C 0x70 body for the requester's team: [u8 3] + one
// [u16 typeId][u8 avail][u8 max] per limit row + u16 0. With the host config's
// unlimited_vehicles set, or a row whose type cap and per-team flag are both -1, the pair is
// 0xFF/0xFF; a row with a type cap but no per-team flag is (cap - live, 0xFF);
// otherwise max = the team's slot count and avail = min(cap - live, max), or
// max alone when the cap is -1. [orig: NetPacket_SerializeWeaponOverlaySlots_0 @0x5105A0]
std::vector<uint8_t> Server_BuildVehicleSpawnAvailability(
		const NapiNPServerCtx &ctx, const world::World &world, uint8_t team);

// sub_5104C0: the row exists, the team has slots left, the live count is under
// the type cap (or the cap is -1); `consume` decrements a finite team slot.
bool Server_VehicleSpawnAllowed(NapiNPServerCtx &ctx, const world::World &world,
		uint16_t type_id, uint8_t team, bool consume);

// C2S 0x40: the spawner's admission in retail order — authority, a live
// non-spectator sender, a real source handle, the type index inside the
// source def's pcvehicle_spawnlist mask, a team-neutral or same-team source,
// the limit check (consumed), the spawn position (the source's "boat"/"helo"
// userpoint, else its position raised 2.0: models are not reachable here so
// the raised position stands), the deployable spawn through the embedder's
// spawner seam, the team stamp, the S2C 0x18 fan (mask 0x90) and the
// requester's attach. The pool-1 sibling fan on entity+533 is unmodeled.
// Returns true when a vehicle was spawned.
// [orig: NapiNPServerMsg_HandleVehicleSpawnRequest @0x51C4C0 — gates
//  @0x51C4F2..0x51C5F6, position @0x51C60C..0x51C682, spawn @0x51C69E, team
//  @0x51C6BD, 0x18 @0x51C6CD..0x51C6F1, siblings @0x51C6F6..0x51C76F, attach
//  @0x51C7EF]
bool Server_HandleVehicleSpawnRequest(NapiNPServerCtx &ctx, NapiNPConnection &conn,
		const VehicleSpawnRequest &request, world::World &world);

} // namespace opennova::inmatch
