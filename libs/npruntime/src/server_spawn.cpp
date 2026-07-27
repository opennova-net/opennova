#include "npruntime/server_spawn.h"

#include <world/player_spawn.h> // PlayerSpawn, spawn_player / spawn_remote_player
#include <world/spawn_select.h> // select_player_spawn (§5.2c) / world_has_spawn_zone (§5.61)
#include <world/world.h>        // World, registry, cached

#include <algorithm>
#include <array>
#include <optional>

namespace opennova::np {

namespace {

// [D-NET-112 FIXED 2026-06-27] The original assigns a player NO net-id/SSN. `Entity_SpawnFromAnimSlotProperty
// @0x43c390` memsets the entity and writes ONLY entity+0x78 (ownerConnectionId/dcb) as an id — Ssn@0x2e /
// DcbId@0x7c / NetId@0x15c are left 0. A player is identified by its pool HANDLE (pool<<12|slot, the wire
// identity) + ownerConnectionId (the client self-match, Player_FindLocalPlayerEntity @0x4e0090) — never by
// an SSN. The prior reimpl invented a high-band `allocate_player_net_id` only because tracking keyed on
// Entity.net_id; that allocator is DELETED. A player now carries net_id 0 (faithful), so it stays out of
// the WAC/BMS find_by_net_id SSN space (authored mission entities own that space); tracking is by handle +
// owner_connection_id. [orig: Server_PlayerAdd @0x51cbc0 / Entity_SpawnFromAnimSlotProperty @0x43c390]

// [orig: Server_AssignPlayerTeam @0x4fe310; D-NET-113] The spawning player's team.
// Witnessed branch order (Server_AssignPlayerTeam): (0) spectator (+100567 && is_in_session) -> 0;
// (1) co-op gametype ((game_type & 0xFFFDFFFF) == 0x10020) or any non-MP session (!is_in_session)
// -> 1; (2) DM/TDM -> requested team name / preference, then autobalance. We DROP branch (0) — no
// spectator field exists in the reimpl yet — so SP / co-op LAN land on team 1 faithfully via branch
// (1), and a real DM/TDM session autobalances to the least-populated side over the LIVE pool-0
// players already added (2-team: (t1 > t2) + 1). The spectator branch, the requested-team-name
// (g_team1/2_name) and team-preference legs, and 4-team placement are the follow-up MP path (the
// join request carries no team/spectator field yet); they default into the autobalance below.
uint8_t assign_player_team(const GameConfig &config, bool is_in_session,
		const std::vector<NapiNPConnection> &roster,
		const NapiNPConnection &joining, const world::World &world) {
	constexpr uint32_t kCoopGameTypeMasked = 0x10020u;
	const uint32_t gt = config.game_type;
	if (!is_in_session || (gt & 0xFFFDFFFFu) == kCoopGameTypeMasked) return 1;
	uint32_t team1 = 0, team2 = 0;
	world.registry.for_each([&](const world::Entity &e) {
		if (e.handle.pool() != 0 || e.item_id != world::kPlayerInfantryTypeId) return;
		if (e.team == 1) ++team1;
		else if (e.team == 2) ++team2;
	});
	// Reservations without an entity are already player-slot assignments for
	// balancing. Spawned reservations are represented by the World walk above.
	for (const NapiNPConnection &c : roster) {
		if (&c == &joining || !c.assigned_team_valid ||
		    c.link.owned_entity.valid())
			continue;
		if (c.assigned_team == 1) ++team1;
		else if (c.assigned_team == 2) ++team2;
	}
	return static_cast<uint8_t>((team1 > team2) + 1);
}

// Roster slots are stable identities. Retail walks the fixed player-slot table and installs the
// player into the first empty row; counting live players collides after a non-tail leave.
// [orig: Server_PlayerAdd @0x51CBC0 writes the first free dword_A87048 row]
bool connection_claims_player_slot(const NapiNPConnection &connection) {
	return connection.reply.player_slot_reserved ||
			connection.phase >= ConnectionPhase::PlayerAdded ||
			connection.link.owned_entity.valid();
}

std::optional<uint8_t> first_free_player_slot(
		const std::vector<NapiNPConnection> &roster,
		const NapiNPConnection *joining, uint32_t slot_capacity) {
	std::array<bool, 256> occupied{};
	for (const NapiNPConnection &connection : roster) {
		if (&connection == joining ||
		    !connection_claims_player_slot(connection)) continue;
		occupied[connection.reply.player_slot] = true;
	}
	const std::size_t bounded_capacity =
			std::min<std::size_t>(slot_capacity, occupied.size());
	for (std::size_t slot = 0; slot < bounded_capacity; ++slot) {
		if (!occupied[slot]) return static_cast<uint8_t>(slot);
	}
	return std::nullopt;
}

} // namespace

std::optional<uint8_t> Server_ReservePlayerSlot(
		const std::vector<NapiNPConnection> &roster, NapiNPConnection &conn,
		uint32_t slot_capacity) {
	if (connection_claims_player_slot(conn)) {
		if (conn.reply.player_slot >= slot_capacity) return std::nullopt;
		// A repeated post-handshake send and the alternate explicit-bind seam
		// are idempotent, but never bless a duplicate claim as authoritative.
		for (const NapiNPConnection &other : roster) {
			if (&other == &conn ||
			    !connection_claims_player_slot(other)) continue;
			if (other.reply.player_slot == conn.reply.player_slot)
				return std::nullopt;
		}
		return conn.reply.player_slot;
	}

	const std::optional<uint8_t> slot =
			first_free_player_slot(roster, &conn, slot_capacity);
	if (!slot.has_value()) return std::nullopt;
	conn.reply.player_slot = *slot;
	conn.reply.player_slot_reserved = true;
	return slot;
}

uint8_t Server_ReservePlayerTeam(const GameConfig &config, bool is_in_session,
		const std::vector<NapiNPConnection> &roster, NapiNPConnection &conn,
		const world::World &world) {
	if (!conn.assigned_team_valid) {
		conn.assigned_team =
				assign_player_team(config, is_in_session, roster, conn, world);
		conn.assigned_team_valid = true;
	}
	return conn.assigned_team;
}

// [orig: Server_InitNewRoundState @0x51c8e0] — local-player/round context for an authority host.
void Server_InitNewRoundState(NapiNPServerCtx &ctx) {
	if (!ctx.is_authority) return;
	// Fresh round: the per-connection §5.2a burst cursor (conn.burst) is the host-side spawn/load clock
	// — there is no host-global load-progress counter (the client's dword_A82370 walk is client state,
	// not host bookkeeping; D-NET-132). The spawn-success gate is dropped later by the per-frame 0x0A
	// flags1 & 0x01 (§5.2a step 4), not here — the original clears the loading-*timeout* gate at this
	// step, not the spawn gate.
	(void)ctx;
}

// [orig: Server_BuildPlayerInfoAndAdd @0x51d560 -> Server_PlayerAdd @0x51cbc0]
world::EntityHandle Server_BuildPlayerInfoAndAdd(NapiNPServerCtx &ctx, NapiNPConnection &conn,
                                                 world::World &world) {
	const std::optional<uint8_t> player_slot =
			Server_ReservePlayerSlot(
					ctx.np_protocol.connection_list, conn,
					std::min<uint32_t>(ctx.config.max_players, 251u));
	if (!player_slot.has_value()) return {};

	world::PlayerSpawn spawn;
	// Team FIRST — a team gametype's start markers are per-team (6096-6099 primary,
	// 6003/6004/6090/6091 fallback), so the AS join spawn needs the assigned team before the
	// §5.2c marker scan; without the split both teams land in team 1's base (net-re §5.61).
	// [orig: Server_AssignPlayerTeam @0x4fe310 runs in Server_PlayerAdd BEFORE
	// Server_PositionPlayerForSpawn's team switch @0x50d266]
	spawn.team = Server_ReservePlayerTeam(
			ctx.config, ctx.is_in_session, ctx.np_protocol.connection_list,
			conn, world); // [orig: Server_AssignPlayerTeam @0x4fe310]
	const world::SpawnPointResult sel =
			world::select_player_spawn_for_team(world, spawn.team, ctx.config.game_type);
	if (sel.found) {
		spawn.position = sel.position; // mission space, straight from the chosen marker
		spawn.yaw = sel.yaw;
	} else {
		// No start marker authored: spawn at the mission origin (terrain clamp grounds it). Never an
		// NPC position. [orig: Entity_FindBestSpawnPoint @0x50ccc0 returns no marker -> caller fallback]
		spawn.position = {0.0f, 0.0f, 0.0f};
		spawn.yaw = 0;
	}
	spawn.min_entity_slot = kRetailPlayerMinEntitySlot;
	// [D-NET-112] A player carries no SSN (net_id 0) — faithful to the original, which identifies it by
	// handle + ownerConnectionId, not an SSN (see the note above). Keeps players out of the WAC find_by_net_id space.
	spawn.net_id = 0;
	// entity+0x78 = the owning connection's dcb (host loopback dcb / a joiner's ack dcb).
	// [orig: Server_PlayerAdd @0x51cbc0 writes entity+0x78 = conn->connection_id]
	spawn.owner_connection_id = conn.connection_id;
	// Equipped-weapon spawn default = the WPN_M4AUTO armory index, resolved BY NAME like retail
	// [orig: PlayerClass_InitEntity @0x4B1116 -> AvatarDef_FindIndexByName("WPN_M4AUTO")];
	// 0xFF (none) when no armory table is fed (unit-test hosts). A joiner's own extended uplink
	// overwrites it on the first drained 0x0C. (D-NET-143)
	const int m4 = world.weapons.index_of("WPN_M4AUTO");
	spawn.equipped_adm_index = m4 >= 0 ? static_cast<uint8_t>(m4) : 0xFF;

	// The type-2 loopback is the host's OWN client (input-ordered, publishes cached.local_player); a
	// type-1 node is a remote joiner the host snaps from the wire (never the local player). [ADR 0012]
	const bool is_host_own = (conn.type == 2);

	// Character stamp from the joiner's 0x42 CU vars, picked per ASSIGNED team — side A for teams
	// 1/3 or any non-team-based game type, side B otherwise [orig: Server_PlayerAdd @0x51cbc0
	// @0x51cff7]. The picked avatar byte is the entity+0x374 animSlot [@0x51d0b1] and the picked
	// char id the entity+0x15C wire NetId [slot+440] — the joiner's own 0x0C record must echo the
	// values it uploaded (golden ASH_I5A: VCB=4/CI1=0x8207 -> record 4/0x8207) or its client binds
	// a wrong-type character slot: the D-NET-146 DBuggy1-shadow bug. Retail additionally validates
	// the char id against the character-slot registry (MinimapSlot_HasEntity @0x57b140 -> realloc
	// @0x57ad40); the reimpl has no registry yet, so the id is echoed unvalidated and 0 falls back
	// to the encoder's D-NET-137 shim (two default-profile joiners colliding on 0x8207 is a
	// tracked deferral, harmless at 2-player scope).
	const int side = (spawn.team == 1 || spawn.team == 3 ||
	                  (ctx.config.game_type & 0x10000u) == 0)
	        ? 0
	        : 1;
	if (is_host_own) {
		// The host's own player never uploads CU vars — retail stamps its animSlot on the LOCAL
		// path from the profile avatar byte, default-resolved to 1 when the profile carries none
		// (the golden host record). Its NetId comes from local deploy, not this record -> keep 0
		// (the encoder shim emits the golden 0x0200). [orig: Player_InitPlayer @0x4e15f0
		// (@0x4e1843) <- g_avatarTeam1/2 <- apply_session_settings_to_globals @0x551500 with the
		// sub_57AE60 not-found default 1]
		spawn.anim_slot = 1;
	} else {
		spawn.anim_slot = conn.char_vars.avatar[side];       // raw echo; 0 = tag absent (retail)
		spawn.minimap_net_id = conn.char_vars.char_id[side]; // 0 -> encoder shim
		// playerClass = TR ? CTB : CTA [orig: Server_BuildPlayerInfoAndAdd @0x51d711 buf[52]];
		// absent (0) -> 8 in-session [@0x51d02b]; outside [5,9] -> 8 [@0x51d102]. The joiner's
		// later C2S 0x2F loadout re-stamps it (server_message_dispatch case 0x2F), same as retail.
		const uint8_t cls = conn.char_vars.team_request != 0 ? conn.char_vars.char_class[1]
		                                                     : conn.char_vars.char_class[0];
		spawn.player_class = (cls >= 5 && cls <= 9) ? cls : 8;
	}

	const world::EntityHandle h =
			is_host_own ? world::spawn_player(world, spawn) : world::spawn_remote_player(world, spawn);
	if (!h.valid()) return h;

	conn.link.owned_entity = h; // the per-connection S2C anchor + C2S owner-verify subject

	// RESPAWN-PENDING at join, iff the mission offers deploy-selectable spawn zones — the
	// joiner enters UNDEPLOYED and its per-frame 0x0A flags1 bit1 holds the deploy screen
	// open until a successful C2S 0x0E pick clears it [orig: Server_OnPlayerJoin @0x51a6f2
	// stateByte |= 0x10 iff SpawnZoneList_GetCount() > 0; the pre-placed entity is the
	// deploy-camera anchor]. The pending entity is HIDDEN (state_flags bit0 — the golden
	// pre-deploy record byte13 = 0x01) [orig: NetPacket_WritePlayerState @0x4ff7dd ORs
	// entity+36 bit0 each frame while pending]. The host's OWN loopback player skips the
	// hold — it deploys through the local flow, not the wire. (D-NET-156)
	if (!is_host_own && world::world_has_spawn_zone(world)) {
		conn.link.respawn_pending = true;
		if (world::Entity *pe = world.registry.get(h)) {
			pe->flags |= 1u;
			pe->damage_state = 620;
		}
		// The join-time respawn countdown (entity+292 = 620 ticks) is display/wave state the
		// 0x6E status reports; with default host wave options the deploy is pick-driven, so
		// only the pending flag is modeled (tracked, §5.61).
	}

	// Bind the per-connection reply state so the §5.1 roster (0x16) / player-sync (0x46) / player-index
	// (0x4D) all point at THIS player's REAL slot + entity handle. Without it the joiner is told
	// player-slot 0 -> entity 0 (≠ its dcb entity at the actual pool-0 slot) and can never bind its
	// local player -> never deploys (golden: 0x4D=1, 0x16 grows to slot 1, 0x46 slot 1 -> the joiner's
	// own entity). Slot assignment walks the fixed roster table and takes its first free row, so a
	// non-tail disconnect can be reused without colliding with a later live player.
	// [orig: Server_PlayerAdd @0x51cbc0 writes the player into dword_A87048[slot]; 0x4D/0x16/0x46 read it]
	// D-NET-132: link.owned_entity (bound above) IS the roster binding — the reply builders read the
	// wire handle off owned_entity.packed and the team off the live entity (team @entity+344). Only the
	// roster ORDER (player_slot) and the echoed name stay on conn.reply.
	conn.reply.player_slot = *player_slot;
	// Golden parity: a player carries a NAME in BOTH its 0x0C organic record (entity name, read by
	// build_pool0_organic_batch) and its 0x46 player-sync (conn.reply.player_name). The host's own
	// loopback connection carries no ClientHello name, so fall back to the host profile name
	// (config.player_name) — an EMPTY host-player name diverges from the golden retail host record
	// (name="cdouglass") and leaves the joiner's roster slot / minimap tag unnamed. [golden diff 2026-07-01]
	const std::string resolved_name =
			!conn.player_name.empty() ? conn.player_name : ctx.config.player_name;
	if (!resolved_name.empty()) {
		conn.reply.player_name = resolved_name;
		if (world::Entity *e = world.registry.get(h)) e->name = resolved_name;
	}

	conn.phase = ConnectionPhase::PlayerAdded;
	conn.reply.player_slot_reserved = false;
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
		// create_session) or a joiner that completed the C2S 0x00 -> 0x01 -> 0x02 admission
		// exchange (and may later restamp its dcb via 0x48). Mid-handshake nodes are skipped.
		if (!conn.self_id_seen) continue;
		if (conn.phase >= ConnectionPhase::PlayerAdded) continue; // already spawned
		if (Server_BuildPlayerInfoAndAdd(ctx, conn, world).valid()) ++spawned;
	}
	return spawned;
}

// See header. Synthetic in-process peer admit (no handshake) — the owner/test hook.
world::EntityHandle admit_synthetic_peer(NapiNPServerCtx &ctx, world::World &world, const PeerAddr &peer,
                                         const world::PlayerSpawn &spawn_in,
                                         netsim::ISessionTransport *transport) {
	// Reuse an existing connection for this peer, or prepare a fresh type-1 node id.
	NapiNPConnection *conn = nullptr;
	for (NapiNPConnection &c : ctx.np_protocol.connection_list) {
		if (c.peer == peer) {
			conn = &c;
			break;
		}
	}
	const uint32_t conn_id = conn ? conn->connection_id : ctx.np_protocol.next_connection_id;

	world::PlayerSpawn spawn = spawn_in;
	spawn.min_entity_slot = kRetailPlayerMinEntitySlot;
	// [D-NET-112] No SSN for a player (net_id 0) — faithful; identity is handle + ownerConnectionId(dcb).
	spawn.net_id = 0;
	spawn.owner_connection_id = conn_id; // entity+0x78 dcb
	const world::EntityHandle h = world::spawn_remote_player(world, spawn);
	if (!h.valid()) return h;

	if (conn == nullptr) {
		NapiNPConnection c;
		c.peer = peer;
		c.type = 1; // server-side view of a remote client
		c.connection_id = ctx.np_protocol.next_connection_id++;
		c.self_id_seen = true;
		ctx.np_protocol.connection_list.push_back(c);
		conn = &ctx.np_protocol.connection_list.back();
	}
	conn->assigned_team = spawn.team;
	conn->assigned_team_valid = true;
	conn->link.owned_entity = h;
	conn->link.transport = transport;
	conn->link.mode = netsim::TransportMode::Client;
	conn->phase = ConnectionPhase::PlayerAdded;
	conn->burst.spawned = true; // in-match (is_in_match): drained + emitted by Server_TickUpdate
	return h;
}

} // namespace opennova::np
