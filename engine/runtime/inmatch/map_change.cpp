#include <runtime/inmatch/map_change.h>

#include <base/gameprofile/game_type.h>
#include <net/npwire/ingame_message_id.h>
#include <runtime/inmatch/host_config.h>
#include <runtime/inmatch/host_role.h>
#include <runtime/inmatch/mission_rotation.h>
#include <runtime/inmatch/server_message_dispatch.h> // build_tag7b_session_summary
#include <runtime/inmatch/server_spawn.h>            // Server_BuildPlayerInfoAndAdd
#include <runtime/inmatch/server_team_change.h>      // the auto-balance pair
#include <runtime/mission/mission_kernel.h>
#include <runtime/inmatch/session_transport.h>

#include <algorithm>
#include <utility>
#include <vector>

namespace opennova::inmatch {

namespace {

// An active slot: a connection whose player was added (retail's slot +4).
bool slot_active(const NapiNPConnection &conn) {
	return conn.phase >= ConnectionPhase::PlayerAdded;
}

// The teardown's Attack-and-Defend arm, on an in-session authority with the
// launch option set and the Post Menu pending. A SETNEXT latch at a first
// half's end skips the swap: the half toggle is set and the latch cleared,
// so the router clears the toggle and advances onto the SETNEXT entry.
// Otherwise every live slot's team swaps 1 <-> 2 (its side's avatar and spawn
// selection follow the team at its next spawn), the side-to-team map swaps,
// and a set latch then sets the toggle and clears itself. The slot's pending
// team byte (+100568), which the swap takes first when nonzero, has no
// writer in the port.
// [orig: Game_TeardownMission @0x522435..0x522520 -- the latch test
//  @0x522448..0x522457, the slot walk @0x52245D..0x5224F4 (live: +4 and the
//  entity pointer @0x522470..0x522479; the pending team @0x52247B..0x522485;
//  1 <-> 2 @0x52248B..0x5224AA; the side's avatar +89857 and spawn word +440
//  @0x5224B7..0x5224DF), the map swap @0x522501..0x522512, the toggle and the
//  latch @0x522519..0x522520]
void swap_sides_or_latch(NapiNPServerCtx &ctx, HostRotation &rotation) {
	if (!rotation.setnext_latch || rotation.is_flipped) {
		for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
			if (!slot_active(conn) || !conn.link.owned_entity.valid() || !conn.assigned_team_valid)
				continue;
			if (conn.assigned_team == 1)
				conn.assigned_team = 2;
			else if (conn.assigned_team == 2)
				conn.assigned_team = 1;
		}
		std::swap(rotation.side_team[0], rotation.side_team[1]);
		if (!rotation.setnext_latch) return;
	}
	rotation.is_flipped = true;
	rotation.setnext_latch = false;
}

// A kept slot's per-mission state, back to a fresh mission's, around the
// identity the map change keeps: the connection's keys, sequencing, clocks,
// admission and team; the link's transport, its spectator latch and the
// client's medic preference; the reply state's name, roster slot, ping
// history, team-change stamp and integrity salts, and the latches that make
// the admission's 0x04 / 0x7B boundary and the join announcement one-shot.
// The burst starts over at the kept slot's next C2S 0x37 (the 0x16 push),
// as a stock joiner's reload asks for it.
// [orig: Server_DisconnectAndResetAllPlayerSlots @0x516160 -- the entity
//  pointer, +360..+368, +376, +380, +460, +89856, +89864, +89868, +89932,
//  +94372..+94380 cleared @0x5163B2..0x516417, the stats block re-inited
//  @0x5163F5..0x516403, +332 @0x516405, the sync phases @0x51641D..0x516432;
//  the slot's identity survives (net-re §5.70.6)]
void reset_slot_for_next_mission(NapiNPConnection &conn) {
	replication::Connection link;
	link.transport = conn.link.transport;
	link.mode = conn.link.mode;
	link.send_mask = conn.link.send_mask;
	link.spectator = conn.link.spectator;
	link.auto_medic_enabled = conn.link.auto_medic_enabled;
	conn.link = std::move(link);

	SessionReplyState reply;
	const SessionReplyState &kept = conn.reply;
	reply.rtt_ms = kept.rtt_ms;
	reply.rtt_ring = kept.rtt_ring;
	reply.min_ping_strikes = kept.min_ping_strikes;
	reply.max_ping_strikes = kept.max_ping_strikes;
	reply.client_quality = kept.client_quality;
	reply.client_quality_dirty = kept.client_quality_dirty;
	reply.team_change_ms = kept.team_change_ms;
	reply.integrity_weapon_crc_salt = kept.integrity_weapon_crc_salt;
	reply.integrity_ammo_crc_salt = kept.integrity_ammo_crc_salt;
	reply.admission_metadata_pushed = kept.admission_metadata_pushed;
	reply.spawn_metadata_pushed = kept.spawn_metadata_pushed;
	reply.admission_completed_tick = kept.admission_completed_tick;
	reply.roster_counted = kept.roster_counted;
	reply.roster_seen_gen = kept.roster_seen_gen;
	reply.player_name = kept.player_name;
	reply.player_slot = kept.player_slot;
	conn.reply = std::move(reply);

	conn.score = 0;
	conn.score_wire_mirror = 0;
	conn.tick_seed = 0;
	conn.fire_tick_floor = 0;
	conn.fire_disarmed_at = 0;
	conn.fire_tick_mode = false;
	conn.phase = ConnectionPhase::PlayerAdded; // the slot stays active; its entity comes back
	conn.discard_pre_deploy_uplinks = false;
	conn.staged_frame_update.clear();
	conn.frame_update_staged = false;
	conn.pending_guidance.clear();
	conn.world_stream_announced = false;
	conn.spawned_announced = false;
	conn.burst = InitialStateBurst{};
	conn.weapon_slots.clear();
	conn.equipped_slot = NapiNPConnection::EquippedSlotRef{};
}

// [orig: Server_DisconnectAndResetAllPlayerSlots @0x516160 -- for every
//  active slot in states 2..7 the S2C 0x25 (mask 0x20, that slot, empty
//  body; the six `push 25h` sends @0x5161BB..0x516385), the net player to
//  game state 8 and the slot to state 1; then every active slot's
//  per-mission state]
void reset_all_player_slots(HostRole &role) {
	NapiNPServerCtx &ctx = role.state.host_owner.ctx;
	HostRotation *rotation = role.rotation();
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (!slot_active(conn)) continue;
		if (conn.burst.sync_state >= 2 && conn.link.transport != nullptr) {
			if (conn.type == NapiNPConnection::kTypeServerSide) {
				conn.link.transport->host_send(s2c::GAME_RESET, std::vector<uint8_t>{},
						/*reliable=*/false);
				// The slot's staged C2S belonged to the old mission; a slot out of
				// state 6 has no handler for them.
				replication::Datagram stale;
				while (conn.link.transport->host_recv(stale)) {}
			} else if (rotation != nullptr) {
				// The host's own client takes its 0x25 through the loopback, where
				// the authority arm only counts the round; its client view is
				// rebuilt with the next mission.
				// [orig: NapiNPClientMsg_GameReset @0x422855]
				++rotation->round_count;
			}
		}
		reset_slot_for_next_mission(conn);
	}
}

} // namespace

MapChangeStep begin_host_map_change(HostRole &role,
		const std::vector<mission_catalog::Row> &catalog, gamecfg::GameCfg *cfg_block,
		HostScreenState *session) {
	HostOwner &owner = role.state.host_owner;
	NapiNPServerCtx &ctx = owner.ctx;
	HostRotation *rotation = role.rotation();
	mission::MissionKernel *kernel = role.kernel();
	const bool authority_in_session = ctx.is_in_session != 0 && ctx.is_authority != 0;

	// The Game Loop's shutdown with the Post Menu pending: the swap arm, then
	// the per-mission teardown (pools 0..2, the authority's PostMission
	// sweep), then the slot reset.
	// [orig: Game_TeardownMission @0x522413..0x52241D (the Post Menu test),
	//  the entities @0x52236F..0x5223C0, EventTrigger_UpdateAllWithFlag4
	//  @0x52266C, Server_DisconnectAndResetAllPlayerSlots @0x52269B]
	if (authority_in_session && rotation != nullptr && rotation->list.map_launch_option != 0)
		swap_sides_or_latch(ctx, *rotation);
	if (kernel != nullptr) {
		kernel->run_post_mission_pass(/*is_authority=*/true);
		role.map_change.crt_rand = kernel->world.crt_rand;
	}
	role.map_change.torn_down = true;
	reset_all_player_slots(role);
	// The old World goes with its kernel; nothing reads it past here.
	ctx.world = nullptr;
	ctx.mission = nullptr;

	// The Post Menu's router on an in-session authority: a launch-option map
	// whose half toggle is clear replays the same entry with the toggle set;
	// with the toggle set it clears it and advances; without the option it
	// advances and clears the toggle after. A next mission pushes the PreMenu
	// at state 7 (PRE_CONNECTED -> the Game Loop); none ends the session.
	// [orig: PostMenu_RouteMissionExit @0x5685C2..0x56864F -- the option test
	//  @0x5685D8, the replay @0x568607, the cleared toggle and the advance
	//  @0x5685EA..0x568600, the advance and the cleared toggle
	//  @0x568610..0x568624, g_PreMenuState = 7 and the PreMenu push
	//  @0x568632..0x568641, the miss @0x56864F]
	if (!authority_in_session || rotation == nullptr) return MapChangeStep::RotationEnded;
	HostRotation &r = *rotation;
	const bool replay = ctx.config.replay_enabled != 0;
	bool next = false;
	if (r.list.map_launch_option != 0 && !r.is_flipped) {
		r.is_flipped = true;
		next = true;
	} else if (r.list.map_launch_option != 0) {
		r.is_flipped = false;
		next = r.list.advance(catalog, replay);
	} else {
		next = r.list.advance(catalog, replay);
		r.is_flipped = false;
	}
	if (!next) return MapChangeStep::RotationEnded;
	// The next mission's start re-applies the cfg block to the session.
	if (cfg_block != nullptr && session != nullptr) *session = host_session_settings(*cfg_block);
	role.map_change.pending = true;
	// The 0x25s leave at the next load's first pumps, ahead of the streams'
	// rebuild [orig: Game_StartMission -> Server_PumpNetworkTransport
	// @0x5245E8; the teardown's own pump @0x522588 precedes the reset].
	role.pump_load();
	return MapChangeStep::NextMission;
}

void init_all_player_entities_for_round(HostRole &role) {
	NapiNPServerCtx &ctx = role.state.host_owner.ctx;
	if (ctx.is_authority == 0 || ctx.world == nullptr) return;
	world::World &world = *ctx.world;
	// Auto-balance first, while the previous-mode word still names the
	// previous mission; then the word takes this one.
	// [orig: Server_InitAllPlayerEntitiesForRound @0x516AAD..0x516AB6 (the
	//  Server_ShouldAutoBalance / Server_AutoBalanceTeams pair), dword_24D212C
	//  @0x516AD5]
	if (Server_ShouldAutoBalance(ctx)) Server_AutoBalanceTeams(ctx, &world);
	if (ctx.rotation != nullptr) ctx.rotation->previous_game_type = ctx.config.game_type;
	std::vector<NapiNPConnection *> slots;
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list)
		if (slot_active(conn) && !conn.link.owned_entity.valid()) slots.push_back(&conn);
	std::stable_sort(slots.begin(), slots.end(), [](const NapiNPConnection *a,
			const NapiNPConnection *b) { return a->reply.player_slot < b->reply.player_slot; });
	const uint32_t game_type = ctx.config.game_type;
	for (NapiNPConnection *conn : slots) {
		// An objective mode or a non-team mode has no team 2: such a slot takes
		// team 1 with its side-1 selections.
		// [orig: @0x516AFD (0x20000), @0x516B33 (no 0x10000)]
		if (conn->assigned_team_valid && conn->assigned_team == 2 &&
				((game_type & game_type::kObjectiveBit) != 0 ||
						(game_type & game_type::kTeamBit) == 0))
			conn->assigned_team = 1;
		// The slot's new entity [orig: Entity_SpawnFromAnimSlotProperty
		// @0x516B67 and the slot's stamps @0x516B6C..0x516C20].
		(void)Server_BuildPlayerInfoAndAdd(ctx, *conn, world);
	}
	// Every kept slot names a new entity: the roster every in-match client
	// holds is stale.
	++ctx.np_protocol.roster_generation;
}

void Server_BroadcastPlayerInfoToAll(NapiNPServerCtx &ctx) {
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (!slot_active(conn) || conn.link.transport == nullptr) continue;
		conn.link.transport->host_send(s2c::FULL_PLAYER_INFO,
				build_tag7b_session_summary(ctx.config, conn), /*reliable=*/true);
	}
}

} // namespace opennova::inmatch
