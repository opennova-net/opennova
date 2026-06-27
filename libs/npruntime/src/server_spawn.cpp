#include "npruntime/server_spawn.h"

#include <world/player_spawn.h> // PlayerSpawn, spawn_player / spawn_remote_player
#include <world/spawn_select.h> // select_player_spawn (§5.2c start-marker scan)
#include <world/world.h>        // World, registry, cached

namespace opennova::np {

namespace {

uint16_t allocate_player_net_id(const world::World &world) {
	for (uint32_t candidate = kPlayerNetIdBase; candidate >= 0x8000u; --candidate) {
		const uint16_t id = static_cast<uint16_t>(candidate);
		if (id == 0 || id == 0xFFFFu) continue;
		if (!world.registry.find_by_net_id(id).valid()) return id;
		if (candidate == 0x8000u) break;
	}
	return 0;
}

} // namespace

// [orig: Server_InitNewRoundState @0x51c8e0] — local-player/round context for an authority host.
void Server_InitNewRoundState(NapiNPServerCtx &ctx) {
	if (!ctx.is_authority) return;
	// Fresh round: reset the loading-progress counter (the §5.1 dword_A82370 walk restarts). The
	// spawn-success gate is dropped later by the per-frame 0x0A flags1 & 0x01 (§5.2a step 4), not here
	// — the original clears the loading-*timeout* gate at this step, not the spawn gate.
	ctx.loading_progress = 0;
}

// [orig: Server_BuildPlayerInfoAndAdd @0x51d560 -> Server_PlayerAdd @0x51cbc0]
world::EntityHandle Server_BuildPlayerInfoAndAdd(NapiNPServerCtx &ctx, NapiNPConnection &conn,
                                                 world::World &world) {
	(void)ctx;
	// §5.2c spawn-pose selection from the mission's promoted start markers (never an NPC's spot).
	const world::SpawnPointResult sel = world::select_player_spawn(world);
	world::PlayerSpawn spawn;
	if (sel.found) {
		spawn.position = sel.position; // mission space, straight from the chosen marker
		spawn.yaw = sel.yaw;
	} else {
		// No start marker authored: spawn at the mission origin (terrain clamp grounds it). Never an
		// NPC position. [orig: Entity_FindBestSpawnPoint @0x50ccc0 returns no marker -> caller fallback]
		spawn.position = {0.0f, 0.0f, 0.0f};
		spawn.yaw = 0;
	}
	spawn.team = 1; // placeholder until the MP team path (matches the Godot listen host)
	spawn.min_entity_slot = kRetailPlayerMinEntitySlot;
	// Distinct high-band SSN per player. Allocate by scanning downward from the reserved base so the
	// sequence never wraps through 0xFFFF/0x0000 and never collides with authored mission net IDs.
	spawn.net_id = allocate_player_net_id(world);
	if (spawn.net_id == 0) return {};
	// entity+0x78 = the owning connection's dcb (host loopback dcb / a joiner's ack dcb).
	// [orig: Server_PlayerAdd @0x51cbc0 writes entity+0x78 = conn->connection_id]
	spawn.owner_connection_id = conn.connection_id;

	// The type-2 loopback is the host's OWN client (input-ordered, publishes cached.local_player); a
	// type-1 node is a remote joiner the host snaps from the wire (never the local player). [ADR 0012]
	const bool is_host_own = (conn.type == 2);
	const world::EntityHandle h =
			is_host_own ? world::spawn_player(world, spawn) : world::spawn_remote_player(world, spawn);
	if (!h.valid()) return h;

	conn.link.owned_entity = h; // the per-connection S2C anchor + C2S owner-verify subject
	conn.phase = ConnectionPhase::PlayerAdded;
	return h;
}

// [orig: CNapiServer_ProcessPendingPlayerSpawns @0x4c8dc0]
int Server_ProcessPendingPlayerSpawns(NapiNPServerCtx &ctx, world::World &world) {
	if (!ctx.is_authority || ctx.spawn_success_gate != 0) return 0;
	int spawned = 0;
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		// Spawn an accepted-but-unspawned player: the host loopback (self_id_seen latched at
		// create_session) or a joiner past the 0x42 join (self_id_seen latched in handle_client_join /
		// the 0x48 ack). Mid-handshake nodes (self_id_seen == false) are skipped until accepted.
		if (!conn.self_id_seen) continue;
		if (conn.phase >= ConnectionPhase::PlayerAdded) continue; // already spawned
		if (Server_BuildPlayerInfoAndAdd(ctx, conn, world).valid()) ++spawned;
	}
	return spawned;
}

} // namespace opennova::np
