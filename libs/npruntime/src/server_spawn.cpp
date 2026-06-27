#include "npruntime/server_spawn.h"

#include <world/player_spawn.h> // PlayerSpawn, spawn_player / spawn_remote_player
#include <world/spawn_select.h> // select_player_spawn (§5.2c start-marker scan)
#include <world/world.h>        // World, registry, cached

namespace opennova::np {

namespace {

// [D-NET-112] DIVERGENCE: the original has NO high-band player net-id allocator. It identifies
// every entity on the wire by its handle (pool<<12|slot, serialize_entity_states_to_packet
// @0x50f070) and carries four distinct id fields (ownerConnectionId@0x78=dcb, DcbId@0x7c=the
// find_by_net_id key, Ssn@0x2e, NetId@0x15c). The reimpl collapses those into one
// world::Entity::net_id, so a player's id must not collide with the small authored mission ids
// that share that field — hence this reserved high band. Scan downward and skip live ids so the
// sequence never wraps through 0xFFFF/0x0000 (fixes the prior count-based overflow + reuse). A
// faithful multi-field id model is the follow-up. [orig: Server_PlayerAdd @0x51cbc0]
uint16_t allocate_player_net_id(const world::World &world) {
	for (uint32_t candidate = kPlayerNetIdBase; candidate >= 0x8000u; --candidate) {
		const uint16_t id = static_cast<uint16_t>(candidate);
		if (id == 0 || id == 0xFFFFu) continue;
		if (!world.registry.find_by_net_id(id).valid()) return id;
		if (candidate == 0x8000u) break;
	}
	return 0;
}

// [orig: Server_AssignPlayerTeam @0x4fe310; D-NET-113] The spawning player's team.
// Witnessed branch order (Server_AssignPlayerTeam): (0) spectator (+100567 && is_in_session) -> 0;
// (1) co-op gametype ((game_type & 0xFFFDFFFF) == 0x10020) or any non-MP session (!is_in_session)
// -> 1; (2) DM/TDM -> requested team name / preference, then autobalance. We DROP branch (0) — no
// spectator field exists in the reimpl yet — so SP / co-op LAN land on team 1 faithfully via branch
// (1), and a real DM/TDM session autobalances to the least-populated side over the LIVE pool-0
// players already added (2-team: (t1 > t2) + 1). The spectator branch, the requested-team-name
// (g_team1/2_name) and team-preference legs, and 4-team placement are the follow-up MP path (the
// join request carries no team/spectator field yet); they default into the autobalance below.
uint8_t assign_player_team(const NapiNPServerCtx &ctx, const world::World &world) {
	constexpr uint32_t kCoopGameTypeMasked = 0x10020u;
	const uint32_t gt = ctx.game_settings.game_type;
	if (!ctx.is_in_session || (gt & 0xFFFDFFFFu) == kCoopGameTypeMasked) return 1;
	uint32_t team1 = 0, team2 = 0;
	world.registry.for_each([&](const world::Entity &e) {
		if (e.handle.pool() != 0 || e.item_id != world::kPlayerInfantryTypeId) return;
		if (e.team == 1) ++team1;
		else if (e.team == 2) ++team2;
	});
	return static_cast<uint8_t>((team1 > team2) + 1);
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
	spawn.team = assign_player_team(ctx, world); // [orig: Server_AssignPlayerTeam @0x4fe310]
	spawn.min_entity_slot = kRetailPlayerMinEntitySlot;
	// Reserved high-band player id (see allocate_player_net_id / D-NET-112 divergence note above).
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

// [orig: CNapiServer_ProcessPendingPlayerSpawns @0x4c8dc0] — gated is_authority && !dword_24D1DE0 &&
// !g_spawn_success_gate. dword_24D1DE0 is the mission-LOADING-in-progress flag (set/cleared all over
// Game_StartMission @0x524360); the original does NOT process spawns until the load completes and the
// pool-3 start markers are promoted. [D-NET-116] The reimpl maps g_spawn_success_gate -> spawn_success_gate
// but has no dword_24D1DE0 equivalent — acceptable today because the only callers (tests + the future
// host driver) wire ctx.world AFTER the world is loaded with its markers. A production driver that wires
// ctx.world DURING load must add a load-complete gate here, else select_player_spawn finds no marker and
// the idempotent origin fallback below latches the player at (0,0,0) permanently.
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
