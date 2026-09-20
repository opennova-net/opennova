#include <runtime/inmatch/server_spawn.h>

#include <runtime/world/ai.h>           // AiEntity / AiSystem
#include <runtime/world/angle.h>        // bam_heading_from_mission_yaw_deg
#include <runtime/world/entity_spawn.h> // entity_reset_to_spawn_state
#include <runtime/world/infantry.h>     // infantry_respawn_snap
#include <runtime/world/player_spawn.h> // PlayerSpawn, spawn_player / spawn_remote_player
#include <runtime/world/spawn_select.h> // resolve_player_spawn_pose / world_has_spawn_zone
#include <runtime/world/vehicle_attach.h>
#include <runtime/world/world.h>        // World, registry, cached

#include <base/gameprofile/game_type.h>
#include <base/io/strutil.h>
#include <net/npwire/ingame_message_id.h> // s2c::FORMATTED_GAME_TEXT (the 0x51 convert notice)

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace opennova::inmatch {

namespace {

// [D-NET-112 FIXED 2026-06-27] The original assigns a player NO net-id/SSN. `Entity_SpawnFromAnimSlotProperty
// @0x43c390` memsets the entity and writes ONLY entity+0x78 (ownerConnectionId/dcb) as an id — Ssn@0x2e /
// DcbId@0x7c / NetId@0x15c are left 0. A player is identified by its pool HANDLE (pool<<12|slot, the wire
// identity) + ownerConnectionId (the client self-match, Player_FindLocalPlayerEntity @0x4e0090) — never by
// an SSN. The prior reimpl invented a high-band `allocate_player_net_id` only because tracking keyed on
// Entity.net_id; that allocator is DELETED. A player now carries net_id 0 (faithful), so it stays out of
// the WAC/BMS find_by_net_id SSN space (authored mission entities own that space); tracking is by handle +
// owner_connection_id. [orig: Server_PlayerAdd @0x51cbc0 / Entity_SpawnFromAnimSlotProperty @0x43c390]

// [orig: Server_AssignPlayerTeam @0x4FE310; D-NET-113] One assignment policy for every mode.
// Retail's misleading g_team1_name/g_team2_name symbols are the live SidePasswordA/B strings
// (apply_session_settings_to_globals @0x552043/@0x552054), not a second team-name domain.
// The submitted JSP credential selects a matching protected side before balancing.
uint8_t assign_player_team(const GameConfig &config, bool is_in_session,
		const std::vector<NapiNPConnection> &roster,
		const NapiNPConnection &joining, const world::World &world) {
	// A spectator is a roster player on neutral team zero, bypassing the team
	// password/selection path. Retail gates the early return on being in a
	// live MP session; a non-session add ignores the flag and takes team 1.
	// [orig: Server_AssignPlayerTeam @0x4fe310 — `slot+100567 && is_in_session`
	// -> +416 = 0, the FIRST leg before the co-op/solo team-1 return]
	if (joining.link.spectator && is_in_session) return 0;
	const uint32_t gt = config.game_type;
	if (!is_in_session || opennova::game_type::is_waypoint_family(gt)) return 1;

	// A freshly allocated solo player is already present in retail's fixed slot
	// table, so the no-team-bit scan finds the same pointer and stores team 1.
	// Do not autobalance DM/Flag Me into a fictitious team 2.
	// [orig: Server_AssignPlayerTeam @0x4FE398..0x4FE400]
	if (!opennova::game_type::is_team(gt)) return 1;

	const uint8_t active_teams =
			opennova::game_type::active_team_count(gt, config.num_teams);
	const bool side_a_locked = !config.side_a_password.empty();
	const bool side_b_locked = !config.side_b_password.empty();

	// The add event carries only the first 16 characters of the submitted JSP
	// (Napi_CopyString(event+80, net_cfg.fid, 17) = 16 chars + NUL), and that
	// truncated copy is what the ordered side compares see. Admission
	// (validate_side_password) compares the untruncated field, as retail's
	// Server_ValidatePlayerJoinRequest does.
	// [orig: Server_BuildPlayerInfoAndAdd @0x51D686 (17-byte copy) ->
	// Server_AssignPlayerTeam @0x4FE43F]
	const std::string submitted_password = joining.join_password.substr(0, 16);
	// Password matches precede the two/four-team split and team preference.
	// If both side passwords match, side A wins the ordered comparison.
	// [orig: Server_AssignPlayerTeam @0x4FE424..0x4FE4AD]
	if (side_a_locked && opennova::strutil::iequals(
			config.side_a_password.c_str(), submitted_password.c_str())) return 1;
	if (side_b_locked && opennova::strutil::iequals(
			config.side_b_password.c_str(), submitted_password.c_str())) return 2;
	// Without a match, two-team mode selects an unlocked side or fails.
	// [orig: @0x4FE4AE..0x4FE519]
	if (active_teams == 2) {
		if (side_a_locked && side_b_locked) return 0;
		if (side_a_locked) return 2;
		if (side_b_locked) return 1;
	}

	// jsp[60] is signed at the original call site: 0/1 request side A/B and
	// 0xFF means automatic. A locked requested side falls through to balance.
	// Server_PlayerAdd overwrites the add event's preference byte with -1
	// BEFORE the assignment runs whenever the game type is exactly 0x10000
	// (TDM) and either side password is set, so a TDM host with any locked
	// side never honors TR; only the four-team path can observe the
	// difference, since the two-team count legs above already decided.
	// [orig: Server_PlayerAdd @0x51CC76..0x51CC91 (event+97 = -1), passed as
	// teamPref @0x51CF24; assignment @0x4FE51A..0x4FE587]
	uint8_t team_request = joining.char_vars.team_request;
	if (gt == opennova::game_type::kTeamDeathmatch && (side_a_locked || side_b_locked))
		team_request = 0xFF;
	if ((config.mp_attributes & GameConfig::kMpAttribTeamChoose) != 0) {
		if (team_request == 0 && !side_a_locked) return 1;
		if (team_request == 1 && !side_b_locked) return 2;
	}

	std::array<uint32_t, 4> counts{};
	world.registry.for_each([&](const world::Entity &e) {
		if (e.handle.pool() != 0 || e.item_id != world::kPlayerInfantryTypeId) return;
		if (e.team >= 1 && e.team <= 4) ++counts[e.team - 1];
	});
	// Reservations without an entity are already player-slot assignments for
	// balancing. Spawned reservations are represented by the World walk above.
	for (const NapiNPConnection &c : roster) {
		if (&c == &joining || !c.assigned_team_valid ||
		    c.link.owned_entity.valid())
			continue;
		if (c.assigned_team >= 1 && c.assigned_team <= 4)
			++counts[c.assigned_team - 1];
	}

	if (active_teams == 2)
		return static_cast<uint8_t>((counts[0] > counts[1]) + 1);

	// Retail pairwise-sorts both the counts and their original indices. It then
	// overwrites sorted count slots 0/1 with SidePasswordA/B-present booleans,
	// but probes that array with the ORIGINAL indices. That mixed index space is
	// a shipped defect: an unprotected empty lobby alternates 1,2,1,2; protected
	// four-team configurations can select team 3. Preserve the instructions,
	// rather than substituting an ideal least-populated-four policy.
	// [orig: @0x4FE62C..0x4FE723]
	std::array<uint8_t, 4> original_indices = {0, 1, 2, 3};
	for (std::size_t i = 0; i < counts.size() - 1; ++i) {
		for (std::size_t j = i + 1; j < counts.size(); ++j) {
			if (counts[i] <= counts[j]) continue;
			std::swap(counts[i], counts[j]);
			std::swap(original_indices[i], original_indices[j]);
		}
	}
	counts[0] = side_a_locked ? 1u : 0u;
	counts[1] = side_b_locked ? 1u : 0u;
	for (uint8_t original_index : original_indices) {
		if (counts[original_index] == 0)
			return static_cast<uint8_t>(original_index + 1);
	}
	return 0;
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
	// reset_round_counters copies the configured StartDelay seconds into the
	// one live pre-round timer. Keep the timer on World: it is the authority
	// phase predicate and the source of the phase-0 0x0A projection, rather
	// than a second connection-local countdown.
	// [orig: reset_round_counters @0x516C50, store @0x516C8D]
	if (ctx.world != nullptr)
		ctx.world->preround_delay_seconds = ctx.config.start_delay;
	// The stock round initializer clears the global S2C 0x79 countdown. Its next
	// Server_TickUpdate boundary therefore emits immediately and reloads 0x136.
	// [orig: Server_InitNewRoundState @0x51CA9E]
	ctx.network_quality_broadcast_countdown = 0;
	// Fresh round: the per-connection §5.2a burst cursor (conn.burst) is the host-side spawn/load clock
	// — there is no host-global load-progress counter (the client's dword_A82370 walk is client state,
	// not host bookkeeping; D-NET-132). The spawn-success gate is dropped later by the per-frame 0x0A
	// flags1 & 0x01 (§5.2a step 4), not here — the original clears the loading-*timeout* gate at this
	// step, not the spawn gate.
}

// [orig: Server_BuildPlayerInfoAndAdd @0x51d560 -> Server_PlayerAdd @0x51cbc0]
world::EntityHandle Server_BuildPlayerInfoAndAdd(NapiNPServerCtx &ctx, NapiNPConnection &conn,
                                                 world::World &world) {
	const std::optional<uint8_t> player_slot =
			Server_ReservePlayerSlot(
					ctx.np_protocol.connection_list, conn,
					ctx.config.total_player_slot_capacity());
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
	// The join spawn enters the no-pick arm (Server_OnPlayerJoin passes spawn
	// handle low word 0, which Server_ResolveSpawnTargetHandle rejects), where
	// a spectator slot is POSITIONED with the substitute team (Co-op 1; team
	// modes 2 - (tick & 1) for this fresh, not-dead entity) while its assigned
	// team stays 0 — the hidden body lands on a real start marker, never at
	// the origin. [orig: Server_OnPlayerJoin @0x51A786 ->
	// Server_PositionPlayerForSpawn @0x50D17C..0x50D1C6; the latch is
	// slot+100567 stored by Server_PlayerAdd @0x51CD83]
	const world::SpawnSlotState slot_state{
			conn.link.spectator, conn.spectator_restore_team};
	const world::SpawnPointResult sel =
			world::resolve_player_spawn_pose(
					world, world::EntityHandle{}, world::EntityHandle{},
					*player_slot, spawn.team, ctx.config.game_type, slot_state);
	if (sel.found) {
		spawn.position = sel.position; // mission space, straight from the chosen marker
		spawn.yaw = sel.yaw;
		spawn.pitch = sel.pitch;
		spawn.roll = sel.roll;
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
	const int m4 = world.tables.weapons.index_of("WPN_M4AUTO");
	spawn.equipped_adm_index = m4 >= 0 ? static_cast<uint8_t>(m4) : 0xFF;

	// The type-2 loopback is the host's OWN client (input-ordered, publishes cached.local_player); a
	// type-1 node is a remote joiner the host snaps from the wire (never the local player). [ADR 0012]
	const bool is_host_own = (conn.type == NapiNPConnection::kTypeClientSide);

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
	const int side = (conn.link.spectator || spawn.team == 1 || spawn.team == 3 ||
	                  (ctx.config.game_type & 0x10000u) == 0)
	        ? 0
	        : 1;
	if (is_host_own) {
		// The host's own player never uploads CU vars — start_host_session installs the
		// mounted profile's per-side selection on the type-2 loopback (stock fresh-profile
		// seed when nothing is saved: side A 0x0200/avatar 1 = the golden host record). Retail
		// stamps the SAME profile fields on its LOCAL path, picked by the assigned team's side:
		// animSlot <- g_avatarTeam1/2, the packed character/minimap id from the validated
		// profile ids, playerClass <- g_charClassTeam1/2 (clamped [5,9] at session start).
		// [orig: Player_InitPlayer @0x4e15f0 (@0x4e1843) <- g_avatarTeam1/2 + g_charClassTeam1/2
		// <- apply_session_settings_to_globals @0x551500 (class clamp @0x5516ab..0x5516ec, avatar
		// via Avatars_ResolveSelectionIndex (ex sub_57AE60)); ids validated + stored by PlayerSession_InitFromProfile @0x50ca80]
		spawn.anim_slot = conn.char_vars.avatar[side];
		spawn.minimap_net_id = conn.char_vars.char_id[side];
		const uint8_t cls = conn.char_vars.char_class[side];
		spawn.player_class = (cls >= 5 && cls <= 9) ? cls : 8;
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
	// The Co-op marker arm's survivors land on the fresh entity: the marker's
	// chute bit and, for a team-2 marker, the queued 0x200 mount onto its
	// carrier (+0x16C/+0x180). [orig: Server_PositionPlayerForSpawn @0x50D424..0x50D45A]
	world::apply_spawn_point_latches(*world.registry.get(h), sel);

	conn.link.owned_entity = h; // the per-connection S2C anchor + C2S owner-verify subject
	conn.link.owned_entity_spawn_id = world.registry.get(h)->registry_spawn_id;
	conn.link.last_deploy_tick = world.logic_tick;
	conn.link.last_deploy_tick_valid = true;
	// [orig: Server_BuildPlayerInfoAndAdd @0x51D560 binds the newly allocated
	// player row into the recipient slot used by Server_SendEntityStateToPlayer]

	// RESPAWN-PENDING at join, iff the mission offers deploy-selectable spawn zones — the
	// joiner enters UNDEPLOYED and its per-frame 0x0A flags1 bit1 holds the deploy screen
	// open until a successful C2S 0x0E pick clears it [orig: Server_OnPlayerJoin @0x51a6f2
	// stateByte |= 0x10 iff SpawnZoneList_GetCount() > 0 — UNCONDITIONAL on the spectator
	// latch, so a join-time spectator holds the bit forever (nothing ever deploys it);
	// the sole clear is the deploy leg of Server_ProcessPlayerDeath @0x517791]. The
	// pending entity is HIDDEN (state_flags bit0 — the golden pre-deploy record
	// byte13 = 0x01) [orig: NetPacket_WritePlayerState @0x4ff7dd ORs entity+36 bit0
	// each frame while pending]. The host's OWN loopback player skips the hold — it
	// deploys through the local flow, not the wire. (D-NET-156)
	if (!is_host_own && world.zones.has_spawn_zone()) {
		conn.link.respawn_pending = true;
		if (!conn.link.spectator) {
			if (world::Entity *pe = world.registry.get(h)) pe->flags |= 1u;
		}
	}
	// Every created player entity starts under the 620-tick spawn protection
	// (entity+292): the round-init creation stores 620 for every active slot and
	// the join stores it unconditionally after the 0x0F send (a bot slot +96483
	// stores 0; our roster has none). It is host-own and spawn-zone independent.
	// The per-tick arm in Server_TickUpdate counts it down in a live MP session,
	// zeroes it outside one, and holds a spectator at -1 (the unwitnessed +97538
	// latch our spectator bit stands in for, D-NET-217; the join's own -1 store
	// @0x51a7b3 is overwritten by that same 620 and restored by the arm).
	// [orig: Server_InitAllPlayerEntitiesForRound @0x516AA0 @0x516bba;
	//  Server_OnPlayerJoin @0x51A680 @0x51a882 (620), @0x51a878 (bot 0)]
	if (world::Entity *pe = world.registry.get(h)) pe->damage_state = 620;
	// The join writes the whole +89912 state byte: bit 1 (the pre-round loadout
	// latch) iff the pre-round timer runs, and zeroes the +356 armory cooldown on
	// the first state-6 entry. [orig: Server_OnPlayerJoin @0x51a6d3/@0x51a6e2
	//  (byte = 1, or 3 while g_preround_delay_timer); slot[89] = 0 @0x51a752]
	conn.link.preround_loadout_latch = world.preround_delay_seconds != 0;
	conn.link.armory_reuse_seconds = 0;
	if (conn.link.spectator) {
		// Retail still creates a player entity for a spectator, but leaves it
		// hidden and permanently damage-disabled while S2C 0x75 drives the
		// client's free-fly camera: the spectator leg stores entity+36 |= 1
		// and entity+292 = -1 (no 620-tick countdown — the dead/disabled
		// sentinel). The deploy-hold bit set above stays held, so the
		// spectator's 0x0A flags1 reads 0x03 (spectator | deploy-hold) exactly
		// like a retail join-time spectator's slot state. [orig:
		// Server_PlayerAdd @0x51cbc0 — the slot+100567 branch;
		// Entity_UpdateInfantryPlayerBody @0x4b40e0]
		if (world::Entity *pe = world.registry.get(h)) {
			pe->team = 0;
			pe->flags |= 1u;
			pe->damage_state = -1;
		}
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
	world::MatchPlayerIdentity match_player;
	match_player.entity = h;
	match_player.slot = *player_slot;
	match_player.name = resolved_name;
	world.match.upsert_player(match_player);
	// The roster row mirrors the slot's spectator latch (+100567) so the
	// end-round winner award skips a spectator-flagged top scorer
	// [orig: Server_PlayerAdd @0x51CD83].
	world.match.set_player_spectator(h, conn.link.spectator);

	conn.phase = ConnectionPhase::PlayerAdded;
	if (!is_host_own) {
		conn.reply.state6_entry_host_ms =
				ctx.np_protocol.host_run_duration_ms;
		conn.reply.state6_entry_host_ms_valid = true;
	}
	conn.reply.player_slot_reserved = false;
	return h;
}

namespace {

// Server_CountAlivePlayers: the active player-slot rows that carry a bound
// entity/connection [orig: @0x4FD770 — slot byte +4 && dword +28].
uint32_t count_alive_players(const NapiNPServerCtx &ctx) {
	uint32_t count = 0;
	for (const NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (conn.phase >= ConnectionPhase::PlayerAdded &&
				conn.phase < ConnectionPhase::Goodbye &&
				conn.link.owned_entity.valid())
			++count;
	}
	return count;
}

// Server_CountPlayersOnTeam(index): the live players on team index+1, plus the
// pre-spawn reservations already assigned there [orig: @0x501630].
int32_t count_players_on_team(const NapiNPServerCtx &ctx, const world::World &world,
		const NapiNPConnection &joining, uint8_t team) {
	int32_t count = 0;
	world.registry.for_each([&](const world::Entity &e) {
		if (e.handle.pool() != 0 || e.item_id != world::kPlayerInfantryTypeId) return;
		if (e.team == team) ++count;
	});
	for (const NapiNPConnection &c : ctx.np_protocol.connection_list) {
		if (&c == &joining || !c.assigned_team_valid || c.link.owned_entity.valid()) continue;
		if (c.assigned_team == team) ++count;
	}
	return count;
}

// The balance-join hold: with a team-mode BuildFlags nibble and `balance_join`
// set, a joiner who REQUESTED a side (0/1, or one selected by a matching side
// password) is refused while adding it would leave the larger team more than
// max(1, ceil(min(team0, team1) * balance_join_percent)) ahead. An automatic
// request (-1) is never held.
// [orig: CNapiServer_ProcessPendingPlayerSpawns @0x4C8E89 (BuildFlags & 0xF0),
//  side-password override @0x4C8ED1/@0x4C8EEC, gate @0x4C8F0A, counts
//  @0x4C8F19/@0x4C8F1B, ceil @0x4C8F41..0x4C8F53, compare @0x4C8F74]
bool balance_join_admits(const NapiNPServerCtx &ctx, const NapiNPConnection &conn,
		const world::World &world) {
	if ((ctx.np_protocol.build_flags & 0xF0u) == 0) return true;
	int team_id = conn.char_vars.team_request == 0 ? 0
			: conn.char_vars.team_request == 1 ? 1 : -1;
	if (!ctx.config.side_a_password.empty() &&
			opennova::strutil::iequals(conn.join_password.c_str(),
					ctx.config.side_a_password.c_str())) {
		team_id = 0;
	} else if (!ctx.config.side_b_password.empty() &&
			opennova::strutil::iequals(conn.join_password.c_str(),
					ctx.config.side_b_password.c_str())) {
		team_id = 1;
	}
	if (!ctx.config.balance_join || team_id == -1) return true;
	int32_t team0_count = count_players_on_team(ctx, world, conn, 1);
	int32_t team1_count = count_players_on_team(ctx, world, conn, 2);
	const int32_t min_team_count = std::min(team0_count, team1_count);
	int32_t max_team_diff = static_cast<int32_t>(std::ceil(
			static_cast<double>(min_team_count) * ctx.config.balance_join_percent));
	if (max_team_diff < 1) max_team_diff = 1;
	if (team_id != 0) ++team1_count;
	else ++team0_count;
	const int32_t larger_team = std::max(team0_count, team1_count);
	const int32_t smaller_team = std::min(team0_count, team1_count);
	return larger_team - smaller_team <= max_team_diff;
}

} // namespace

// [orig: CNapiServer_ProcessPendingPlayerSpawns @0x4c8dc0] — gated is_authority && !dword_24D1DE0 &&
// !g_spawn_success_gate. dword_24D1DE0 is the mission-LOADING-in-progress flag (set/cleared all over
// Game_StartMission @0x524360); the original does NOT process spawns until the load completes and the
// pool-3 start markers are promoted. [D-NET-116] The reimpl maps the retail
// round-over gate to world::Match's sole outcome latch but has no dword_24D1DE0
// equivalent — acceptable today because callers wire ctx.world AFTER the world
// is loaded with its markers. A production driver that wires
// ctx.world DURING load must add a load-complete gate here, else the spawn resolver finds no marker and
// the idempotent origin fallback below latches the player at (0,0,0) permanently.
// The pump runs once per periodic second from the host tick (the caller's
// gate). Every successful add restarts the walk from the head with the capacity
// predicate re-evaluated (LABEL_5 @0x4C8E02); a pending player the capacity or
// balance-join gate holds is re-nagged with S2C 0x03 at most once per 1000 ms
// (@0x4C8F7A..0x4C8FD1, staged here and framed by tick_connections).
int Server_ProcessPendingPlayerSpawns(NapiNPServerCtx &ctx, world::World &world) {
	if (!ctx.is_authority || world.match.outcome().ended) return 0;
	int spawned = 0;
	const uint32_t host_ms = ctx.np_protocol.host_run_duration_ms;
	bool restart = true;
	while (restart) {
		restart = false;
		const bool has_spawn_slots = count_alive_players(ctx) < ctx.config.max_players;
		for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
			// Spawn an accepted-but-unspawned player: the host loopback (self_id_seen latched at
			// create_session) or a joiner that completed the C2S 0x00 -> 0x01 -> 0x02 admission
			// exchange (and may later restamp its dcb via 0x48). Mid-handshake nodes are skipped.
			if (!conn.self_id_seen) continue;
			if (conn.phase >= ConnectionPhase::PlayerAdded) continue; // already spawned
			// NetPlayer state 4: the NovaWorld ticket result is outstanding, so
			// the player is not yet in the pump's state-6 population.
			// [orig: CNetPlayer_SetGameState(player, 4) @0x4C8BCA, back to 6 only
			//  in HandlePlayEnterResponse @0x4D1C11]
			if (conn.player_enter_pending()) continue;
			if (has_spawn_slots && balance_join_admits(ctx, conn, world)) {
				// Retail unlinks the node before the add, so a failed add (pool
				// full, no AiSystem) leaves the walk; only a successful add
				// restarts it from the head [orig: unlink @0x4C8FF6, goto LABEL_5
				// @0x4C9128].
				if (!Server_BuildPlayerInfoAndAdd(ctx, conn, world).valid()) continue;
				++spawned;
				restart = true;
				break;
			}
			if (conn.type != NapiNPConnection::kTypeServerSide) continue;
			if (!conn.reply.admission_hold_nag_host_ms_valid ||
					host_ms - conn.reply.admission_hold_nag_host_ms > 1000u) {
				conn.reply.admission_hold_nag_pending = true;
				conn.reply.admission_hold_nag_host_ms = host_ms;
				conn.reply.admission_hold_nag_host_ms_valid = true;
			}
		}
	}
	return spawned;
}

bool Server_SetPlayerSpectator(NapiNPServerCtx &ctx, NapiNPConnection &conn,
		world::World &world, bool spectator) {
	if (!ctx.is_authority || conn.phase < ConnectionPhase::PlayerAdded ||
	    !conn.link.owned_entity.valid()) {
		return false;
	}
	world::Entity *player = world.registry.get(conn.link.owned_entity);
	if (player == nullptr) return false;
	if (conn.link.spectator == spectator) return true;

	world::AiEntity *ai =
			world.ai.for_handle(player->handle);
	world.vehicles.detach(player->handle);
	// The deploy-hold bit is untouched on ENTERING spectator mode (retail's
	// runtime conversion @0x519e76 leaves slot+89912 alone — a deployed
	// convert has it clear, an undeployed one keeps holding it); LEAVING runs
	// the deploy below, which clears it like the witnessed deploy leg
	// [orig: Server_ProcessPlayerDeath @0x517791 `and 0xEF`].

	if (spectator) {
		const uint8_t current_team =
				player->team != 0 ? player->team : conn.assigned_team;
		if (current_team != 0) conn.spectator_restore_team = current_team;
		conn.link.spectator = true;
		// [orig: Server_KillPlayerAndNotify @0x519E76 — the runtime convert
		//  writes the same slot latch the roster row mirrors]
		world.match.set_player_spectator(conn.link.owned_entity, true);
		conn.assigned_team = 0;
		conn.assigned_team_valid = true;
		player->team = 0;
		player->flags |= 1u;
		// The witnessed spectator entity state: damage permanently disabled
		// (-1 sentinel), not the 620-tick join countdown.
		// [orig: Server_PlayerAdd @0x51cbc0 — entity+292 = -1]
		player->damage_state = -1;
		if (ai != nullptr) {
			ai->team = 0;
			ai->vel_x = 0;
			ai->vel_z = 0;
			ai->inf.player_moving = false;
			ai->inf.vel[0] = ai->inf.vel[1] = ai->inf.vel[2] = 0;
		}
		return true;
	}

	conn.link.spectator = false;
	world.match.set_player_spectator(conn.link.owned_entity, false);
	conn.link.respawn_pending = false;
	uint8_t team = conn.spectator_restore_team;
	if (team == 0) team = 1;
	conn.assigned_team = team;
	conn.assigned_team_valid = true;
	const world::SpawnPointResult selected = world::resolve_player_spawn_pose(
			world, player->handle, world::EntityHandle{},
			conn.reply.player_slot, team, ctx.config.game_type);
	if (selected.found) {
		player->position = selected.position;
		player->yaw = selected.yaw;
		player->pitch = selected.pitch;
		player->roll = selected.roll;
	}
	// [orig: Server_PositionPlayerForSpawn @0x50D424..0x50D45A]
	world::apply_spawn_point_latches(*player, selected);
	player->team = team;
	player->flags &= ~1u;
	player->flags &= ~world::kEntityFlagDead;
	player->engine_flags &= ~world::kEntityFlagDead;
	player->health = player->health_max > 0 ? player->health_max : 100;
	player->alive = true;
	world::entity_reset_to_spawn_state(*player);
	// The deploy leg's spawn protection: 620 authority ticks for a non-bot slot
	// whose revive latch (+89932) is clear; the reset itself never writes +292.
	// Neither the bot slot nor the medic-revive deploy exists in our roster, so
	// the two 0-stores are unreachable here.
	// [orig: Server_ProcessPlayerDeath @0x517740 — 620 @0x517937/@0x517952/
	//  @0x517960; 0 @0x51790a (bot) / @0x51791c (revive latch)]
	player->damage_state = 620;
	if (ai != nullptr) {
		const int32_t pos[3] = {
				world::to_fixed(player->position.x),
				world::to_fixed(player->position.y),
				world::to_fixed(player->position.z)};
		const int32_t heading =
				world::bam_heading_from_mission_yaw_deg(player->yaw);
		const int16_t health = static_cast<int16_t>(
				std::min<int32_t>(player->health, 32767));
		ai->team = team;
		world::infantry_respawn_snap(*ai, pos, heading, health);
	}
	return true;
}

// See header. Synthetic in-process peer admit (no handshake) — the owner/test hook.
world::EntityHandle admit_synthetic_peer(NapiNPServerCtx &ctx, world::World &world, const PeerAddr &peer,
                                         const world::PlayerSpawn &spawn_in,
                                         replication::ISessionTransport *transport) {
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

	bool created_connection = false;
	if (conn == nullptr) {
		created_connection = true;
		NapiNPConnection c;
		c.peer = peer;
		c.type = 1; // server-side view of a remote client
		c.connection_id = ctx.np_protocol.next_connection_id++;
		c.self_id_seen = true;
		// Period stored only; the countdown arms at the admission dictation
		// (see find_or_create_connection).
		c.s2c_send_holdoff_ticks = clamp_send_holdoff_ticks(
				ctx.config.effective_send_holdoff_ticks());
		// This no-handshake test/adaptor seam synthesizes an already-admitted
		// peer, so model the dictation reset that its skipped admission would own.
		reset_s2c_send_holdoff_counter(c);
		ctx.np_protocol.connection_list.push_back(c);
		conn = &ctx.np_protocol.connection_list.back();
	}
	const std::optional<uint8_t> player_slot = Server_ReservePlayerSlot(
			ctx.np_protocol.connection_list, *conn,
			ctx.config.total_player_slot_capacity());
	if (!player_slot.has_value()) {
		world.registry.despawn(h);
		if (created_connection) ctx.np_protocol.connection_list.pop_back();
		return {};
	}
	conn->assigned_team = spawn.team;
	conn->assigned_team_valid = true;
	conn->link.owned_entity = h;
	conn->link.owned_entity_spawn_id = world.registry.get(h)->registry_spawn_id;
	// [orig: Server_PlayerAdd @0x51CBC0 binds the fresh player allocation]
	conn->link.transport = transport;
	conn->link.mode = replication::TransportMode::Client;
	conn->phase = ConnectionPhase::PlayerAdded;
	conn->burst.spawned = true; // in-match (is_in_match): drained + emitted by Server_TickUpdate
	conn->reply.player_slot = *player_slot;
	conn->reply.player_slot_reserved = false;
	world::MatchPlayerIdentity match_player;
	match_player.entity = h;
	match_player.slot = *player_slot;
	match_player.name = conn->player_name;
	world.match.upsert_player(match_player);
	return h;
}

void Server_ProcessSpectatorRespawnRequests(NapiNPServerCtx &ctx, world::World &world) {
	if (!ctx.is_authority) return;
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (!conn.reply.spectator_convert_pending) continue;
		conn.reply.spectator_convert_pending = false;
		// The handler's own gates repeat inside the kill: authority, a live
		// entity, not already a spectator [orig: @0x519E22..0x519E4B].
		if (!conn.link.owned_entity.valid() || conn.link.spectator) continue;
		world::Entity *player = world.registry.get(conn.link.owned_entity);
		if (player == nullptr) continue;
		// The convert: latch + hide byte + team 0 + Flags bit 0 + damage -1 +
		// health 1 [orig: @0x519E5B..0x519EA4]; Server_SetPlayerSpectator is that
		// same slot/entity mutation.
		if (!Server_SetPlayerSpectator(ctx, conn, world, /*spectator=*/true)) continue;
		player->health = 1;                                       // entity+286 = 1 @0x519EA4
		world.zones.spawn_waves.remove_player(conn.link.owned_entity); // @0x519EB3
		// Server_ProcessPlayerDeath(player, killerHandle) @0x519ECE: the deploy
		// leg, whose spectator arm hides the body; the killer handle only feeds
		// the death record.
		(void)conn.reply.spectator_convert_killer;
		// S2C 0x32 [u8 5][cstr name], mask 128 [orig: @0x519EDC..0x519F43].
		std::vector<uint8_t> body;
		body.push_back(5);
		for (const char ch : conn.reply.player_name) body.push_back(static_cast<uint8_t>(ch));
		body.push_back(0);
		for (NapiNPConnection &c : ctx.np_protocol.connection_list) {
			if (!is_in_match(c) || c.link.transport == nullptr) continue;
			c.link.transport->host_send(s2c::FORMATTED_GAME_TEXT, body);
		}
	}
}

} // namespace opennova::inmatch
