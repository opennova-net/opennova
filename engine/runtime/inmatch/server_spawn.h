#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include <runtime/world/player_spawn.h> // world::kRetailPlayerMinEntitySlot (canonical)

#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_server_ctx.h>

// P3 — the World-driven host-side player spawn (§5.2a steps 1-2 / §5.2b). Replaces the
// fixture/game_runtime spawn-gate: the listen-server host runs its own server-side spawn machinery
// in-process and registers the pool-0 player entity in `World` (ADR 0012), writing entity+0x78
// ownerConnectionId. Free functions over NapiNPServerCtx + world::World, mirroring the original flow
// 1:1. Godot-free / socket-free (engine/CLAUDE.md). The §5.2b field-init itself is already ported in
// engine/runtime/world (world::spawn_player / spawn_remote_player); these functions are the orchestration above
// it. [orig: Server_InitNewRoundState @0x51c8e0 -> CNapiServer_ProcessPendingPlayerSpawns @0x4c8dc0
// -> Server_BuildPlayerInfoAndAdd @0x51d560 -> Server_PlayerAdd @0x51cbc0; net-re §5.2a/§5.2b]
namespace opennova::world {
class World;
struct Entity;
struct SpawnPointResult;
}
namespace opennova::replication {
class ISessionTransport;
}

namespace opennova::inmatch {

struct HostRotation;

// Re-export the canonical world::kRetailPlayerMinEntitySlot into np for the spawn call sites (one
// definition, shared with the Godot listen host). [orig: §5.2b spawn placement]
inline constexpr uint16_t kRetailPlayerMinEntitySlot = world::kRetailPlayerMinEntitySlot;

// Reserve the team written to both the pre-spawn S2C 0x04 slot assignment and
// the later player entity. Repeated calls for one connection are idempotent.
// Pending MP reservations participate in autobalance, so admissions received
// in one socket drain cannot all observe stale World counts.
// The side arms (a side password, a TeamChoose request) return the team the
// rotation's side-to-team map gives that side (null: sides 1, 2), which the
// Attack-and-Defend swap exchanges between the halves.
// [orig: Server_AssignPlayerTeam @0x4fe310 writes playerSlot+416 before
// NetPacket_WriteSlotAssignment @0x502b30 reads it; byte_82F240 / byte_82F241
// @0x4FE408, @0x4FE456, @0x4FE492, @0x4FE507, @0x4FE543, @0x4FE56D]
uint8_t Server_ReservePlayerTeam(const GameConfig &config, bool is_in_session,
		const std::vector<NapiNPConnection> &roster, NapiNPConnection &conn,
		const world::World &world, const HostRotation *rotation = nullptr);

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
// is_authority && !world.match.outcome().ended. Walks connection_list; for each accepted-but-unspawned node
// (self_id_seen && phase < PlayerAdded) builds + registers its pool-0 player entity in `world`. The
// host's own type-2 loopback is just another entry in this list. Returns the number spawned this pass.
int Server_ProcessPendingPlayerSpawns(NapiNPServerCtx &ctx, world::World &world);

// §5.2a step 2 (per player) — [orig: Server_BuildPlayerInfoAndAdd @0x51d560 -> Server_PlayerAdd
// @0x51cbc0]. Build the spawn pose from the mission's promoted start markers (§5.2c,
// world::resolve_player_spawn_pose — never an NPC's spot) and register the pool-0 0x14B9 entity, stamping
// entity+0x78 = conn.connection_id. The type-2 loopback (the host's own client) -> world::spawn_player
// (publishes World::cached.local_player); a type-1 remote joiner -> world::spawn_remote_player (the
// host never republishes its local player). Binds the handle onto conn.link.owned_entity and advances
// conn.phase = PlayerAdded. Returns the spawned handle (invalid if pool 0 is full or World has no
// AiSystem).
world::EntityHandle Server_BuildPlayerInfoAndAdd(NapiNPServerCtx &ctx, NapiNPConnection &conn,
                                                 world::World &world);

// Mutate an already-added authoritative player between the ordinary body and
// retail spectator state. The connection flag is serialized by S2C 0x75; the
// neutral/hidden entity keeps its roster identity while simulation continues.
// Disabling respawns through the ordinary marker chain. Authority-only.
// [orig: Server_PlayerAdd @0x51cbc0; NapiNPClientMsg_SetSpectatorMode
// @0x4259e0; Entity_UpdateInfantryPlayerBody @0x4b40e0]
bool Server_SetPlayerSpectator(NapiNPServerCtx &ctx, NapiNPConnection &conn,
		world::World &world, bool spectator);

// Retarget an entity's team on the authority. A no-op unless the team
// changes. A Player with a slot also gets its stats reset (field 35 = 1, the
// script var at +408 cleared), its score-sound cache and armory cooldown
// zeroed, its slot team set, its NetId / animSlot re-stamped from the new
// side's character vars (side A for teams 1/3 or a non-team game), its squad
// link cleared with S2C 0x71 [0xFF][slot] to its new team and 0x72 [0][""] /
// [1][""] to itself. In a session every in-match slot then gets S2C 0x50
// (the identity pair zero for a non-player), and the entity joins the
// team-change list the C2S 0x29 walk reads. The zone half of retail's callers
// is world::ZoneCapture's team change (its 0x50 rides the zone event fan).
// [orig: Server_ChangeEntityTeam @0x518D70 — gates @0x518D84..0x518DAA, team
//  @0x518DB7, Entity_ValidatePtr @0x518DC5, CPlayerStats_ResetAllArrays
//  @0x518DD6, slot stores @0x518DDB..0x518DEF, side pick @0x518E27..0x518E71,
//  NetId / animSlot @0x518E7E / @0x518E8C, Server_SendPlayerStateAndSquad
//  @0x518B40 (the call @0x518E92), +356 @0x518E9A, 0x50 @0x518EA5..0x518EE1,
//  CBufferList_AddOrFind @0x518EEC]
void Server_ChangeEntityTeam(NapiNPServerCtx &ctx, world::World &world,
		world::EntityHandle entity, uint8_t team);

// Apply the C2S 0x51 requests the dispatcher admitted: the retail kill-and-
// convert — the slot's spectator latch and hide byte, its team cleared, the
// entity's command group cleared, the entity hidden with health 1 and damage
// disabled, its spawn-wave removal, the hold latch (the request named no
// target), the deploy leg (Server_ReleasePlayerDeployment) over the request's
// handle, its private replies, then S2C 0x32 [5][name] to every active player.
// Runs once per host tick before the state fan. [orig: Server_KillPlayerAndNotify
//  @0x519E00: gates @0x519E16..0x519E3F, the convert @0x519E5B..0x519EA4,
//  SpawnWaveList_RemovePlayer @0x519EB3, the latch @0x519EBC..0x519EC8,
//  Server_ProcessPlayerDeath @0x519ECE, 0x32 @0x519EDC..0x519F43]
void Server_ProcessSpectatorRespawnRequests(NapiNPServerCtx &ctx, world::World &world);

// The deploy leg's placement, between its raise and its spawn-state reset:
// the convert's hold arm (conn.reply.convert_holds_pose) places nothing, the
// body keeping its position with its three angle words zeroed; any other
// deploy runs Server_PositionPlayerForSpawn over the target (the spectator
// latch's team substitute, a medic revive's saved position, else the previous
// reset's position when nothing was placed). Writes the player's pose and
// returns the placement whose heading word and latches the deploy reads on.
// [orig: Server_ProcessPlayerDeath @0x517837..0x517868]
world::SpawnPointResult Server_PositionDeployingPlayer(const GameConfig &config,
		NapiNPConnection &conn, world::World &world, world::Entity &player,
		world::EntityHandle target_zone);

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
                                         replication::ISessionTransport *transport);

} // namespace opennova::inmatch
