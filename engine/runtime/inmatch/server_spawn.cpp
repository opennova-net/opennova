#include <runtime/inmatch/server_spawn.h>

#include <runtime/inmatch/mission_rotation.h> // HostRotation (LASTGAME, the side-to-team map)
#include <runtime/inmatch/server_chat.h> // broadcast_player_joined_text (the add's 0x32 line)
#include <runtime/inmatch/server_initial_state.h> // Server_OnPlayerJoin (the 0x0B's join)
#include <runtime/inmatch/server_message_dispatch.h> // broadcast_player_sync_on_join (the add's 0x46)

#include <runtime/world/entity.h>
#include <runtime/world/player_spawn.h> // PlayerSpawn, spawn_player / spawn_remote_player
#include <runtime/world/spawn_select.h> // resolve_player_spawn_pose / world_has_spawn_zone
#include <runtime/world/world.h>        // World, registry, cached

#include <base/gameprofile/game_type.h>
#include <base/io/strutil.h>
#include <net/npwire/protocol_message.h> // make_protocol_message (a transport-less join's replies)
#include <net/title/title_identity.h>  // title_in_match (the join's state byte, DFX2-11)
#include <runtime/inmatch/punt_log.h>     // the punt table the round init clears
#include <runtime/inmatch/server_log_recorder.h> // the /PROFILE PDEF of a player add

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
// Retail's misleading g_Team1Name/g_Team2Name symbols are the live SidePasswordA/B strings
// (Game_ApplySessionSettingsToGlobals @0x552043/@0x552054), not a second team-name domain.
// The submitted JSP credential selects a matching protected side before balancing.
uint8_t assign_player_team(const GameConfig &config, bool is_in_session,
		const std::vector<NapiNPConnection> &roster,
		const NapiNPConnection &joining, const world::World &world,
		const std::array<uint8_t, 2> &side_team, const HostSlotRow *host_slot) {
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
	// Each side arm answers with the side's team through the side-to-team map
	// [orig: byte_82F240 @0x4FE456, byte_82F241 @0x4FE492].
	if (side_a_locked && opennova::strutil::iequals(
			config.side_a_password.c_str(), submitted_password.c_str())) return side_team[0];
	if (side_b_locked && opennova::strutil::iequals(
			config.side_b_password.c_str(), submitted_password.c_str())) return side_team[1];
	// Without a match, two-team mode selects an unlocked side or fails.
	// [orig: @0x4FE4AE..0x4FE519 -- side B's team @0x4FE56D, side A's
	//  @0x4FE507]
	if (active_teams == 2) {
		if (side_a_locked && side_b_locked) return 0;
		if (side_a_locked) return side_team[1];
		if (side_b_locked) return side_team[0];
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
		// [orig: side A's team @0x4FE543, side B's @0x4FE56D]
		if (team_request == 0 && !side_a_locked) return side_team[0];
		if (team_request == 1 && !side_b_locked) return side_team[1];
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
	// A Serve Only host's own slot 0 is an active row of the table the tally
	// walks, with no body here to count it (D-NET-352).
	// [orig: @0x4FE5BD..0x4FE5DE: `+4 && slot != joiner`, +416]
	if (host_slot != nullptr && host_slot->active && host_slot->team >= 1 && host_slot->team <= 4)
		++counts[host_slot->team - 1];

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

// Slot 0 is the authority's own row on every host: each round init keeps it
// active and local, the local add takes it (the listen host's loopback), and a
// remote add picks among the inactive rows only, so no joiner ever holds slot
// 0. A Serve Only host builds no local connection and leaves that row
// connectionless, so its first joiner takes slot 1 and its NovaWorld
// PlayerList never names slot 0 (D-NET-350).
// [orig: Server_InitNewRoundState @0x51c99a..0x51ca3c (slot 0 active, local);
//  Server_PlayerAdd @0x51cc31..0x51cc65 (the local add -> g_LocalNetPlayer),
//  @0x51cc9a -> PlayerSlot_PickFreeSlot @0x4fd7d0 (only rows whose +4 active byte is 0)]
std::optional<uint8_t> first_free_player_slot(
		const std::vector<NapiNPConnection> &roster,
		const NapiNPConnection *joining, uint32_t slot_capacity) {
	std::array<bool, 256> occupied{};
	for (const NapiNPConnection &connection : roster) {
		if (&connection == joining ||
		    !connection_claims_player_slot(connection)) continue;
		occupied[connection.reply.player_slot] = true;
	}
	const bool remote = joining != nullptr && joining->type == NapiNPConnection::kTypeServerSide;
	const std::size_t bounded_capacity =
			std::min<std::size_t>(slot_capacity, occupied.size());
	for (std::size_t slot = remote ? 1 : 0; slot < bounded_capacity; ++slot) {
		if (!occupied[slot]) return static_cast<uint8_t>(slot);
	}
	return std::nullopt;
}

// A host with no player profile of its own (opennova-serve keeps none) reads a
// fresh record's: both side class bytes 8, which the session apply keeps in
// 5..9. [orig: PlayerProfile_InitDefaults @0x54BBE0 / @0x54BBE3;
//  Game_ApplySessionSettingsToGlobals @0x5516D0..0x5516FD]
constexpr int32_t kFreshProfileSideClass = 8;

// A Serve Only host's own slot 0 (HostSlotRow): the round init's row, then the
// session create's PlayerSession_InitFromProfile over it. A listen host's slot 0
// is its loopback connection's. The round init's own team assignment
// (Server_AssignPlayerTeam over teamPref) is overwritten by the create's, so
// only the create's rule is kept: the net config's side byte when it is 1 or
// 2, else 1, the slot being no spectator (+100567 is set on a peer only).
// [orig: Server_InitNewRoundState @0x51C99A..0x51CA3C, the team call
//  @0x51CABE..0x51CAC8, the spectator latch @0x51CA53..0x51CA6D;
//  CNapiGameSession_BuildAndCreateSession: Server_InitNewRoundState @0x5695B3
//  ahead of CNapiGameSession_CreateSession @0x56997D; PlayerSession_InitFromProfile
//  @0x50CA92..0x50CAB8 (the team), @0x50CB53 (the name), @0x50CB7C (the class);
//  CNapiGameSession_CreateSession @0x4C9952..0x4C996C (teamPref 0 the side-A
//  class, else the side-B class)]
void init_host_slot_row(NapiNPServerCtx &ctx) {
	ctx.host_slot = HostSlotRow{};
	if (ctx.is_mp_session_peer != 0) return;
	HostSlotRow &host = ctx.host_slot;
	host.active = true;
	// CNapiPlayer_SetName's 32-byte field keeps 31 characters [orig: @0x50CB53].
	host.name = ctx.server_text.host_name.substr(0, 31);
	const int8_t side = ctx.config.host_side_request;
	host.team = side == 1 || side == 2 ? static_cast<uint8_t>(side) : uint8_t{1};
	host.player_class = kFreshProfileSideClass;
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
		const world::World &world, const HostRotation *rotation,
		const HostSlotRow *host_slot) {
	if (!conn.assigned_team_valid) {
		conn.assigned_team =
				assign_player_team(config, is_in_session, roster, conn, world,
						rotation != nullptr ? rotation->side_team
											: std::array<uint8_t, 2>{1, 2},
						host_slot);
		conn.assigned_team_valid = true;
	}
	return conn.assigned_team;
}

// [orig: Server_InitNewRoundState @0x51c8e0] — local-player/round context for an authority host.
void Server_InitNewRoundState(NapiNPServerCtx &ctx) {
	// The team-change list starts every round empty
	// [orig: CBufferList_Free(g_TeamChangeEntityList) @0x51C911..0x51C92A].
	ctx.team_change_entities.clear();
	// banlist.txt: freed, and reloaded on a NovaWorld authority session
	// [orig: Server_ReloadBanListAndResetNetStats @0x51c92f -> BanList_InitFromMission @0x5098f0].
	Server_ReloadPcidBanList(ctx);
	// banned.txt: zeroed and re-read, authority or not
	// [orig: @0x51cb25..0x51cb3b].
	Server_ReloadAddressBanList(ctx);
	// The designation table clears on every round init, authority or not
	// [orig: memset(g_ServerDesignations, 0, 0x1B74) @0x51cb95..0x51cba5].
	ctx.designations.fill(ServerDesignation{});
	// The punt table clears on every round init too [orig: memset(&g_PuntLog,
	//  0, 0x7000) @0x51c990, outside the authority test].
	if (ctx.logs.punt != nullptr) ctx.logs.punt->clear_records();
	// The LASTGAME toggle clears only here, at the session's create and
	// destroy, so it holds across the map changes between them (§5.70.3)
	// [orig: `mov g_LastGameToggle, 0` @0x51CAA4].
	if (ctx.rotation != nullptr) ctx.rotation->last_game = false;
	// The join lock clears with it, so a new session admits joiners; a map change runs no round
	// state init and keeps it [orig: `mov g_ServerJoinLocked, ebx(0)` @0x51CAAA].
	ctx.join_locked = false;
	if (!ctx.is_authority) return;
	init_host_slot_row(ctx);
	// Server_ResetRoundCounters copies the configured StartDelay seconds into the
	// one live pre-round timer. Keep the timer on World: it is the authority
	// phase predicate and the source of the phase-0 0x0A projection, rather
	// than a second connection-local countdown.
	// [orig: Server_ResetRoundCounters @0x516C50, store @0x516C8D]
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
                                                 world::World &world, bool round_init) {
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
			conn, world, ctx.rotation, &ctx.host_slot); // [orig: Server_AssignPlayerTeam @0x4fe310]
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
		// The chosen marker's position words, copied raw (D-NET-507).
		// [orig: Server_PositionPlayerForSpawn @0x50CF60 - the copies
		//  @0x50CFB7..0x50CFE2 / @0x50D3DB..0x50D403]
		spawn.position = sel.position;
		spawn.yaw = sel.yaw;
		spawn.heading_bam = sel.heading_bam; // the copied word (D-NET-376)
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
	// [orig: PlayerClass_InitEntity @0x4B1116 -> AdmDef_FindIndexByName("WPN_M4AUTO")];
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
		// animSlot <- g_AvatarTeam1/2, the packed character/minimap id from the validated
		// profile ids, playerClass <- g_CharClassTeam1/2 (clamped [5,9] at session start).
		// [orig: Player_InitPlayer @0x4e15f0 (@0x4e1843) <- g_AvatarTeam1/2 + g_CharClassTeam1/2
		// <- Game_ApplySessionSettingsToGlobals @0x551500 (class clamp @0x5516ab..0x5516ec, avatar
		// via Avatars_ResolveSelectionIndex (ex sub_57AE60)); ids validated + stored by PlayerSession_InitFromProfile @0x50ca80]
		// Retail's round init spawns the own slot with the slot's class (the session create's
		// side-requirement pick, then its own accepts'), and Player_InitPlayer re-stamps the
		// class word from g_CharClassTeam1/2 by the team, which every weapon-slot list sent to
		// the own slot rewrites with the slot's class; this arm takes the session start's
		// per-side selection, which no own accept or list updates (D-NET-695)
		// [orig: CNapiGameSession_CreateSession @0x4C9952..0x4C996C, the profile init
		//  @0x4C9CCE; Player_InitPlayer @0x4E1809..0x4E1832;
		//  Server_SendWeaponSlotListToPlayer @0x502629..0x50264B].
		spawn.anim_slot = conn.char_vars.avatar[side];
		spawn.minimap_net_id = conn.char_vars.char_id[side];
		const uint8_t cls = conn.char_vars.char_class[side];
		spawn.player_class = (cls >= 5 && cls <= 9) ? cls : 8;
	} else if (round_init) {
		spawn.anim_slot = conn.char_vars.avatar[side];
		spawn.minimap_net_id = conn.char_vars.char_id[side];
		// The round init spawns a kept slot with the slot's class, its last
		// writer's (the add's, an accepted loadout's), and stores none.
		// [orig: Server_InitAllPlayerEntitiesForRound @0x516AA0 - movzx edx,
		//  byte [ebp+15EDCh] @0x516B56, the Entity_SpawnFromAnimSlotProperty call
		//  @0x516B67]
		spawn.player_class = static_cast<uint8_t>(conn.slot_class);
	} else {
		spawn.anim_slot = conn.char_vars.avatar[side];       // raw echo; 0 = tag absent (retail)
		spawn.minimap_net_id = conn.char_vars.char_id[side]; // 0 -> encoder shim
		// playerClass = TR ? CTB : CTA [orig: Server_BuildPlayerInfoAndAdd @0x51d711 buf[52]];
		// absent (0) -> 8 in-session [@0x51d02b]; outside [5,9] -> 8 [@0x51d102]. The joiner's
		// later C2S 0x2F loadout re-stamps it (server_message_dispatch case 0x2F), same as retail.
		// The add stores it as the slot's class [orig: Server_PlayerAdd @0x51D09A, the
		// clamp's store @0x51D10E].
		const uint8_t cls = conn.char_vars.team_request != 0 ? conn.char_vars.char_class[1]
		                                                     : conn.char_vars.char_class[0];
		spawn.player_class = (cls >= 5 && cls <= 9) ? cls : 8;
		conn.slot_class = spawn.player_class;
	}

	// Outside the team game types every player spawns BERSERK, everyone's enemy.
	// [orig: Entity_SpawnFromAnimSlotProperty @0x43C53C..0x43C54F]
	spawn.berserk = (ctx.config.game_type & 0x10000u) == 0;
	const world::EntityHandle h =
			is_host_own ? world::spawn_player(world, spawn) : world::spawn_remote_player(world, spawn);
	if (!h.valid()) return h;
	// The body binds its items.def row at its spawn: a mid-match join's and a
	// map change's kept slot's alike (D-NET-395) [orig: the Server_PlayerAdd
	// call @0x51D7A7 -> its Entity_SpawnFromAnimSlotProperty call @0x51D056 (the
	// round init's @0x516B67) -> the Entity_InitFromModel call @0x43C492].
	if (ctx.bind_spawned_body) ctx.bind_spawned_body(h);
	// The Co-op marker arm's survivors land on the fresh entity: the marker's
	// chute bit and, for a team-2 marker, the queued 0x200 mount onto its
	// carrier (+0x16C/+0x180). [orig: Server_PositionPlayerForSpawn @0x50D424..0x50D45A]
	world::apply_spawn_point_latches(*world.registry.get(h), sel);

	conn.link.owned_entity = h; // the per-connection S2C anchor + C2S owner-verify subject
	world.registry.get(h)->player_slot_id = conn.reply.player_slot; // entity+0x154 (Entity::player_slot_id)
	// The impact damage reads the shooter's slot table, which a kept slot keeps across
	// the round init's new entity: the entity's copy of it (D-NET-508). [orig:
	//  Weapon_CalcImpactDamage @0x4ECA11..0x4ECA24 (Entity_ValidatePtr's slot)]
	world.registry.get(h)->ammo_damage_class.assign(conn.ammo_damage_classes.begin(),
			conn.ammo_damage_classes.end());
	conn.link.owned_entity_spawn_id = world.registry.get(h)->registry_spawn_id;
	conn.link.last_deploy_tick = world.logic_tick;
	conn.link.last_deploy_tick_valid = true;
	// [orig: Server_BuildPlayerInfoAndAdd @0x51D560 binds the newly allocated
	// player row into the recipient slot used by Server_SendEntityStateToPlayer]

	// The join (Server_OnPlayerJoin, server_initial_state.cpp) writes the slot's state byte and
	// the armory cooldown; the add writes neither (D-NET-502) [orig: Server_PlayerAdd
	// @0x51CBC0, its row memset @0x51CD06]. A pending body is hidden from the add on.
	// [orig: NetPacket_WritePlayerState @0x4FF7B3..0x4FF7B8 ORs entity+36 bit0 each frame
	//  while +89912 & 0x10]
	const TitleInMatch &title_rules = title_in_match(ctx.np_protocol.identity);
	if ((world.zones.has_spawn_zone() || title_rules.join_state_fixed) &&
			(!is_host_own || world.rules.mp_session) && !conn.link.spectator) {
		if (world::Entity *pe = world.registry.get(h)) pe->flags |= 1u;
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
	if (conn.link.spectator) {
		// Retail still creates a player entity for a spectator, but leaves it
		// hidden and permanently damage-disabled while S2C 0x75 drives the
		// client's free-fly camera: the spectator leg stores entity+36 |= 1
		// and entity+292 = -1 (no 620-tick countdown — the dead/disabled
		// sentinel). The join's deploy-hold bit (spawn zones) then stays held, so
		// the spectator's 0x0A flags1 reads 0x03 (spectator | deploy-hold) exactly
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
	// wire handle off owned_entity.packed, and the team is the slot's own, assigned_team (D-NET-651).
	// Only the roster ORDER (player_slot) and the echoed name stay on conn.reply.
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
		// The player's name is also its entity's Name (entity+0xF4), which the
		// 0x0C record and the friendly tags read.
		// [orig: Server_PlayerAdd strcpy into +0xF4 @0x51D06B..0x51D082]
		if (world::Entity *e = world.registry.get(h)) {
			e->name = resolved_name;
			e->display_name = resolved_name;
		}
	}
	world::MatchPlayerIdentity match_player;
	match_player.entity = h;
	match_player.slot = *player_slot;
	match_player.name = resolved_name;
	world.match.upsert_player(match_player);
	// The roster row mirrors the slot's spectator latch (+100567) so the
	// end-round winner award skips a spectator-flagged top scorer
	// [orig: Server_PlayerAdd @0x51CD83], and its undeployed bit so event 25
	// skips a pending slot [orig: Server_OnPlayerJoin @0x51A6F2;
	// Server_UpdateCaptureZoneProximity @0x5087A2], and its team byte (+416),
	// which the numbered capture's scorers match (D-NET-650)
	// [orig: CaptureZone_CheckProximityScoring @0x500CEE].
	world.match.set_player_spectator(h, conn.link.spectator);
	world.match.set_player_respawn_pending(h, false); // the join's [orig: Server_PlayerAdd @0x51CD06]
	world.match.set_player_slot_team(h, conn.assigned_team_valid ? conn.assigned_team : uint8_t{0});

	conn.phase = ConnectionPhase::PlayerAdded;
	conn.reply.player_slot_reserved = false;
	// The /PROFILE roster row, late in the add [orig: Server_PlayerAdd
	//  @0x51d2fd..0x51d30f -> CServerLog_WritePlayerNameRecord].
	if (ctx.logs.profile != nullptr) {
		if (const world::Entity *added = world.registry.get(h)) ctx.logs.profile->write_player(*added);
	}
	return h;
}

// The add's PlayerList entry: PlayerTeam the entity's signed team byte,
// PlayerType its class [orig: Server_PlayerAdd: movsx entity+0x162 @0x51D368,
// entity+0x294 @0x51D382, the SetOrCreates @0x51D48F / @0x51D4AA].
void stamp_player_list_add(NapiNPConnection &conn, const world::World &world) {
	const world::Entity *player = world.registry.get(conn.link.owned_entity);
	if (player == nullptr) return;
	conn.player_list_team = static_cast<int8_t>(player->team);
	conn.player_list_type = player->player_class;
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

// See header. [orig: NapiNPServerMsg_PlayerJoinRequest @0x51AB10]
void Server_HandlePlayerJoinRequest(NapiNPServerCtx *ctx, NapiNPConnection &conn,
		world::World *world, uint32_t now_tick, std::vector<ProtocolMessage> &replies) {
	if (ctx == nullptr || world == nullptr || ctx->world != world) return;
	if (!player_slot_active(conn) || conn.burst.game_state == 10) return;
	if (world->registry.get(conn.link.owned_entity) == nullptr) return;
	// [orig: the Server_OnPlayerJoin calls @0x51ABA1 / @0x51ABB5]
	for (InitialStateMessage &m : Server_OnPlayerJoin(*ctx, conn, now_tick)) {
		if (conn.link.transport != nullptr) {
			conn.link.transport->host_send(m.tag, std::move(m.body), m.reliable);
		} else {
			ProtocolMessage message = make_protocol_message(m.tag, std::move(m.body));
			message.reliable = m.reliable;
			replies.push_back(std::move(message));
		}
	}
}

// [orig: CNapiServer_ProcessPendingPlayerSpawns @0x4c8dc0] — gated is_authority && !dword_24D1DE0 &&
// !g_SpawnSuccessGate. dword_24D1DE0 is the mission-LOADING-in-progress flag (set/cleared all over
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
		// Against the session's cap, not the slot limit [orig: `cmp eax,
		// [ecx+0F28h]` @0x4C8E13].
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
				// A remote add's slot memset zeroes its damage-class bytes, which
				// the round init's re-add keeps; the host's own add arm skips the
				// memset, its bytes the slot table's allocation zero, as its fresh
				// loopback connection's are. The memset precedes the add's class
				// store [orig: Server_PlayerAdd @0x51CD06 ahead of @0x51D09A, the
				// local arm @0x51CC33..0x51CC60; Server_AllocatePlayerSlotTable
				// @0x51C1CB].
				if (conn.type == NapiNPConnection::kTypeServerSide)
					conn.ammo_damage_classes.fill(0);
				if (!Server_BuildPlayerInfoAndAdd(ctx, conn, world, /*round_init=*/false).valid())
					continue;
				// The added player's net player enters game state 8 and its slot state 1,
				// where it waits for its C2S 0x09 [orig: CNetPlayer_SetGameState(.., 8)
				// @0x4C911A, CServerTick_SetPhase(slot, 1) @0x4C9123].
				conn.burst.game_state = 8;
				conn.slot_state = PlayerSlotState::Added;
				// The pump's add is Server_PlayerAdd's, which writes the slot's
				// PlayerList entry; the map change's round init re-adds a kept
				// slot through no PlayerAdd and writes none, so its PlayerTeam
				// and PlayerType stay the previous mission's [orig:
				// CNapiServer_ProcessPendingPlayerSpawns -> Server_BuildPlayerInfoAndAdd
				// @0x4C903A -> Server_PlayerAdd @0x51D7A7; Server_InitAllPlayerEntitiesForRound
				// @0x516AA0 -> Entity_SpawnFromAnimSlotProperty @0x516B67, no SetOrCreate].
				stamp_player_list_add(conn, world);
				// The add's join line and its 0x46 push (fieldFlags 0x1CF7) go to every
				// joined client (send mask 0x80, slot states 6 / 7; the added slot itself
				// is not one yet), so their next 0x16's new row is accepted rather than
				// dropped and 0x22-retried (D-NET-158).
				// [orig: Server_PlayerAdd @0x51CBC0 - the 0x32 @0x51D291, the 0x46
				//  @0x51D2BF / @0x51D2CA, ahead of the bot's join @0x51D2E1]
				broadcast_player_joined_text(ctx.np_protocol.connection_list, conn, &world);
				broadcast_player_sync_on_join(ctx.config, ctx.np_protocol.connection_list,
						conn, &world);
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
	world.registry.get(h)->player_slot_id = *player_slot; // entity+0x154 (Entity::player_slot_id)
	// The add's memset zeroes the slot's damage-class bytes, and the impact damage
	// reads the entity's copy (D-NET-508) [orig: Server_PlayerAdd @0x51CD06].
	conn->ammo_damage_classes.fill(0);
	world.registry.get(h)->ammo_damage_class.assign(conn->ammo_damage_classes.begin(),
			conn->ammo_damage_classes.end());
	// [orig: Server_PlayerAdd @0x51CBC0 binds the fresh player allocation]
	conn->link.transport = transport;
	conn->link.mode = replication::TransportMode::Client;
	conn->phase = ConnectionPhase::PlayerAdded;
	conn->burst.spawned = true; // in-match (is_in_match): drained + emitted by Server_TickUpdate
	conn->slot_state = PlayerSlotState::Joined;
	conn->reply.player_slot = *player_slot;
	conn->reply.player_slot_reserved = false;
	// The admitted player's name is its record's and its entity's Name, as on
	// the handshake path. [orig: Server_PlayerAdd strcpy into +0xF4
	//  @0x51D06B..0x51D082]
	if (!conn->player_name.empty()) {
		conn->reply.player_name = conn->player_name;
		if (world::Entity *e = world.registry.get(h)) {
			e->name = conn->player_name;
			e->display_name = conn->player_name;
		}
	}
	world::MatchPlayerIdentity match_player;
	match_player.entity = h;
	match_player.slot = *player_slot;
	match_player.name = conn->player_name;
	world.match.upsert_player(match_player);
	world.match.set_player_slot_team(h, conn->assigned_team); // the slot team mirror (D-NET-650)
	// The add's class is the slot's [orig: Server_PlayerAdd @0x51D09A].
	conn->slot_class = world.registry.get(h)->player_class;
	stamp_player_list_add(*conn, world);
	return h;
}

} // namespace opennova::inmatch
