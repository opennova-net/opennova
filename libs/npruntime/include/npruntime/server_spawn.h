#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include <world/player_spawn.h> // world::kRetailPlayerMinEntitySlot (canonical)

#include "npruntime/napi_np_connection.h"
#include "npruntime/napi_np_server_ctx.h"

// P3 — the World-driven host-side player spawn (§5.2a steps 1-2 / §5.2b). Replaces the
// fixture/game_runtime spawn-gate: the listen-server host runs its own server-side spawn machinery
// in-process and registers the pool-0 player entity in `World` (ADR 0012), writing entity+0x78
// ownerConnectionId. Free functions over NapiNPServerCtx + world::World, mirroring the original flow
// 1:1. Godot-free / socket-free (libs/CLAUDE.md). The §5.2b field-init itself is already ported in
// libs/world (world::spawn_player / spawn_remote_player); these functions are the orchestration above
// it. [orig: Server_InitNewRoundState @0x51c8e0 -> CNapiServer_ProcessPendingPlayerSpawns @0x4c8dc0
// -> Server_BuildPlayerInfoAndAdd @0x51d560 -> Server_PlayerAdd @0x51cbc0; net-re §5.2a/§5.2b]
namespace opennova::world {
class World;
}
namespace opennova::netsim {
class ISessionTransport;
}

namespace opennova::np {

// Re-export the canonical world::kRetailPlayerMinEntitySlot into np for the spawn call sites (one
// definition, shared with the Godot listen host). [orig: §5.2b spawn placement]
inline constexpr uint16_t kRetailPlayerMinEntitySlot = world::kRetailPlayerMinEntitySlot;

// Reserve the team written to both the pre-spawn S2C 0x04 slot assignment and
// the later player entity. Repeated calls for one connection are idempotent.
// Pending MP reservations participate in autobalance, so admissions received
// in one socket drain cannot all observe stale World counts.
// [orig: Server_AssignPlayerTeam @0x4fe310 writes playerSlot+416 before
// NetPacket_WriteSlotAssignment @0x502b30 reads it]
uint8_t Server_ReservePlayerTeam(const GameConfig &config, bool is_in_session,
		const std::vector<NapiNPConnection> &roster, NapiNPConnection &conn,
		const world::World &world);

// Reserve the first free fixed roster-table row before S2C 0x04 advertises it.
// Existing player bindings and other pre-spawn reservations both occupy rows;
// repeated calls for one connection return the same row. Player-add consumes
// the reservation, while erasing the connection releases it. No row at or
// above the advertised slot capacity can be reserved.
// [orig: Server_PlayerAdd's dword_A87048 row is already attached to the
// playerSlot record consumed by NetPacket_WriteSlotAssignment @0x502b30]
std::optional<uint8_t> Server_ReservePlayerSlot(
		const std::vector<NapiNPConnection> &roster, NapiNPConnection &conn,
		uint32_t slot_capacity);

// §5.2a step 1 — [orig: Server_InitNewRoundState @0x51c8e0]. Set up the local-player/round context
// for an authority host. Structural: clears the loading-progress counter for a fresh round (the
// timeout gate, NOT the spawn-success gate, which the per-frame 0x0A drops, §5.2a step 4). No-op
// unless ctx.is_authority.
void Server_InitNewRoundState(NapiNPServerCtx &ctx);

// §5.2a step 2 — [orig: CNapiServer_ProcessPendingPlayerSpawns @0x4c8dc0]. Gated
// is_authority && !spawn_success_gate. Walks connection_list; for each accepted-but-unspawned node
// (self_id_seen && phase < PlayerAdded) builds + registers its pool-0 player entity in `world`. The
// host's own type-2 loopback is just another entry in this list. Returns the number spawned this pass.
int Server_ProcessPendingPlayerSpawns(NapiNPServerCtx &ctx, world::World &world);

// §5.2a step 2 (per player) — [orig: Server_BuildPlayerInfoAndAdd @0x51d560 -> Server_PlayerAdd
// @0x51cbc0]. Build the spawn pose from the mission's promoted start markers (§5.2c,
// world::select_player_spawn — never an NPC's spot) and register the pool-0 0x14B9 entity, stamping
// entity+0x78 = conn.connection_id. The type-2 loopback (the host's own client) -> world::spawn_player
// (publishes World::cached.local_player); a type-1 remote joiner -> world::spawn_remote_player (the
// host never republishes its local player). Binds the handle onto conn.link.owned_entity and advances
// conn.phase = PlayerAdded. Returns the spawned handle (invalid if pool 0 is full or World has no
// AiSystem).
world::EntityHandle Server_BuildPlayerInfoAndAdd(NapiNPServerCtx &ctx, NapiNPConnection &conn,
                                                 world::World &world);

// Synthetic in-process peer admit WITHOUT a handshake — an owner/test hook (the Godot binding's
// admit_test_remote_peer). Spawns a pool-0 REMOTE player at `spawn` (net_id forced to 0 — a player
// carries no SSN, D-NET-112; identity is handle + ownerConnectionId), registers (or reuses) a type-1
// connection for `peer` already in-match (burst.spawned), and
// binds conn.link.owned_entity + the supplied NON-OWNING transport. Mirrors the post-PeerSpawned state
// the handshake pipeline leaves, minus the socket legs; NEVER publishes World::cached.local_player
// (spawn_remote_player). Returns the spawned handle (invalid if pool 0 is full / no AiSystem). The real
// path is handle_server_datagram -> Server_ProcessPendingPlayerSpawns.
world::EntityHandle admit_synthetic_peer(NapiNPServerCtx &ctx, world::World &world, const PeerAddr &peer,
                                         const world::PlayerSpawn &spawn,
                                         netsim::ISessionTransport *transport);

} // namespace opennova::np
