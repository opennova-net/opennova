#pragma once

#include <cstdint>

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

namespace opennova::np {

// Re-export the canonical world::kRetailPlayerMinEntitySlot into np for the spawn call sites (one
// definition, shared with the Godot listen host). [orig: §5.2b spawn placement]
inline constexpr uint16_t kRetailPlayerMinEntitySlot = world::kRetailPlayerMinEntitySlot;

// High reserved SSN base for player entities — far above the small mission bms ids. P3 allocates
// downward from this base and skips already-live ids so it never wraps into 0xFFFF/0x0000.
inline constexpr uint16_t kPlayerNetIdBase = 0xFFF0u;

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

} // namespace opennova::np
