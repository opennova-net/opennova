#pragma once

#include <cstdint>
#include <vector>

#include <net/npwire/replication_model.h> // GameEntitySnapshot
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_server_ctx.h>

namespace opennova::world {
class World;
}

// One slot's S2C 0x0A, the per-player entity-state write: the server tick's per-slot fan
// (Server_TickUpdate) and the join (Server_OnPlayerJoin) both call it.
// [orig: Server_SendEntityStateToPlayer @0x517BA0]
namespace opennova::inmatch {

// Every 0x0A written for a spectator slot first restamps its body: the hide bit, damage
// disabled and Health 1, whatever a deploy left (the hide bytes are the latch here,
// replication_model.h owner_hidden). Run ahead of the snapshot: the slot's own records
// read the body after it.
// [orig: NetPacket_WritePlayerState @0x4FF6FA..0x4FF728, called from
//  Server_SendEntityStateToPlayer @0x517C76 before its records @0x517C81]
void restamp_spectator_body(world::World &world, const NapiNPConnection &conn);

// Whether the slot's 0x0A walks the pools: the listen host's own player gets the
// header-only frame [orig: Server_SendEntityStateToPlayer @0x517c1b skips
// Server_BuildEntityPriorityList for g_LocalPlayerEntity].
bool entity_state_takes_records(const world::World &world, const NapiNPConnection &conn);

// The settled world snapshot the 0x0A records are built from, a spectator connection's
// own player entity stamped owner-hidden [orig: Server_BuildEntityPriorityList @0x50e6fd].
std::vector<GameEntitySnapshot> snapshot_entity_states(const NapiNPServerCtx &ctx,
		world::World &world);

// Builds `conn`'s 0x0A body from `ents` into `frame_out`; false when the slot has no
// transport or no live owned entity.
bool write_entity_state(NapiNPServerCtx &ctx, world::World &world, NapiNPConnection &conn,
		const std::vector<GameEntitySnapshot> &ents, std::vector<uint8_t> &frame_out);

// The one-slot writer the join calls: the spectator restamp, the snapshot when the slot
// takes records, and the 0x0A body into `frame_out`.
// [orig: Server_SendEntityStateToPlayer @0x517BA0]
bool Server_SendEntityStateToPlayer(NapiNPServerCtx &ctx, world::World &world,
		NapiNPConnection &conn, std::vector<uint8_t> &frame_out);

} // namespace opennova::inmatch
