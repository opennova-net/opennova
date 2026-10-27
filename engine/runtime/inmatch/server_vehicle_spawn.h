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

// One EntityLimit row: the LAST row naming `type_id` is updated in place, else a new row is
// appended while the table holds fewer than 128; the row takes the type cap, the per-team flag
// `initial` and all 70 per-team slots set to `initial`. A full table takes nothing.
// [orig: EntityLimit_SetEntry @0x500E50 - the walk keeping the last match @0x500E72..0x500E7E,
//  the 128 cap @0x500E8B, the append @0x500E95, row[1] @0x500EA8, row[0] @0x500EAF,
//  row[2] @0x500EB1, memset32(row + 3, initial, 0x46) @0x500EBC]
void Server_SetEntityLimitEntry(NapiNPServerCtx &ctx, uint16_t type_id, int32_t type_cap,
		int32_t initial);

// The mission start's EntityLimit table: cleared, then the 23 stock rows (type id, type cap,
// per-team flag), in retail's order. Every mission start runs it, so a map change or a restart
// also restores the consumed per-team slots.
// [orig: Game_StartMission @0x526182 -> EntityLimit_InitTable @0x509A70 - memset(g_EntityLimitTable,
//  0, 0x9204) (the table and g_EntityLimitCount @0xC84680), then 23 EntityLimit_SetEntry calls]
void Server_InitEntityLimitTable(NapiNPServerCtx &ctx);

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

// EntityLimit_TryConsume: the row exists, the team has slots left, the live count is under
// the type cap (or the cap is -1); `consume` decrements a finite team slot.
bool Server_VehicleSpawnAllowed(NapiNPServerCtx &ctx, const world::World &world,
		uint16_t type_id, uint8_t team, bool consume);

// C2S 0x40: the spawner's admission in retail order — authority, a live
// non-spectator sender, a real source handle, the type index inside the
// source def's pcvehicle_spawnlist mask, a team-neutral or same-team source,
// the limit check (consumed), the spawn block (the source's attitude words and its
// "boat", else "helo", userpoint through its euler placement, Entity::deploy_point,
// else its position raised 2.0), the deployable spawn through the embedder's spawner
// seam, the team stamp, the S2C 0x18 fan (mask 0x90) for the vehicle and then each
// other pool-1 row of its reference group (entity+533), and the requester's attach at
// the vehicle def's control bone. Returns true when a vehicle was spawned.
// [orig: NapiNPServerMsg_HandleVehicleSpawnRequest @0x51C4C0 — gates
//  @0x51C4F2..0x51C5F6, position @0x51C60C..0x51C682, spawn @0x51C69E, team
//  @0x51C6BD, 0x18 @0x51C6CD..0x51C6F1, siblings @0x51C6F6..0x51C76F, attach
//  @0x51C7EF]
bool Server_HandleVehicleSpawnRequest(NapiNPServerCtx &ctx, NapiNPConnection &conn,
		const VehicleSpawnRequest &request, world::World &world);

} // namespace opennova::inmatch
