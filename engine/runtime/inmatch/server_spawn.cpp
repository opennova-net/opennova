#include <runtime/inmatch/server_spawn.h>

#include <runtime/inmatch/join_character_profile.h> // fresh_profile_character_vars (the host row's side bytes)
#include <runtime/inmatch/mission_rotation.h> // HostRotation (LASTGAME, the side-to-team map)
#include <runtime/inmatch/server_chat.h> // broadcast_player_joined_text (the add's 0x32 line)
#include <runtime/inmatch/server_squad_registry.h>
#include <runtime/inmatch/server_initial_state.h> // Server_OnPlayerJoin (the 0x0B's join)
#include <runtime/inmatch/server_session.h> // session_crt_rand (the round init's draws)
#include <runtime/inmatch/server_message_dispatch.h> // broadcast_player_sync_on_join (the add's 0x46)
#include <runtime/inmatch/server_files.h> // server_logs_punt (the join's HMCRC / CMCRC lines)
#include <runtime/inmatch/server_tick.h> // Server_StageHostPunt (the join's checksum punts)

#include <runtime/world/entity.h>
#include <runtime/world/player_spawn.h> // PlayerSpawn, spawn_player / spawn_remote_player
#include <runtime/world/spawn_select.h> // resolve_player_spawn_pose / world_has_spawn_zone
#include <runtime/world/world.h>        // World, registry, cached

#include <base/gameprofile/game_type.h>
#include <base/io/strutil.h>
#include <net/npwire/nw_session_framing.h> // next_connection_index (a synthetic node's dcb)
#include <net/npwire/protocol_message.h> // make_protocol_message (a transport-less join's replies)
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
// `side_password` is the call's side password: a remote add's connection JSP, null for
// the round init's own-slot call.
bool connection_claims_player_slot(const NapiNPConnection &connection);

uint8_t assign_player_team(const GameConfig &config, bool is_in_session,
		const std::vector<NapiNPConnection> &roster,
		const NapiNPConnection &joining,
		const std::array<uint8_t, 2> &side_team, const HostSlotRow *host_slot,
		const std::string *side_password) {
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

	// A null side password (the round init's own-slot call) takes none of the password,
	// team-choose or balance legs: 1 for the side requirement -1, else the requirement's
	// side's team through the side-to-team map. The side requirement is -1 on every host
	// (Config_SetDefaults, the pre.mnu commit; its cfg key is dead), so the own slot's
	// round-init team is 1 there; a requirement past the two sides is unreachable.
	// [orig: Server_AssignPlayerTeam @0x4FE310 - `test edi, edi; jnz` @0x4FE3CA..0x4FE3CC,
	//  `cmp g_SideRequirement, -1` @0x4FE3CE..0x4FE3D6, 1 @0x4FE3DD,
	//  g_RotationSideTeam0[g_SideRequirement] @0x4FE401..0x4FE408]
	if (side_password == nullptr) {
		const int8_t side = config.host_side_request;
		if (side == 0 || side == 1) return side_team[static_cast<std::size_t>(side)];
		return 1;
	}

	const uint8_t active_teams =
			opennova::game_type::active_team_count(gt, config.num_teams);
	// Every side-password read of the pick is the mission start's pair, g_SidePasswordA / B
	// (GameConfig::mission_side_a_password), not game_settings' words the join legs read: a
	// create of no team game clears only game_settings', and an admin SET writes the block and
	// game_settings' at once but reaches this pair only at the next mission start
	// (D-NET-NEW-L1-44's DIVERGENT). [orig: Server_AssignPlayerTeam @0x4FE310 - g_SidePasswordA
	//  @0x4FE41C / @0x4FE434 / @0x4FE4D0 / @0x4FE52E / @0x4FE6AE, g_SidePasswordB @0x4FE464 /
	//  @0x4FE47D / @0x4FE4E9 / @0x4FE553 / @0x4FE6C9; their one writer
	//  Game_ApplySessionSettingsToGlobals @0x552043 / @0x552054]
	const std::string &side_a_password = config.mission_side_a_password;
	const std::string &side_b_password = config.mission_side_b_password;
	const bool side_a_locked = !side_a_password.empty();
	const bool side_b_locked = !side_b_password.empty();

	// The add event carries only the first 16 characters of the submitted JSP
	// (Napi_CopyString(event+80, net_cfg.fid, 17) = 16 chars + NUL), and that
	// truncated copy is what the ordered side compares see. Admission
	// (validate_side_password) compares the untruncated field, as retail's
	// Server_ValidatePlayerJoinRequest does.
	// [orig: Server_BuildPlayerInfoAndAdd @0x51D686 (17-byte copy) ->
	// Server_AssignPlayerTeam @0x4FE43F]
	const std::string submitted_password = side_password->substr(0, 16);
	// Password matches precede the two/four-team split and team preference.
	// If both side passwords match, side A wins the ordered comparison.
	// [orig: Server_AssignPlayerTeam @0x4FE424..0x4FE4AD]
	// Each side arm answers with the side's team through the side-to-team map
	// [orig: g_RotationSideTeam0 @0x4FE456, g_RotationSideTeam1 @0x4FE492].
	if (side_a_locked && opennova::strutil::iequals(
			side_a_password.c_str(), submitted_password.c_str())) return side_team[0];
	if (side_b_locked && opennova::strutil::iequals(
			side_b_password.c_str(), submitted_password.c_str())) return side_team[1];
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
	// Server_PlayerAdd's remote arm overwrites the add event's preference byte with -1
	// BEFORE the assignment runs whenever the cfg block's game type is exactly 0x10000
	// (TDM) and either of the block's side passwords is set. Both are the block's live
	// words, not the mission's game type nor the pick's pair: the block's type is
	// game.cfg's or the host file's (a host screen's mission choice sets the mission's
	// alone), and an admin SET's side password reaches the test at once (D-NET-NEW-L1-46).
	// [orig: Server_PlayerAdd @0x51CBC0 - eax 0 @0x51CBDF, the local add's skip @0x51CC29,
	//  `cmp gameType_3F0, 10000h` @0x51CC6C / jnz @0x51CC76, sideAPassword_383 @0x51CC78,
	//  sideBPassword_394 @0x51CC85, event+97 = -1 @0x51CC91, passed as teamPref @0x51CF24;
	//  assignment @0x4FE51A..0x4FE587]
	uint8_t team_request = joining.char_vars.team_request;
	if (config.cfg_game_type == static_cast<int32_t>(opennova::game_type::kTeamDeathmatch) &&
			(!config.cfg_side_a_password.empty() || !config.cfg_side_b_password.empty()))
		team_request = 0xFF;
	if ((config.mp_attributes & GameConfig::kMpAttribTeamChoose) != 0) {
		// [orig: side A's team @0x4FE543, side B's @0x4FE56D]
		if (team_request == 0 && !side_a_locked) return side_team[0];
		if (team_request == 1 && !side_b_locked) return side_team[1];
	}

	// The tally walks the slot table: every active row but the joiner's, by
	// the row's own team byte (+416), whatever its body or its join state (a
	// loading joiner counts; a session spectator's team 0 counts nowhere;
	// D-NET-NEW-L3-6).
	// [orig: @0x4FE588..0x4FE5EA: `+4 && slot != joiner` @0x4FE5BD, +416
	//  @0x4FE5BF, the four compares @0x4FE5C7..0x4FE5DE]
	std::array<uint32_t, 4> counts{};
	for (const NapiNPConnection &c : roster) {
		if (&c == &joining || !connection_claims_player_slot(c)) continue;
		if (c.assigned_team >= 1 && c.assigned_team <= 4)
			++counts[c.assigned_team - 1];
	}
	// A Serve Only host's own slot 0 is an active row of the table the tally
	// walks, with no connection here to count it (D-NET-352).
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

// Roster slots are stable identities: a row is active while a connection claims it, from the
// add's pick (the reservation it takes before the body spawns) on.
// [orig: the slot's active byte +4, Server_PlayerAdd @0x51CBC0 (stored @0x51CD14)]
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
// PlayerList never names slot 0 (D-NET-350). Of the inactive rows the pick
// takes the one free the longest: the first inactive row is the best until a
// later one's free-tick count is greater, signed, so a tie keeps the lowest
// index; after a leave the next joiner takes the row freed earliest or never
// used, not the one just freed (D-NET-351).
// [orig: Server_InitNewRoundState @0x51c99a..0x51ca3c (slot 0 active, local);
//  Server_PlayerAdd @0x51cc31..0x51cc65 (the local add -> g_LocalNetPlayer),
//  the call @0x51CC95 -> PlayerSlot_PickFreeSlot @0x4FD7B0 - the active byte @0x4FD7D0, the
//  first best @0x4FD7DA, `cmp edx, edi; jle` @0x4FD7E2..0x4FD7EA]
std::optional<uint8_t> first_free_player_slot(
		const std::vector<NapiNPConnection> &roster,
		const NapiNPConnection *joining, uint32_t slot_capacity,
		const PlayerRowFreeTicks *free_ticks,
		const std::vector<OrphanedPlayerRow> *orphaned = nullptr) {
	std::array<bool, 256> occupied{};
	for (const NapiNPConnection &connection : roster) {
		if (&connection == joining ||
		    !connection_claims_player_slot(connection)) continue;
		occupied[connection.reply.player_slot] = true;
	}
	// An orphaned row stays active (D-NET-NEW-L1-24).
	if (orphaned != nullptr)
		for (const OrphanedPlayerRow &orphan : *orphaned)
			occupied[orphan.row.reply.player_slot] = true;
	const bool remote = joining != nullptr && joining->type == NapiNPConnection::kTypeServerSide;
	const std::size_t bounded_capacity =
			std::min<std::size_t>(slot_capacity, occupied.size());
	std::optional<uint8_t> best;
	int32_t best_ticks = 0;
	for (std::size_t slot = remote ? 1 : 0; slot < bounded_capacity; ++slot) {
		if (occupied[slot]) continue;
		const int32_t ticks = free_ticks != nullptr ? (*free_ticks)[slot] : 0;
		if (!best.has_value() || ticks > best_ticks) {
			best = static_cast<uint8_t>(slot);
			best_ticks = ticks;
		}
	}
	return best;
}

// A host with no player profile of its own (opennova-serve keeps none) reads a
// fresh record's: both side class bytes 8, which the session apply keeps in
// 5..9. [orig: PlayerProfile_InitDefaults @0x54BBE0 / @0x54BBE3;
//  Game_ApplySessionSettingsToGlobals @0x5516D0..0x5516FD]
constexpr int32_t kFreshProfileSideClass = 8;

// A Serve Only host's own slot 0 (HostSlotRow): the round init's row, then the
// session create's PlayerSession_InitFromProfile over it. A listen host's slot 0
// is its loopback connection's. The round init's own team assignment
// (Server_AssignPlayerTeam with a null side password) is overwritten by the create's, so
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
	host.char_vars = fresh_profile_character_vars(ctx.characters);
}

} // namespace

void PlayerSession_InitHostRowFromProfile(NapiNPServerCtx &ctx, const CharacterJoinVars &vars) {
	HostSlotRow &host = ctx.host_slot;
	if (!host.active) return;
	host.char_vars = vars;
	PlayerSession_ResolveOwnCharacterIds(ctx, host.char_vars);
	host.player_class = static_cast<int8_t>(ctx.config.host_side_request == 0 ? vars.char_class[0]
	                                                                        : vars.char_class[1]);
}

void PlayerSession_ResolveOwnCharacterIds(const NapiNPServerCtx &ctx, CharacterJoinVars &vars) {
	for (int side = 0; side < 2; ++side)
		if (ctx.characters.find_by_packed_id(vars.char_id[side]) == nullptr)
			vars.char_id[side] = ctx.characters.first_character_id(side);
}

void PlayerSession_InitOwnSlotClass(const NapiNPServerCtx &ctx, NapiNPConnection &own) {
	const uint8_t side_class = ctx.config.host_side_request == 0 ? own.char_vars.char_class[0]
	                                                             : own.char_vars.char_class[1];
	own.slot_class = static_cast<int8_t>(side_class); // movsx of the class byte
}

uint8_t PlayerSession_InitOwnSlotTeam(const NapiNPServerCtx &ctx, NapiNPConnection &own) {
	const int8_t side = ctx.config.host_side_request;
	own.assigned_team = side == 1 || side == 2 ? static_cast<uint8_t>(side)
	                                           : static_cast<uint8_t>(own.link.spectator ? 0 : 1);
	own.assigned_team_valid = true;
	return own.assigned_team;
}

void Player_InitPlayerSideWords(const NapiNPServerCtx &ctx, world::Entity &body) {
	const CharacterJoinVars *words = ctx.host_slot.active ? &ctx.host_slot.char_vars : nullptr;
	for (const NapiNPConnection &conn : ctx.np_protocol.connection_list)
		if (conn.type == NapiNPConnection::kTypeClientSide) words = &conn.char_vars;
	if (words == nullptr) return;
	const int side = body.team == 1 || body.team == 3 ? 0 : 1;
	body.player_class = words->char_class[side];
	body.anim_slot = words->avatar[side];
}

world::EntityHandle Server_SpawnHostSlotBody(NapiNPServerCtx &ctx, world::World &world) {
	HostSlotRow &host = ctx.host_slot;
	host.body = {};
	if (!host.active || ctx.is_authority == 0) return {};
	// An objective or non-team mode has no team 2: the row takes team 1, as every kept
	// slot does [orig: @0x516AFD (0x20000), @0x516B33 (no 0x10000)].
	const uint32_t game_type = ctx.config.game_type;
	if (host.team == 2 && ((game_type & game_type::kObjectiveBit) != 0 ||
			(game_type & game_type::kTeamBit) == 0))
		host.team = 1;
	world::PlayerSpawn spawn;
	spawn.team = host.team;
	// The row's side bytes are the session create's profile init's: the host's player
	// profile's, a fresh record's on a host with no profile of its own
	// (PlayerSession_InitHostRowFromProfile) [orig: PlayerSession_InitFromProfile @0x50CA80 -
	//  +0x1B8 / +0x1BC, +0x15F01..+0x15F03].
	const CharacterJoinVars &vars = host.char_vars;
	const int side = (spawn.team == 1 || spawn.team == 3 || (game_type & 0x10000u) == 0) ? 0 : 1;
	spawn.anim_slot = vars.avatar[side];
	spawn.minimap_net_id = vars.char_id[side];
	spawn.player_class = static_cast<uint8_t>(host.player_class);
	spawn.item_class = spawn.player_class;
	const world::SpawnPointResult sel = world::resolve_player_spawn_pose(
			world, world::EntityHandle{}, world::EntityHandle{}, 0, spawn.team, game_type,
			world::SpawnSlotState{false, 0});
	if (sel.found) {
		spawn.position = sel.position;
		spawn.heading = sel.heading_bam;
		spawn.pitch_bam = sel.pitch_bam; // the marker's words (D-NET-NEW-L6-3)
		spawn.roll_bam = sel.roll_bam;
	}
	spawn.min_entity_slot = kRetailPlayerMinEntitySlot;
	spawn.net_id = 0;
	spawn.owner_connection_id = 0;
	// The process's loadout name list's first name, as every body's (D-NET-143): a dedicated
	// host writes the list never, so its row takes the initializer's WPN_M4AUTO
	// [orig: PlayerClass_InitEntity @0x4B1100..0x4B1116].
	spawn.equipped_adm_index = world::player_spawn_equipped_default(world);
	spawn.berserk = (game_type & 0x10000u) == 0;
	const world::EntityHandle h = world::spawn_player(world, spawn);
	if (!h.valid()) return h;
	if (ctx.bind_spawned_body) ctx.bind_spawned_body(h);
	world::Entity *body = world.registry.get(h);
	if (body == nullptr) return {};
	world::apply_spawn_point_latches(*body, sel);
	// The round init's slot name (the local body's own name is Player_InitPlayer's, at the
	// mission start: HostRole::init_local_player_at_mission_start)
	// [orig: Server_InitAllPlayerEntitiesForRound @0x516AA0 - the copy @0x516BA0..0x516BAF].
	body->name = host.name;
	body->display_name = host.name;
	body->player_slot_id = 0;
	// The round init's 620-tick spawn protection, as every active slot's new body takes it;
	// the protection pass counts only a state-6 slot down, which the host row never is, so
	// its hidden body stays damage-protected for good [orig: Server_InitAllPlayerEntitiesForRound
	//  @0x516BBA; Server_UpdateAllActivePlayerSlots - the state-6 test @0x518874..0x518878;
	//  the row's state, the allocation's zero, set to 6 only by a join @0x51A702..0x51A712].
	body->damage_state = 620;
	// The spawn hides every body it creates [orig: Entity_SpawnFromAnimSlotProperty
	// @0x43C433]; the mission start's local-player block leaves it hidden
	// (HostRole::init_local_player_at_mission_start), and no join ever clears it.
	body->flags |= world::kEntityFlagCarried;
	host.body = h;
	return h;
}

std::optional<uint8_t> Server_ReservePlayerSlot(
		const std::vector<NapiNPConnection> &roster, NapiNPConnection &conn,
		uint32_t slot_capacity, PlayerRowFreeTicks *free_ticks,
		const std::vector<OrphanedPlayerRow> *orphaned) {
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
			first_free_player_slot(roster, &conn, slot_capacity, free_ticks, orphaned);
	if (!slot.has_value()) return std::nullopt;
	conn.reply.player_slot = *slot;
	conn.reply.player_slot_reserved = true;
	if (free_ticks != nullptr) (*free_ticks)[*slot] = 0; // [orig: the add's memset @0x51CD06]
	return slot;
}

void Server_AdvanceFreeRowTicks(NapiNPServerCtx &ctx) {
	std::array<bool, 256> active{};
	active[0] = ctx.host_slot.active;
	for (const NapiNPConnection &connection : ctx.np_protocol.connection_list)
		if (connection_claims_player_slot(connection)) active[connection.reply.player_slot] = true;
	for (const OrphanedPlayerRow &orphan : ctx.orphaned_player_rows)
		active[orphan.row.reply.player_slot] = true;
	const std::size_t rows = std::min<std::size_t>(
			ctx.config.total_player_slot_capacity(), ctx.player_row_free_ticks.size());
	for (std::size_t row = 0; row < rows; ++row) {
		if (active[row]) continue;
		int32_t &ticks = ctx.player_row_free_ticks[row];
		if (ticks < 0x7FFFFFFF) ++ticks;
	}
}

uint8_t Server_ReservePlayerTeam(const GameConfig &config, bool is_in_session,
		const std::vector<NapiNPConnection> &roster, NapiNPConnection &conn,
		const HostRotation *rotation, const HostSlotRow *host_slot) {
	if (!conn.assigned_team_valid) {
		conn.assigned_team =
				assign_player_team(config, is_in_session, roster, conn,
						rotation != nullptr ? rotation->side_team
											: std::array<uint8_t, 2>{1, 2},
						host_slot, &conn.join_password);
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
	// The listing's GameServerBaffleKey, authority or not: (GetTickCount() + rand()) % 900000 +
	// 100000, the rand the session's CRT stream's next draw (after the slot table's seed and
	// first draw), the tick the create's (the session's start tick: the same frame).
	// [orig: Server_InitNewRoundState @0x51C8E0 - GetTickCount @0x51CB69, rand @0x51CB6D, the
	//  unsigned add @0x51CB74, div 0xDBBA0 @0x51CB7B, + 0x186A0 @0x51CB82, g_GameServerBaffleKey
	//  @0x51CB88; after Server_AllocatePlayerSlotTable's srand / rand (from @0x51C97B)]
	// The stream is the session's (session_crt_rand): the process's at a host's create, ahead of
	// its first mission's World.
	io::CrtRand *stream = session_crt_rand(ctx);
	{
		const uint32_t draw = stream != nullptr ? stream->next() : 0u;
		ctx.game_server_baffle_key = (ctx.np_protocol.host_start_tick + draw) % 0xDBBA0u + 0x186A0u;
	}
	// The cookie-key ring starts right after it, authority or not: slot 0 from the same tick and
	// the stream's next draw (the Host list's first PCIDKey, the joiners' cookie key). The
	// server-info refresh's countdown clears with the round, so the first server tick refreshes.
	// [orig: Server_InitNewRoundState @0x51C8E0 - g_ServerInfoUpdateTimer = 0 @0x51CA86,
	//  CSessionIdRing_Init on g_NapiCookieKeyTable (the call @0x51CB8E); CSessionIdRing_Init
	//  @0x4DBC20 - GetTickCount @0x4DBC3D, rand @0x4DBC45, the store @0x4DBC5B]
	ctx.server_info_update_timer = 0;
	ctx.cookie_key_table.init(ctx.np_protocol.host_start_tick,
			static_cast<int>(stream != nullptr ? stream->next() : 0u));
	// The designation table clears on every round init, authority or not
	// [orig: memset(g_ServerDesignations, 0, 0x1B74) @0x51cb95..0x51cba5].
	ctx.designations.fill(ServerDesignation{});
	// Between the memset's pushes, the round init's GetTickCount stamp, the status report's
	// UP-TIME base, authority or not: the create's frame, the start tick (session_uptime_ms)
	// [orig: GetTickCount @0x51CB93, `mov g_RoundStateInitTick, eax` @0x51CBA0].
	ctx.round_state_init_tick = ctx.np_protocol.host_start_tick;
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
	// The table's allocation zeroes every row, the free-tick counts and the orphaned rows'
	// active bytes included [orig: Server_AllocatePlayerSlotTable @0x51C1CB, from @0x51C97B].
	ctx.player_row_free_ticks.fill(0);
	ctx.orphaned_player_rows.clear();
	init_host_slot_row(ctx);
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

// See server_spawn.h. The timer stays on the World: it is the authority's phase predicate and the
// source of the phase-0 0x0A projection, rather than a second connection-local countdown.
// [orig: Server_ResetRoundCounters @0x516C50 - the session test @0x516C66, the authority test
//  @0x516C80, `mov eax, g_StartDelay` @0x516C88, the g_PreRoundDelayTimer store @0x516C8D]
void store_preround_delay_timer(NapiNPServerCtx &ctx) {
	if (ctx.world == nullptr || ctx.is_in_session == 0 || ctx.is_authority == 0) return;
	ctx.world->preround_delay_seconds = ctx.config.start_delay;
}

namespace {

// A remote add's send-state reset, past the slot's report clocks: with the slot's net player
// bound, NapiNPServer_UpdateHoldoffTicks dictates the period again (its two settings updates
// ride the spawn pump's block, build_spawn_pump_metadata) and zeroes the connection's
// countdown, so the slot's send boundary opens on the add's own server tick, ahead of that
// tick's boundary test and send pump, and its phase counts from the add (D-NET-NEW-L3-21). The
// host's own arm jumps past it, and the round init's re-add runs no Server_PlayerAdd.
// [orig: Server_PlayerAdd @0x51CBC0 - slot+28 = the net player @0x51CD4E,
//  CNetPlayer_ResetSendState(slot, 8 or 4) @0x51CD6A / @0x51CD6F; CNetPlayer_ResetSendState
//  @0x500B30 - the net-player test @0x500B3C, NapiNPServer_UpdateHoldoffTicks @0x500B59;
//  NapiNPServer_UpdateHoldoffTicks @0x4C5F40 - CNapiNPConnection_ResetSendHoldoffCounter
//  @0x4C5F8D / @0x4C5FD4; the add's tick: Server_TickUpdate - the receive pump @0x51D895, the
//  spawn pump @0x51DBFD, the boundary test @0x51E3D6, the send pump @0x51E487]
void reset_added_slot_send_holdoff(const NapiNPServerCtx &ctx, NapiNPConnection &conn) {
	conn.s2c_send_holdoff_ticks = clamp_send_holdoff_ticks(ctx.config.effective_send_holdoff_ticks());
	reset_s2c_send_holdoff_counter(conn);
}

} // namespace

namespace {

// The add over `p_world`, which only the host's own arm may meet null (a session create's, ahead
// of its first mission's World: g_LocalPlayerEntity is none there).
// [orig: Server_BuildPlayerInfoAndAdd @0x51d560 -> Server_PlayerAdd @0x51cbc0]
world::EntityHandle build_player_info_and_add(NapiNPServerCtx &ctx, NapiNPConnection &conn,
		world::World *p_world, bool round_init) {
	const std::optional<uint8_t> player_slot =
			Server_ReservePlayerSlot(
					ctx.np_protocol.connection_list, conn,
					ctx.config.total_player_slot_capacity(), &ctx.player_row_free_ticks,
					&ctx.orphaned_player_rows);
	// With no free row retail disconnects the first bot row's player and picks again; JO
	// has no bot rows (the bot byte's one store copies the add event's robot word, which
	// the one add builder zeroes), so it disconnects nobody, logs to the _svrlog ring
	// nothing writes, and the add fails (D-NET-401).
	// [orig: Server_PlayerAdd @0x51CCA0..0x51CCCB -> PlayerSlot_FindFirstBot @0x500A50 (the
	//  bot byte +0x178E3 @0x500A60), its store @0x51CD77; the log @0x51CCCD..0x51CCD2;
	//  ServerLog_BufferAndFlush's g_CreateSvrLogTxt gate @0x4FC0C3..0x4FC0CA]
	if (!player_slot.has_value()) return {};

	// The remote add stamps the row's time in the server ahead of its body, GetTickCount, the
	// process clock (D-NET-NEW-L8-33); the host's own arm and the round init's re-spawn store
	// none [orig: Server_PlayerAdd @0x51CBC0 - GetTickCount @0x51CD8F stored at +0x170B0
	//  @0x51CD95; the own arm @0x51CC24..0x51CC60].
	if (!round_init && conn.type != NapiNPConnection::kTypeClientSide)
		conn.row_added_host_ms = host_tick_count_ms(ctx);

	// The type-2 loopback is the host's OWN client (input-ordered, publishes cached.local_player); a
	// type-1 node is a remote joiner the host snaps from the wire (never the local player). [ADR 0012]
	const bool is_host_own = (conn.type == NapiNPConnection::kTypeClientSide);

	// A joiner's two character ids each name a combo of the host's Avatars.def,
	// else the side's default replaces it: the first combo of the side's
	// alignment, else entry 0, or 0 for an empty table (D-NET-137). The build
	// validates the JSP ids into the add record and the add validates them again
	// (a no-op after the first pass). The host's own add carries none: the local
	// connection's net-flags byte takes the record's empty arm and the add's local
	// branch, so neither pass runs; the own ids were validated by the session's profile
	// init (PlayerSession_ResolveOwnCharacterIds, run by start_host_session).
	// The round init's kept slots were validated at their add.
	// [orig: Server_BuildPlayerInfoAndAdd @0x51D560 - CAvatarDefs_HasPackedId @0x51D6A4,
	//  CAvatarDefs_PackFirstComboOfAlignment(side 0) @0x51D6BA; @0x51D6DD, side 1
	//  @0x51D6F4; the local connection's empty arm @0x51D5C3 / @0x51D5ED, the ids 0
	//  @0x51D61B / @0x51D623; Server_PlayerAdd @0x51CBC0 - the local branch
	//  @0x51CC24..0x51CC60; CAvatarDefs_HasPackedId @0x51CF87,
	//  CAvatarDefs_PackFirstComboOfAlignment(side 0) @0x51CF9D, slot+0x1BA @0x51CFA5;
	//  @0x51CFB9, side 1 @0x51CFD0, slot+0x1BC @0x51CFD8; the own ids:
	//  PlayerSession_InitFromProfile @0x50CA80 - @0x50CAC4 / @0x50CAF1]
	if (!round_init && !is_host_own) {
		for (int side = 0; side < 2; ++side) {
			if (ctx.characters.find_by_packed_id(conn.char_vars.char_id[side]) == nullptr)
				conn.char_vars.char_id[side] = ctx.characters.first_character_id(side);
		}
	}

	world::PlayerSpawn spawn;
	// Team FIRST — a team gametype's start markers are per-team (6096-6099 primary,
	// 6003/6004/6090/6091 fallback), so the AS join spawn needs the assigned team before the
	// §5.2c marker scan; without the split both teams land in team 1's base (net-re §5.61).
	// [orig: Server_AssignPlayerTeam @0x4fe310 runs in Server_PlayerAdd BEFORE
	// Server_PositionPlayerForSpawn's team switch @0x50d266]
	// The listen host's own slot at the session create: its team is the round init's
	// assignment, a null side password's call (no password, team-choose or balance leg: 1,
	// or 0 for a session spectator), from which its add writes its PlayerList entry (the
	// bodyless arm, stamp_player_list_add); then the create's profile init overwrites the
	// slot's team, which the body (the mission start's round init's) takes. A map
	// change's re-add keeps the slot's.
	// [orig: Server_InitNewRoundState @0x51C8E0 - ebx 0 @0x51C93E, Server_AssignPlayerTeam(slot,
	//  0, g_SideRequirement, 0) @0x51CABE..0x51CAC8; Server_PlayerAdd's own arm, the entry
	//  @0x51D398; CNapiGameSession_CreateSession - PlayerSession_InitFromProfile @0x4C9CCE after
	//  the add; Server_InitAllPlayerEntitiesForRound - the body's team @0x516B78..0x516B7E]
	if (is_host_own && !round_init) {
		conn.player_list_team = assign_player_team(ctx.config, ctx.is_in_session,
				ctx.np_protocol.connection_list, conn,
				ctx.rotation != nullptr ? ctx.rotation->side_team : std::array<uint8_t, 2>{1, 2},
				&ctx.host_slot, /*side_password=*/nullptr);
		spawn.team = PlayerSession_InitOwnSlotTeam(ctx, conn);
		// The own arm spawns nothing and places nothing: it stores g_LocalPlayerEntity as the
		// slot's entity, none at a session create (each mission's World starts with none, as
		// retail's teardown zeroes the word), so the row is added bodyless, as the bodyless
		// arm below leaves a row; the profile init then stores the slot's class, and the
		// mission start's round init spawns the body past the PreMission pass
		// (D-NET-NEW-L7-39).
		// [orig: Server_PlayerAdd's own arm - g_LocalNetPlayer @0x51CC33, [slot+0] =
		//  g_LocalPlayerEntity @0x51CC58..0x51CC5D, `jmp loc_51D314` @0x51CC60;
		//  Game_TeardownMission - the memset of g_LocalPlayerEntity @0x522774..0x522780;
		//  CNapiGameSession_CreateSession - PlayerSession_InitFromProfile @0x4C9CCE;
		//  Game_StartMission - the EventTrigger_UpdateAllWithFlag2 call @0x525B86, the
		//  Server_InitAllPlayerEntitiesForRound call @0x525BAF]
		conn.link.owned_entity =
				p_world != nullptr ? p_world->cached.local_player : world::EntityHandle{};
		PlayerSession_InitOwnSlotClass(ctx, conn);
		conn.reply.player_slot = *player_slot;
		const std::string &own_name =
				!conn.player_name.empty() ? conn.player_name : ctx.config.player_name;
		if (!own_name.empty()) conn.reply.player_name = own_name;
		conn.phase = ConnectionPhase::PlayerAdded;
		conn.reply.player_slot_reserved = false;
		return conn.link.owned_entity;
	} else {
		spawn.team = Server_ReservePlayerTeam(
				ctx.config, ctx.is_in_session, ctx.np_protocol.connection_list,
				conn, ctx.rotation, &ctx.host_slot); // [orig: Server_AssignPlayerTeam @0x4fe310]
	}
	world::World &world = *p_world;
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
		spawn.heading = sel.heading_bam; // the copied word (D-NET-376)
		spawn.pitch_bam = sel.pitch_bam;
		spawn.roll_bam = sel.roll_bam;
	} else {
		// No start marker authored: the placement stores nothing, so the body keeps the
		// zeroed entity's words: position 0 and heading word 0 (mission yaw 90). Never an
		// NPC position. [orig: Entity_SpawnFromAnimSlotProperty - the memset @0x43C3FE, the
		//  placement @0x43C459; Entity_FindBestSpawnPoint @0x50CCC0 - no marker, no store
		//  (@0x50CD20)]
		spawn.position = {0.0f, 0.0f, 0.0f};
		spawn.heading = 0;
	}
	spawn.min_entity_slot = kRetailPlayerMinEntitySlot;
	// [D-NET-112] A player carries no SSN (net_id 0) — faithful to the original, which identifies it by
	// handle + ownerConnectionId, not an SSN (see the note above). Keeps players out of the WAC find_by_net_id space.
	spawn.net_id = 0;
	// entity+0x78 = the owning connection's dcb (host loopback dcb / a joiner's ack dcb).
	// [orig: Server_PlayerAdd @0x51cbc0 writes entity+0x78 = conn->connection_id]
	spawn.owner_connection_id = conn.connection_id;
	// Equipped-weapon spawn default = the first name of the process's loadout name list
	// (g_LoadoutWeaponNameList: WPN_M4AUTO until the host's own ACCEPT or its own client's 0x5A
	// writes it), resolved BY NAME like retail; 0xFF (none) when no armory table is fed
	// (unit-test hosts). A joiner's own extended uplink overwrites it on the first drained
	// 0x0C. (D-NET-143) [orig: PlayerClass_InitEntity @0x4B1100..0x4B1116]
	spawn.equipped_adm_index = world::player_spawn_equipped_default(world);

	// Character stamp from the joiner's 0x42 CU vars, picked per ASSIGNED team — side A for teams
	// 1/3 or any non-team-based game type, side B otherwise [orig: Server_PlayerAdd @0x51cbc0
	// @0x51cff7]. The picked avatar byte is the entity+0x374 animSlot [@0x51d0b1] and the picked
	// char id the entity+0x15C wire NetId [slot+440] — the joiner's own 0x0C record must echo the
	// values it uploaded (golden ASH_I5A: VCB=4/CI1=0x8207 -> record 4/0x8207) or its client binds
	// a wrong-type character slot: the D-NET-146 DBuggy1-shadow bug. A joiner's ids were validated
	// against the host's Avatars.def above (D-NET-137) [orig: CAvatarDefs_HasPackedId @0x57b140 ->
	// CAvatarDefs_PackFirstComboOfAlignment @0x57ad40].
	// The pick reads the slot's team byte the assignment just wrote: a session
	// spectator's team 0 takes side B in a team game (D-NET-NEW-L3-7).
	// [orig: Server_PlayerAdd @0x51CFDF..0x51CFF7 - slot+0x1A0 against 1 and 3,
	//  then g_GameType & 0x10000; Server_AssignPlayerTeam's team 0 @0x4FE368]
	const int side = (spawn.team == 1 || spawn.team == 3 ||
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
		// The class word is Player_InitPlayer's re-stamp from g_CharClassTeam1/2 by the
		// team (the mission start runs it on the body: Player_InitPlayerSideWords), which
		// every weapon-slot list sent to the own slot rewrites with the slot's class
		// (weapon_slot_list_own_legs) (D-NET-695)
		// [orig: Player_InitPlayer @0x4E1809..0x4E1832;
		//  Server_SendWeaponSlotListToPlayer @0x502629..0x50264B].
		spawn.anim_slot = conn.char_vars.avatar[side];
		spawn.minimap_net_id = conn.char_vars.char_id[side];
		const uint8_t cls = conn.char_vars.char_class[side];
		spawn.player_class = (cls >= 5 && cls <= 9) ? cls : 8;
		// The body itself is the round init's spawn with the slot's class byte, which
		// binds its camouflage row (D-NET-348): retail's own add attaches the local
		// entity and never spawns, and every mission start's round init spawns the own
		// slot by +0x15EDC, the session create's side class or its last accept's
		// (D-NET-695); Player_InitPlayer then re-stamps the class word alone, above.
		// [orig: Server_PlayerAdd's own arm @0x51CC24..0x51CC60;
		//  Server_InitAllPlayerEntitiesForRound @0x516B56 / @0x516B67]
		spawn.item_class = static_cast<uint8_t>(conn.slot_class);
	} else if (round_init) {
		spawn.anim_slot = conn.char_vars.avatar[side];
		spawn.minimap_net_id = conn.char_vars.char_id[side];
		// The round init spawns a kept slot with the slot's class, its last
		// writer's (the add's, an accepted loadout's), and stores none.
		// [orig: Server_InitAllPlayerEntitiesForRound @0x516AA0 - movzx edx,
		//  byte [ebp+15EDCh] @0x516B56, the Entity_SpawnFromAnimSlotProperty call
		//  @0x516B67]
		spawn.player_class = static_cast<uint8_t>(conn.slot_class);
		spawn.item_class = spawn.player_class; // the same byte binds the camouflage item
	} else {
		spawn.anim_slot = conn.char_vars.avatar[side];       // raw echo; 0 = tag absent (retail)
		spawn.minimap_net_id = conn.char_vars.char_id[side];
		// playerClass = TR ? CTB : CTA [orig: Server_BuildPlayerInfoAndAdd @0x51d711 buf[52]];
		// absent (0) -> 8 in-session [@0x51d02b]; outside [5,9] -> 8 [@0x51d102]. The joiner's
		// later C2S 0x2F loadout re-stamps it (server_message_dispatch case 0x2F), same as retail.
		// The add stores it as the slot's class [orig: Server_PlayerAdd @0x51D09A, the
		// clamp's store @0x51D10E].
		const uint8_t cls = conn.char_vars.team_request != 0 ? conn.char_vars.char_class[1]
		                                                     : conn.char_vars.char_class[0];
		spawn.player_class = (cls >= 5 && cls <= 9) ? cls : 8;
		conn.slot_class = spawn.player_class;
		// The spawn binds the camouflage item of the add's word, ahead of the
		// clamp (a zero word reads 8 in a session) (D-NET-348).
		// [orig: Server_PlayerAdd @0x51D02B..0x51D040, the spawn @0x51D056, the
		//  clamp @0x51D0FA..0x51D10E]
		spawn.item_class = cls != 0 ? cls : static_cast<uint8_t>(ctx.is_in_session != 0 ? 8 : 1);
	}

	// Outside the team game types every player spawns BERSERK, everyone's enemy.
	// [orig: Entity_SpawnFromAnimSlotProperty @0x43C53C..0x43C54F]
	spawn.berserk = (ctx.config.game_type & 0x10000u) == 0;
	const world::EntityHandle h =
			is_host_own ? world::spawn_player(world, spawn) : world::spawn_remote_player(world, spawn);
	if (!h.valid()) {
		// A body that does not spawn (pool 0's rows all taken) leaves the row added and
		// bodyless: only the entity's own stores are skipped, the slot keeps its class,
		// team and name, and the add returns the row; the pump takes its success arm, and
		// the row's 0x0B is dropped while it holds no entity (D-NET-NEW-L1-10). A map
		// change's re-add leaves the kept row bodyless the same way.
		// [orig: Server_PlayerAdd - Entity_SpawnFromAnimSlotProperty @0x51D056, slot+0
		//  @0x51D060, the null skip @0x51D063, the class stores @0x51D09A / @0x51D0A0,
		//  the entity guards @0x51D0A6 / @0x51D0D3 / @0x51D114, the row returned
		//  @0x51D53A; EntityPool_Allocate's pool-0 capacity 0x100 @0x4421CD;
		//  NapiNPServerMsg_PlayerJoinRequest's entity test @0x51AB47]
		conn.link.owned_entity = {};
		conn.reply.player_slot = *player_slot;
		const std::string &bodyless_name =
				!conn.player_name.empty() ? conn.player_name : ctx.config.player_name;
		if (!bodyless_name.empty()) conn.reply.player_name = bodyless_name;
		conn.phase = ConnectionPhase::PlayerAdded;
		conn.reply.player_slot_reserved = false;
		// The remote add's report clocks (D-NET-NEW-L9-22) [orig: Server_PlayerAdd - @0x51CD1B /
		//  @0x51CD26].
		if (!is_host_own && !round_init) gate_slot_clocks_player_added(ctx, conn);
		// Then its send-state reset, ahead of the body spawn too (below, D-NET-NEW-L3-21).
		if (!is_host_own && !round_init) reset_added_slot_send_holdoff(ctx, conn);
		// The remote arm's squad leg runs ahead of the body spawn, so a bodyless
		// row files its node too [orig: @0x51CDE0..0x51CEEF, ahead of @0x51D056].
		if (!is_host_own && !round_init) Server_FileSquadRegistryNode(ctx, conn);
		// The add's high marks on the process report clocks, its body's stores aside
		// (D-NET-NEW-L9-22) [orig: Server_PlayerAdd @0x51D2EE / @0x51D2F8].
		if (!is_host_own && !round_init) gate_clocks_player_added_marks(ctx);
		return h;
	}
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
	// [orig: Server_BuildPlayerInfoAndAdd @0x51D560 binds the newly allocated
	// player row into the recipient slot used by Server_SendEntityStateToPlayer]

	// The join (Server_OnPlayerJoin, server_initial_state.cpp) writes the slot's state byte and
	// the armory cooldown; the add writes neither (D-NET-502) [orig: Server_PlayerAdd
	// @0x51CBC0, its row memset @0x51CD06]. Every body spawned here is hidden: a remote
	// add's until its join (D-NET-503), and the round init's, the host's own included,
	// which the mission start's Player_InitPlayer and local-player block leave hidden until
	// its own join (HostRole::init_local_player_at_mission_start, D-NET-NEW-L7-39)
	// [orig: Entity_SpawnFromAnimSlotProperty @0x43C390 - Flags |= 0x101 @0x43C433 and
	// again @0x43C508].
	if (world::Entity *pe = world.registry.get(h)) pe->flags |= 1u;
	// The round init's re-add stores the 620-tick spawn protection for every active slot;
	// a pump add's body carries none until its join stores it after the 0x0F.
	// [orig: Server_InitAllPlayerEntitiesForRound @0x516AA0 @0x516bba; Server_OnPlayerJoin
	//  @0x51A882]
	if (round_init)
		if (world::Entity *pe = world.registry.get(h)) pe->damage_state = 620;
	// The host's own add takes no spectator leg: its arm attaches the local entity and jumps
	// past the remote arm's (a host that ticked SPECTATE, D-NET-NEW-L4-10: its own join's
	// spectator arm and its slot's 0x0A writer apply theirs) [orig: Server_PlayerAdd - the own
	// arm @0x51CC24..0x51CC60 -> @0x51D314, the remote arm's leg @0x51D0B7..0x51D0ED]. Nor
	// does a map change's round init: its body copies the slot's team word (a spectator's 0)
	// and holds the 620-tick protection above until the slot's first 0x0A restamps it (-1,
	// Health 1, hidden: restamp_spectator_body) [orig: Server_InitAllPlayerEntitiesForRound
	// @0x516AA0 - the team remaps @0x516AF6..0x516B19 / @0x516B2C..0x516B4F (2 -> 1 only), the
	// body's team @0x516B78..0x516B7E, 620 @0x516BBA, no spectator leg;
	// NetPacket_WritePlayerState @0x4FF6FA..0x4FF728].
	if (conn.link.spectator && !is_host_own && !round_init) {
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
	// The round init's re-add keeps the slot's live age (+0x184) from the last mission
	// (NapiNPConnection::kept_play_ticks); a fresh add's row starts at 0 (the row memset).
	// [orig: Server_InitAllPlayerEntitiesForRound @0x516AA0 stores no +0x184; Server_PlayerAdd
	//  @0x51CBC0 - the row memset @0x51CD06]
	if (round_init)
		if (world::MatchPlayer *row = world.match.player(h)) row->play_ticks = conn.kept_play_ticks;
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
	// An added slot is short of state 6 until its join, which the proximity
	// pass, the alive-holder count and the capture census read (D-NET-441); the
	// tick's stamp keeps the row in step after that.
	// [orig: CNapiServer_ProcessPendingPlayerSpawns - PlayerSlot_SetState(slot, 1)
	//  @0x4C9123; Server_OnPlayerJoin's state 6 @0x51A712]
	world.match.set_player_slot_in_game(h, false);

	conn.phase = ConnectionPhase::PlayerAdded;
	conn.reply.player_slot_reserved = false;
	// The remote add constructs the row's two report clocks once it is active; the local arm
	// and the round init's re-add construct none (D-NET-NEW-L9-22) [orig: Server_PlayerAdd - the
	// active byte @0x51CD14, CServerTick_Construct @0x51CD1B / @0x51CD26; the local arm's jump
	// past them @0x51CC60].
	if (!is_host_own && !round_init) gate_slot_clocks_player_added(ctx, conn);
	// Then the slot's send-state reset re-phases its connection's send boundary
	// (reset_added_slot_send_holdoff, D-NET-NEW-L3-21).
	if (!is_host_own && !round_init) reset_added_slot_send_holdoff(ctx, conn);
	// The remote add's squad leg, once the slot is active; the local arm and
	// the round init's re-add file nothing (server_squad_registry.h).
	// [orig: Server_PlayerAdd's remote arm, @0x51CDE0..0x51CEEF; its local arm
	//  @0x51CC33..0x51CC65 runs none of it]
	if (!is_host_own && !round_init) Server_FileSquadRegistryNode(ctx, conn);
	// The /PROFILE roster row, late in a remote add; the local arm jumps past it,
	// so the host's own row is the mission start's roster pass's
	// (HostRole's mission-start end) [orig: Server_PlayerAdd
	//  @0x51d2fd..0x51d30f -> CServerLog_WritePlayerNameRecord; the local arm
	//  @0x51CC29 -> `jmp loc_51D314` @0x51CC60].
	// Ahead of it the add raises the process report clocks' high marks over the slots, its own
	// team counted (D-NET-NEW-L9-22) [orig: Server_PlayerAdd - CServerTick_UpdateMaxPlayerCounts
	//  @0x51D2EE / @0x51D2F8].
	if (!is_host_own && !round_init) gate_clocks_player_added_marks(ctx);
	if (ctx.logs.profile != nullptr && !is_host_own) {
		if (const world::Entity *added = world.registry.get(h)) ctx.logs.profile->write_player(*added);
	}
	return h;
}

} // namespace

world::EntityHandle Server_BuildPlayerInfoAndAdd(NapiNPServerCtx &ctx, NapiNPConnection &conn,
                                                 world::World &world, bool round_init) {
	return build_player_info_and_add(ctx, conn, &world, round_init);
}

// The add's PlayerList entry: PlayerTeam the entity's signed team byte,
// PlayerType its class; with no body, the slot's team byte (+416, unsigned) and
// class (+89820) [orig: Server_PlayerAdd: movsx entity+0x162 @0x51D368,
// entity+0x294 @0x51D382; the bodyless arm's slot+0x1A0 @0x51D398 and
// slot+0x15EDC @0x51D3AF; the SetOrCreates @0x51D48F / @0x51D4AA].
void stamp_player_list_add(NapiNPConnection &conn, const world::World *world) {
	// The listen host's own add finds no entity: its arm stores g_LocalPlayerEntity as
	// the slot's entity, which is null at a session create from the menu (zero at boot,
	// and the last mission's teardown zeroed it; the own body is a mission start's), so it
	// takes the bodyless arm: the slot's team, the round init's, and its class, which the
	// slot table's allocation zeroed and the create's profile init stores only after the
	// add (D-NET-659's residual) [orig: Server_PlayerAdd's own arm - [slot+0] =
	//  g_LocalPlayerEntity @0x51CC58..0x51CC5D; Game_TeardownMission's memset of
	//  g_LocalPlayerEntity @0x522774..0x522780; Server_InitNewRoundState - the slot table
	//  Server_AllocatePlayerSlotTable's memset @0x51C1CB, the team Server_AssignPlayerTeam
	//  @0x51CAC8, the entity 0 @0x51CA2F; CNapiGameSession_CreateSession - the profile
	//  init @0x4C9CCE after the add].
	if (conn.type == NapiNPConnection::kTypeClientSide) {
		// The team the add stored, the round init's, ahead of the profile init's overwrite
		// (PlayerSession_InitOwnSlotTeam).
		conn.player_list_type = 0;
		return;
	}
	const world::Entity *player =
			world != nullptr ? world->registry.get(conn.link.owned_entity) : nullptr;
	if (player == nullptr) {
		conn.player_list_team = conn.assigned_team_valid ? conn.assigned_team : 0;
		conn.player_list_type = conn.slot_class;
		return;
	}
	conn.player_list_team = static_cast<int8_t>(player->team);
	conn.player_list_type = player->player_class;
}

namespace {

// Server_CountAlivePlayers: the active player-slot rows that carry a net
// player, body or not (a bodyless add counts, D-NET-NEW-L1-10) [orig: @0x4FD770
// - the active byte +4 @0x4FD785, the net player +28 @0x4FD78B].
uint32_t count_alive_players(const NapiNPServerCtx &ctx) {
	uint32_t count = 0;
	for (const NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (conn.phase >= ConnectionPhase::PlayerAdded &&
				conn.phase < ConnectionPhase::Goodbye)
			++count;
	}
	// An orphaned row keeps its active byte and its net player (D-NET-NEW-L1-24)
	// [orig: Server_CountAlivePlayers @0x4FD770 - the active byte @0x4FD785, +0x1C @0x4FD78B].
	return count + static_cast<uint32_t>(ctx.orphaned_player_rows.size());
}

// Server_CountPlayersOnTeam(index): the player slots that are active (+4) and
// carry a net player (+28), the rows count_alive_players counts, whose slot
// team byte (+0x1A0, assigned_team) is the rotation's side team of index; no
// entity is read and no pending joiner counts. A Serve Only host's own row
// carries no net player.
// [orig: Server_CountPlayersOnTeam @0x501630 - the active byte `cmp byte ptr
//  [ecx-18h], 0` @0x501650, the net player `cmp dword ptr [ecx], 0` @0x501656,
//  g_RotationSideTeam0[index] @0x50165E, the slot team `cmp bl, [ecx+184h]`
//  @0x501664]
int32_t count_players_on_team(const NapiNPServerCtx &ctx, uint8_t index) {
	const uint8_t team = ctx.rotation != nullptr ? ctx.rotation->side_team[index]
			: std::array<uint8_t, 2>{1, 2}[index];
	int32_t count = 0;
	for (const NapiNPConnection &c : ctx.np_protocol.connection_list) {
		if (c.phase < ConnectionPhase::PlayerAdded || c.phase >= ConnectionPhase::Goodbye)
			continue;
		if ((c.assigned_team_valid ? c.assigned_team : uint8_t{0}) == team) ++count;
	}
	for (const OrphanedPlayerRow &orphan : ctx.orphaned_player_rows) {
		const NapiNPConnection &row = orphan.row; // D-NET-NEW-L1-24
		if ((row.assigned_team_valid ? row.assigned_team : uint8_t{0}) == team) ++count;
	}
	return count;
}

// The balance-join hold: with a team-mode BuildFlags nibble and `balance_join`
// set, a joiner who REQUESTED a side (0/1, or one selected by a matching side
// password) is refused while adding it would leave the larger team more than
// max(1, ceil(min(team0, team1) * balance_join_percent)) ahead. An automatic
// request (-1) is never held.
// The nibble is the live BuildFlags word, a side password in a team game (0x20
// side A, 0x10 side B), not the word the create advertised (D-NET-NEW-L1-38).
// The JSP is compared with side A's password, then side B's, whatever their
// lengths: the guard ahead of the compares tests side A's length twice, so it
// never skips them, and an empty JSP selects the side whose password is unset
// (side A when both are; the nibble needs one set) (D-NET-NEW-L1-38).
// [orig: CNapiServer_ProcessPendingPlayerSpawns @0x4C8E89 (the
//  CNapiServerConfig_BuildFlags call @0x4C8E82, & 0xF0 @0x4C8E87), TR @0x4C8E8F,
//  the side A length twice @0x4C8EB0..0x4C8ECD, _stricmp(JSP, A) @0x4C8ED1 -> 0
//  @0x4C8EDD, _stricmp(JSP, B) @0x4C8EEC -> 1 @0x4C8EF8, gate @0x4C8F0A, counts
//  @0x4C8F19/@0x4C8F1B, ceil @0x4C8F41..0x4C8F53, compare @0x4C8F74;
//  CNapiServerConfig_BuildFlags @0x4C4DC0 - 0x20 @0x4C4E5F, 0x10 @0x4C4E7F]
bool balance_join_admits(const NapiNPServerCtx &ctx, const NapiNPConnection &conn) {
	if ((build_server_config_flags(ctx) & 0xF0u) == 0) return true;
	// TR is the net player's signed byte (-1 automatic). It is only ever 0, 1 or -1: the
	// tag load narrows a joiner's to those three (NapiNPProtocol's TR parse), and the host's
	// own copies the session's byte, the cfg block's side requirement, which holds only -1
	// [orig: `mov bl, [esi+1C0h]` @0x4C8E8F; NapiNetConfig_LoadFromConnTags @0x4C7529..0x4C7537;
	//  NapiNPServer_HandleNewConnection @0x4C817D; CNapiGameSession_BuildAndCreateSession
	//  @0x56979F / @0x5697C3; Config_SetDefaults @0x54D1FA; UI_PreGameMenuStateMachine
	//  @0x568C55].
	int team_id = static_cast<int8_t>(conn.char_vars.team_request);
	if (opennova::strutil::iequals(conn.join_password.c_str(),
				ctx.config.side_a_password.c_str())) {
		team_id = 0;
	} else if (opennova::strutil::iequals(conn.join_password.c_str(),
				ctx.config.side_b_password.c_str())) {
		team_id = 1;
	}
	if (!ctx.config.balance_join || team_id == -1) return true;
	// [orig: `push 0` @0x4C8F10, `push 1` @0x4C8F17]
	int32_t team0_count = count_players_on_team(ctx, 0);
	int32_t team1_count = count_players_on_team(ctx, 1);
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
		world::World *world, uint32_t now_tick, const std::vector<uint8_t> &body,
		std::vector<ProtocolMessage> &replies) {
	if (ctx == nullptr || world == nullptr || ctx->world != world) return;
	if (!player_slot_active(conn) || conn.burst.game_state == 10) return;
	if (world->registry.get(conn.link.owned_entity) == nullptr) return;
	std::size_t at = std::find(body.begin(), body.end(), uint8_t{0}) - body.begin();
	at = std::min(at + 1, body.size()); // past the string's NUL, clamped to the end
	std::array<uint32_t, 2> checksums{0, 0};
	for (uint32_t &checksum : checksums) {
		if (at + 4 > body.size()) break;
		checksum = static_cast<uint32_t>(body[at]) | (static_cast<uint32_t>(body[at + 1]) << 8) |
				(static_cast<uint32_t>(body[at + 2]) << 16) |
				(static_cast<uint32_t>(body[at + 3]) << 24);
		at += 4;
	}
	// [orig: the Server_OnPlayerJoin calls @0x51ABA1 / @0x51ABB5]
	for (InitialStateMessage &m : Server_OnPlayerJoin(*ctx, conn, now_tick, checksums)) {
		if (m.host_punt != 0) {
			// The punt-log line, then the description [orig: Server_OnPlayerJoin @0x51A8E1 ahead of
			//  @0x51A8F2; @0x51A922 ahead of @0x51A933].
			server_logs_punt(ctx->logs, conn, m.host_punt_reason);
			Server_CountCRCMismatchPunt(*ctx, conn, m.host_punt);
			(void)Server_StageHostPunt(conn, m.host_punt);
			continue;
		}
		if (conn.link.transport != nullptr) {
			conn.link.transport->host_send(m.tag, std::move(m.body), m.reliable);
		} else {
			ProtocolMessage message = make_protocol_message(m.tag, std::move(m.body));
			message.reliable = m.reliable;
			replies.push_back(std::move(message));
		}
	}
}

// A second add of an added connection: the connection's current row becomes an orphaned row
// (active, its body in the world, its team counted) and the connection is back to its pre-add
// state, so the add picks a fresh row, memset like any new one: a fresh slot state, initial-state
// cursor, slot team and spawn records. The net player's own words (its game state, its admission
// records) are not the row's and stay (D-NET-NEW-L1-24).
// [orig: Server_PlayerAdd @0x51CBC0 - PlayerSlot_PickFreeSlot (the call @0x51CC95), the row memset
//  @0x51CD06, +0x1C @0x51CD4E, +0xC0 rebound @0x51CD51; the earlier row's +0x1C left pointing at
//  the net player]
static void orphan_player_row(NapiNPServerCtx &ctx, NapiNPConnection &conn) {
	// The row's +0x1C keeps naming this net player whatever its id becomes (net_player_serial).
	if (conn.net_player_serial == 0) conn.net_player_serial = ++ctx.last_net_player_serial;
	OrphanedPlayerRow orphan;
	orphan.row = conn; // the row's words as the re-add found them (OrphanedPlayerRow)
	orphan.row.link.transport = nullptr; // its sends go through its net player (orphan_net_player)
	ctx.orphaned_player_rows.push_back(std::move(orphan));
	conn.link.owned_entity = {};
	conn.phase = ConnectionPhase::Joined;
	const uint8_t game_state = conn.burst.game_state;
	conn.burst = InitialStateBurst{};
	conn.burst.game_state = game_state;
	conn.slot_state = PlayerSlotState::Free;
	conn.assigned_team_valid = false;
	conn.reply.player_slot_reserved = false;
	conn.reply.spawn_metadata_pushed = false;
	// The fresh row's live age (+0x184) is the memset's 0; the orphan keeps its own, so a
	// later round init's re-add of this row restores no age it never ran.
	// [orig: Server_PlayerAdd @0x51CBC0 - the row memset @0x51CD06]
	conn.kept_play_ticks = 0;
}

// See header. A node's place is its order key's rank among the listed nodes: the listing
// appends it at the tail, so its key is one past the largest a listed node holds.
// [orig: NapiNPPlayer_MoveNodeToEntityList @0x4C7E60 - the list test @0x4C7E65, the tail
//  append @0x4C7EB6..0x4C7EE2]
void list_on_spawn_queue(std::vector<NapiNPConnection> &connections, NapiNPConnection &conn) {
	if (conn.self_id_seen && !conn.spawn_queue_unlinked) return; // listed: it keeps its place
	uint32_t tail = 0;
	for (const NapiNPConnection &c : connections)
		if (&c != &conn && c.self_id_seen && !c.spawn_queue_unlinked)
			tail = std::max(tail, c.spawn_queue_order);
	conn.spawn_queue_order = tail + 1;
	conn.self_id_seen = true;
	conn.spawn_queue_unlinked = false;
}

// See header. [orig: NetPacket_WriteSpawnQueueRecord @0x502AC0 - the list test @0x502AC4,
//  [0] @0x502AE7, 1 @0x502AFA, the place NapiNode_CountToHead @0x4C5FE0 (the call @0x502AFD:
//  1 @0x4C5FE9 plus one per node ahead @0x4C5FF0..0x4C5FFD), the count `movzx edx, word ptr
//  [ebp+4Ch]` @0x502B13 over the net player's session (+0, NapiNPPlayer_Create @0x4C7923), whose
//  list sits at +0x40 (NapiNPPlayer_MoveNodeToEntityList @0x4C7E6A, the count +0x4C
//  @0x4C7ED8 / @0x4C7EDE; NapiNode_UnlinkFromList @0x4C6042)]
std::vector<uint8_t> spawn_queue_record(const std::vector<NapiNPConnection> &connections,
		const NapiNPConnection &conn) {
	if (!conn.self_id_seen || conn.spawn_queue_unlinked) return {0x00};
	uint16_t place = 1;
	uint16_t count = 0;
	for (const NapiNPConnection &c : connections) {
		if (!c.self_id_seen || c.spawn_queue_unlinked) continue;
		++count;
		if (&c != &conn && c.spawn_queue_order < conn.spawn_queue_order) ++place;
	}
	return {0x01, static_cast<uint8_t>(place), static_cast<uint8_t>(place >> 8),
			static_cast<uint8_t>(count), static_cast<uint8_t>(count >> 8)};
}

// [orig: CNapiServer_ProcessPendingPlayerSpawns @0x4c8dc0] — gated is_authority && !g_NetSpawnSuspended
// && !g_RoundOver. The round-over gate is world::Match's sole outcome latch. g_NetSpawnSuspended
// (NapiNPServerCtx::net_spawn_suspended) is held for the length of a mission load, and this pump
// never reads it set: like the timeout walk it runs only from Server_TickUpdate, and the quality
// sample only from Game_ProcessMainFrame, neither of which runs inside the load (ours runs no host
// tick while the session is Loading). Its other three readers do run inside the load: a session
// host's load pumps its server protocol (HostRole::pump_load), so the 0x29, 0x32 and 0x28 handlers
// read the gate (D-NET-116).
// [orig: Game_StartMission @0x524360 (the set @0x524398, the clear @0x52631B, its
//  Server_PumpNetworkTransport calls @0x5245E8 .. @0x525AC5); Game_ShowErrorLogAndAbortMission
//  @0x520AA0 (the clear @0x520B6C); the readers: this pump @0x4C8DDE (its one caller
//  Server_TickUpdate @0x51DBFD), CNapiNetwork_CheckPlayerTimeouts @0x4C8AD0 (@0x4C8B1B; Server_TickUpdate
//  @0x51DBF3), CNetQuality_UpdateMetrics @0x4C52C0 (@0x4C52EA / @0x4C5606; Game_ProcessMainFrame
//  @0x52658B), NapiNPServerMsg_0x029 @0x514F10 (@0x514F1E), Server_CollectValidWeaponSlots @0x516000
//  (@0x516005), NapiNPServerMsg_SendEmptySlots @0x51A600 (@0x51A609), the last three through
//  CNapiNetwork_PumpServerProtocolRecv @0x4C4EE0 (Server_TickUpdate @0x51D895 and
//  Server_PumpNetworkTransport @0x4FD981)]
// The pump runs once per periodic second from the host tick (the caller's
// gate). Every successful add restarts the walk from the head with the capacity
// predicate re-evaluated (LABEL_5 @0x4C8E02); a pending player the capacity or
// balance-join gate holds is re-nagged with S2C 0x03 at most once per 1000 ms
// (@0x4C8F7A..0x4C8FD1, staged here and framed by tick_connections).
int Server_ProcessPendingPlayerSpawns(NapiNPServerCtx &ctx, world::World &world) {
	return Server_ProcessPendingPlayerSpawns(ctx, &world);
}

int Server_ProcessPendingPlayerSpawns(NapiNPServerCtx &ctx, world::World *world) {
	if (!ctx.is_authority || (world != nullptr && world->match.outcome().ended)) return 0;
	int spawned = 0;
	// The pump's one GetTickCount, the process clock the re-nag's 1000 ms test and its stamp
	// read, so a host load counts (D-NET-NEW-L8-36) [orig: GetTickCount @0x4C8DF8, kept
	//  @0x4C8DFE; the test @0x4C8F81..0x4C8F87, the stamp @0x4C8FCD / @0x4C8FD1].
	const uint32_t host_ms = host_tick_count_ms(ctx);
	bool restart = true;
	while (restart) {
		restart = false;
		// Against the session's cap, not the slot limit [orig: `cmp eax,
		// [ecx+0F28h]` @0x4C8E13].
		const bool has_spawn_slots = count_alive_players(ctx) < ctx.config.max_players;
		for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
			// The pending list's nodes alone: the host loopback (listed at create_session) and
			// each node a C2S 0x02 listed (complete_admission), off the list once the pump
			// unlinked it and back on it at a repeated 0x02. The walk tests nothing else: no game
			// state (a 0x02 in the NovaWorld ticket wait has left it) and no earlier add.
			// [orig: CNapiServer_ProcessPendingPlayerSpawns @0x4C8DC0 - conn+0x24 == 1, the net
			//  player @0x4C8E5A, the list membership @0x4C8E68..0x4C8E6C]
			if (!conn.self_id_seen || conn.spawn_queue_unlinked) continue;
			// With no World bound (a session create's own add) only the host's own node is added;
			// any other waits for the World-bound pump.
			if (world == nullptr && conn.type != NapiNPConnection::kTypeClientSide) continue;
			if (has_spawn_slots && balance_join_admits(ctx, conn)) {
				// Retail unlinks the node and sends its 0x03 before the add, so a
				// failed add (no free row) leaves a joiner with no row, no body and
				// no further message, which the walk never visits again; either
				// way the walk restarts from the head (D-NET-715).
				// [orig: NapiNode_UnlinkFromList @0x4C8FF6, the 0x03 @0x4C9034, the
				//  add @0x4C903A, the success test @0x4C904E, goto LABEL_5 @0x4C9128]
				conn.spawn_queue_unlinked = true;
				// A node added before (a repeated 0x02 listed it again) is added again: the
				// add picks it a fresh row and rebinds its server slot there, the earlier row
				// left active with its body (D-NET-NEW-L1-24).
				// [orig: Server_BuildPlayerInfoAndAdd @0x51D560 -> Server_PlayerAdd's
				//  PlayerSlot_PickFreeSlot (the call @0x51CC95), +0xC0 @0x51CD51]
				if (conn.phase >= ConnectionPhase::PlayerAdded) orphan_player_row(ctx, conn);
				// A remote add's slot memset zeroes its damage-class bytes, which
				// the round init's re-add keeps; the host's own add arm skips the
				// memset, its bytes the slot table's allocation zero, as its fresh
				// loopback connection's are. The memset precedes the add's class
				// store [orig: Server_PlayerAdd @0x51CD06 ahead of @0x51D09A, the
				// local arm @0x51CC33..0x51CC60; Server_AllocatePlayerSlotTable
				// @0x51C1CB].
				if (conn.type == NapiNPConnection::kTypeServerSide)
					conn.ammo_damage_classes.fill(0);
				// The add fails only with no free row; a bodyless add is a success
				// (D-NET-NEW-L1-10) [orig: the success test @0x4C904E on the row].
				(void)build_player_info_and_add(ctx, conn, world, /*round_init=*/false);
				if (conn.phase < ConnectionPhase::PlayerAdded) {
					// The unlink's 0x03 is all it gets (framed by tick_connections, the
					// held-joiner 0x03's writer): the unlinked node's [0].
					if (conn.type == NapiNPConnection::kTypeServerSide) {
						conn.reply.admission_hold_nag_pending = true;
						conn.reply.admission_hold_nag_body =
								spawn_queue_record(ctx.np_protocol.connection_list, conn);
					}
					restart = true;
					break;
				}
				// The added player's net player enters game state 8 and its slot state 1,
				// where it waits for its C2S 0x09 [orig: CNetPlayer_SetGameState(.., 8)
				// @0x4C911A, PlayerSlot_SetState(slot, 1) @0x4C9123].
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
				// A squad's first slot announces its squadron first
				// [orig: @0x51D182..0x51D20E, ahead of the join line @0x51D21E].
				Server_AnnounceSquadronEntered(ctx, conn);
				broadcast_player_joined_text(ctx.np_protocol.connection_list, conn);
				broadcast_player_sync_on_join(ctx.config, ctx.np_protocol.connection_list,
						conn, world);
				// The added player's own records queue here, at the add: the one-byte 0x03,
				// the send-state reset's settings, the squad add, then the 0x05, the 0x04 and
				// the 0x7B, so a later send to the slot (a scoreboard boundary's 0x16 before
				// its next open send boundary) follows them (D-NET-504's early 0x22: ours
				// queued them at that boundary, behind such a 0x16). A connection with no
				// transport keeps tick_connections' framed leg.
				// [orig: CNapiServer_ProcessPendingPlayerSpawns - the 0x03 @0x4C9034, the add
				//  @0x4C903A, 0x05 @0x4C909D, 0x04 @0x4C90D7, 0x7B @0x4C9111;
				//  NapiNPServer_SendToConn @0x4C4F20 -> CNapiNPConnection_QueueMessage @0x628640]
				if (conn.type == NapiNPConnection::kTypeServerSide &&
						conn.link.transport != nullptr && conn.reply.admission_metadata_pushed &&
						!conn.reply.spawn_metadata_pushed) {
					for (ProtocolMessage &m : build_spawn_pump_metadata(ctx.config,
							ctx.mission_text_title, ctx.session_title, conn,
							ctx.np_protocol.connection_list, world,
							ctx.rotation, &ctx.host_slot))
						conn.link.transport->host_send(m.tag, std::move(m.payload), m.reliable,
								m.flags.raw, m.capacity_exempt, m.retention_flushes);
					conn.reply.spawn_metadata_pushed = true;
				}
				++spawned;
				restart = true;
				break;
			}
			if (conn.type != NapiNPConnection::kTypeServerSide) continue;
			if (!conn.reply.admission_hold_nag_host_ms_valid ||
					host_ms - conn.reply.admission_hold_nag_host_ms > 1000u) {
				// The held node is still listed: its place and the list's count
				// (D-NET-NEW-L1-38) [orig: NetPacket_WriteSpawnQueueRecord
				//  called @0x4C8FAA, the 0x03 @0x4C8FC8].
				conn.reply.admission_hold_nag_pending = true;
				conn.reply.admission_hold_nag_body =
						spawn_queue_record(ctx.np_protocol.connection_list, conn);
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
	// A new node's dcb is the create's index from the process's connection counter, as a
	// join's is (D-NET-713) [orig: CNapiNPConnection_Create @0x62ACB0 - @0x62B282..0x62B296].
	const uint32_t conn_id = conn ? conn->connection_id : next_connection_index();

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
		c.connection_id = conn_id;
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
			ctx.config.total_player_slot_capacity(), &ctx.player_row_free_ticks);
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
	conn->spawn_queue_unlinked = true; // added as the pump adds: off the pending list
	conn->burst.spawned = true; // in-match (is_in_match): drained + emitted by Server_TickUpdate
	conn->slot_state = PlayerSlotState::Joined;
	// Armed as the join's state-6 entry arms it [orig: Server_OnPlayerJoin @0x51A74C].
	conn->link.round_watermark = world.out.rounds.frame_clock - 1;
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
	stamp_player_list_add(*conn, &world);
	return h;
}

} // namespace opennova::inmatch
