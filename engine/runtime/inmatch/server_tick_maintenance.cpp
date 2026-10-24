#include <runtime/inmatch/server_tick_maintenance.h>
#include <runtime/inmatch/server_tick.h>
#include <runtime/inmatch/server_message_dispatch.h> // build_player_list_message
#include <runtime/inmatch/server_dispatch_join.h> // admission_padding_probe
#include <runtime/inmatch/server_overlay_state.h>     // the 14-tick minimap overlay visits
#include <runtime/inmatch/server_player_death.h>     // the flag reset's Health store (kill_player_body_at_frame_tail)

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <base/gameprofile/game_type.h>        // the CTF / FlagBall / Flag Me carry-limit modes
#include <base/io/le.h>                        // append_u32_le
#include <base/io/strutil.h>                   // iequals (the JOINTICKET key lookup)
#include <net/napi/session.h>                   // SESSION_HOST_INFO_REFRESH_TICKS
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_hello.h>
#include <runtime/world/ai.h>                  // the remote player's body (kit weight)
#include <runtime/world/world.h>
#include <runtime/world/geom.h>                // to_fixed
#include <runtime/world/local_player.h>        // the listen host's live inventory (kit weight)
#include <runtime/world/weapon_inventory.h>    // weapon_inventory_loadout_weight_fp16

namespace opennova::inmatch {

namespace {

constexpr uint32_t kPuntCharattrSilence = 16;
constexpr uint32_t kPuntTimeSyncSilence = 24;
constexpr uint32_t kPuntDeadTooLong = 7;
constexpr uint32_t kPuntJoinDeployIdle = 35;
constexpr uint32_t kPuntSilenceLimit = 8;
// Periodic seconds, compared strictly-greater after the increment
// [orig: Server_TickUpdate @0x51E066..0x51E07D / @0x51E187].
constexpr uint32_t kDeadLiveSecondLimit = 360;
constexpr uint32_t kJoinDeployIdleLimitMs = 360000;
constexpr uint8_t kDescriptionFlags =
		PROTOCOL_MSG_FLAG_SETTINGS_UPDATE | PROTOCOL_MSG_FLAG_LEN8;
static_assert(kDescriptionFlags == 0xA0);

// The first description event owns the connection's disconnect slot: retail's
// SendChatMessage keeps the first pending event and sends none once the
// description went [orig: CNapiNPConnection_QueueDisconnectEvent @0x4C7EF0, the
// sent byte conn+0x70D @0x4C7F58..0x4C7F5F, the pending slot conn+0x654
// @0x4C7FF2..0x4C8004]. The description's sent byte then closes the connection's
// queue: every later message to it returns unqueued, so nothing follows the record
// (D-NET-NEW-L1-27) [orig: CNapiNPConnection_SendDescriptionOnce @0x629750 - the
// store @0x629772; CNapiNPConnection_QueueMessage @0x628640 - its test @0x62864D].
bool stage_host_disconnect(
		NapiNPConnection &conn, const DisconnectEvent &event) {
	if (conn.host_disconnect_sent || conn.type != NapiNPConnection::kTypeServerSide ||
			conn.link.transport == nullptr)
		return false;
	conn.link.transport->host_send(
			hightag::DESCRIPTION_PACKET,
			connection_description_to_bytes(event),
			/*reliable=*/true, kDescriptionFlags,
			/*capacity_exempt=*/true);
	conn.link.transport->close_host_queue();
	conn.host_disconnect_sent = true;
	return true;
}

// Server_LogCRCMismatchPunt's numeric tN family.
// [orig: @0x517ED0 -> NapiNPDataTransfer_SendDescription @0x628C80]
bool stage_host_punt(NapiNPConnection &conn, uint32_t mismatch_type) {
	DisconnectEvent event;
	event.ds = 1;
	event.dc = 2;
	event.dstr = "t" + std::to_string(mismatch_type);
	event.dpc = 33;
	event.ddstr = "LogPuntEvent";
	if (!stage_host_disconnect(conn, event)) return false;
	conn.host_disconnect_mismatch_type = mismatch_type;
	return true;
}

} // namespace

// The original stores seconds, not tick deadlines. The per-slot decrements sit
// inside the same g_PeriodicSecondTimer block as the win check and the
// capture transaction, so they ride Match's countdown, not a second phase.
// [orig: Server_TickUpdate @0x51DFB0..0x51E00B (slot +0x170/+0x168/+0x16C)]
void tick_respawn_holds(NapiNPServerCtx &ctx, const world::World &world) {
	if (!world.match.periodic_second()) return;
	// Every active row's, an orphaned row's too (D-NET-NEW-L1-24): the row's own words
	// [orig: Server_TickUpdate - the active byte @0x51DFB0, the timers @0x51DFBA..0x51E05C].
	std::vector<NapiNPConnection *> rows;
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) rows.push_back(&conn);
	for (OrphanedPlayerRow &orphan : ctx.orphaned_player_rows) rows.push_back(&orphan.row);
	for (NapiNPConnection *row : rows) {
		NapiNPConnection &conn = *row;
		// The +356 armory-reuse cooldown rides the same per-slot pass: a positive
		// value counts down, a negative one clamps to zero.
		// [orig: Server_TickUpdate @0x51e00b..0x51e022]
		if (conn.link.armory_reuse_seconds > 0)
			--conn.link.armory_reuse_seconds;
		else if (conn.link.armory_reuse_seconds < 0)
			conn.link.armory_reuse_seconds = 0;
		if (conn.link.respawn_delay_seconds != 0)
			--conn.link.respawn_delay_seconds;
		if (conn.link.spawn_target_hold_seconds != 0)
			--conn.link.spawn_target_hold_seconds;
		if (conn.link.downed_revive_seconds != 0)
			--conn.link.downed_revive_seconds;
		// The +376 emote and +380 radio-call cooldowns: a positive value counts
		// down, a negative one clamps to zero [orig: @0x51e028..0x51e05c].
		for (int32_t *cooldown : {&conn.link.emote_cooldown_seconds,
				&conn.link.radio_call_cooldown_seconds})
			*cooldown = *cooldown > 0 ? *cooldown - 1 : 0;
	}
}

namespace {

// The 620-tick spawn-protection arm of the per-player maintenance walk: for
// every active state-6 slot with an entity, while the round is live and no
// pre-round timer runs — a live MP session holds a spectator (the +97538
// latch our spectator bit stands in for, D-NET-217) at -1 and counts a positive
// entity+292 down by one; outside a session (SP) it is zeroed every tick, so SP
// never carries protection. Runs before the receive pump, so a deploy admitted
// this tick keeps its full 620.
// [orig: Server_UpdateAllActivePlayerSlots @0x518820 — gate @0x51888c
//  (!g_RoundOver && !g_PreRoundDelayTimer), non-session zero
//  @0x51889c, latch -1 @0x5188ac, decrement @0x5188b8..0x5188c5; caller
//  Server_TickUpdate @0x51d88b ahead of the recv pump @0x51d895]
void tick_spawn_protection(world::World &world, NapiNPConnection &conn, bool in_match) {
	if (world.match.outcome().ended || world.preround_delay_seconds != 0) return;
	if (!in_match || !conn.link.owned_entity.valid()) return;
	world::Entity *player = world.registry.get(conn.link.owned_entity);
	if (player == nullptr) return;
	if (world.rules.mp_session) {
		if (conn.link.spectator)
			player->damage_state = -1;
		else if (player->damage_state > 0)
			--player->damage_state;
	} else {
		player->damage_state = 0;
	}
}

// The frontier-hint arm of the same walk: a state-6 slot whose bit 0x04 is set
// and which has played 1240 ticks gets S2C 0x1E event 58 [its slot team's frontier
// zone][the enemy team's, 0 when the same][0xFF] (mask 0xA0, itself) unless its own
// frontier is 0; either way the bit clears. Both read the slot team (D-NET-583).
// [orig: Server_UpdateAllActivePlayerSlots @0x518820 — @0x51890C..0x5189A6, @0x518929..0x51894A]
void emit_frontier_hint(world::World &world, NapiNPConnection &conn, bool in_match) {
	if (!in_match || !conn.reply.frontier_hint_pending) return;
	const world::Entity *player = world.registry.get(conn.link.owned_entity);
	const world::MatchPlayer *slot = world.match.player(conn.link.owned_entity);
	if (player == nullptr || slot == nullptr || slot->play_ticks < 1240u) return;
	const uint8_t own = world.zones.frontier_zone(conn.assigned_team);
	uint8_t enemy = world.zones.frontier_zone(conn.assigned_team == 1 ? 2 : 1);
	if (enemy == own) enemy = 0;
	if (own != 0 && conn.link.transport != nullptr)
		conn.link.transport->host_send(s2c::GAME_EVENT,
				{0x3A, own, enemy, 0xFF, 0, 0, 0, 0});
	conn.reply.frontier_hint_pending = false;
}

void run_control_quartet(NapiNPServerCtx &ctx, world::World &world, NapiNPConnection &conn,
		NapiNPConnection *net_player, bool in_match);

} // namespace

// The per-player maintenance walk at the tick head, ahead of the receive pump:
// per slot, the spawn-protection arm, the minimap overlay visit, the frontier
// hint, then the control quartet. All four sit in the walk's state-6 block, and
// the round end
// moves every slot to state 7, which the match outcome stands for here, so
// none runs through the end-round linger. The walk is the slot table's, in
// slot order: a visit's carrier pass can flush its ring to another slot, so
// the order shows on that slot's wire.
// [orig: Server_UpdateAllActivePlayerSlots @0x518820 - the slot-index walk
//  @0x518824..0x518A81, the state test @0x518878, the protection
//  @0x5188AC..0x5188C5, the overlay visit @0x5188D6, the frontier hint
//  @0x518929, the quartet @0x5189AD..0x518A79; called @0x51D88B;
//  Server_ProcessRoundEnd's state 7 @0x51685E]
void update_all_active_player_slots(NapiNPServerCtx &ctx, world::World &world) {
	if (world.match.outcome().ended) return;
	// Every active row, an orphaned one too (D-NET-NEW-L1-24): its gates, cooldowns, counts
	// and draw are the row's own words (its state word, body, team, live age, spectator latch,
	// the overlay cooldown and phase, the frontier bit, the quartet's countdown and counts), so
	// they step past its net player's leave too; its sends go to that net player (send_mask
	// 0x20 / 0xA0 send through the row's +0x1C) and its punts punt it, none past the leave
	// (Q7-36 (a)).
	// [orig: Server_UpdateAllActivePlayerSlots @0x518820 - the active byte @0x518840, +0x15F28
	//  @0x518849, the state test @0x518874..0x518878, the frontier hint's send_mask 0xA0 and
	//  the row @0x518974 / @0x51897E, the quartet's punts Server_LogCRCMismatchPunt(slot)
	//  @0x518A13 / @0x518A3A (the row's +0x1C @0x517F47); Server_SendPendingOverlayState's
	//  send_mask 0x20 @0x50FE4E; NapiNPServer_SendFiltered's 0x20 arm, the row's +0x1C
	//  @0x4C8A1F]
	struct SlotRow {
		NapiNPConnection *row;
		NapiNPConnection *net_player;
	};
	std::vector<SlotRow> slots;
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) slots.push_back({&conn, &conn});
	for (OrphanedPlayerRow &orphan : ctx.orphaned_player_rows) {
		NapiNPConnection *net_player = orphan_net_player(ctx, orphan);
		// The 0x20 arm sends to a server-side net player; a described one's queue drops it.
		if (net_player == nullptr || net_player->type != NapiNPConnection::kTypeServerSide ||
				net_player->host_disconnect_sent)
			orphan.row.link.transport = nullptr;
		slots.push_back({&orphan.row, net_player});
	}
	std::stable_sort(slots.begin(), slots.end(), [](const SlotRow &a, const SlotRow &b) {
		return a.row->reply.player_slot < b.row->reply.player_slot;
	});
	for (const SlotRow &slot : slots) {
		NapiNPConnection &conn = *slot.row;
		const bool own_row = slot.net_player == &conn;
		// The row's state word: a punted row (its description gone, its teardown not yet run)
		// walks on, its sends dropped by its closed queue (D-NET-652).
		const bool in_match = slot_in_match(conn);
		const bool visited = own_row ? in_match && conn.link.transport != nullptr : in_match;
		tick_spawn_protection(world, conn, in_match);
		emit_minimap_overlay_for_slot(ctx, world, conn, visited);
		emit_frontier_hint(world, conn, in_match);
		run_control_quartet(ctx, world, conn, slot.net_player, visited);
	}
}

// The 1 Hz violation sweep, in session only, per active state-6 slot with an
// entity. (a) In CTF / FlagBall / Flag Me the slot counts consecutive seconds
// its entity's mountedChild is a 4091/4093/4095 flag (else the counter resets);
// at the host's carry limit the counter resets, the carried object is dropped
// beside the carrier (its 0x2F drop state), the flag is re-synced to its
// authored pose (a second feed-less 0x2F), and the carrier's Health is set to
// -1 so the ordinary death transaction kills it — no 0x1E return event, no
// scoring. The compare is signed, so a limit <= 0 fires every second for every
// slot exactly as retail would. (b) Every remote slot (the host's own slot byte
// +5 and bots +96483 are excluded; bots do not exist here): with the
// MaxFriendlyKills limit L, `L < 0 || teamKills <= L` routes to the suicide arm
// (`suicides > 9` -> punt t6, the suicide arm logging "#S>9" to _PUNT.TXT
// first), else the team-kill arm punts t6 with no log line. The global
// punts-off switch dword_B4C698 (`/NOPUNT`) is not modeled (no port host sets
// it). The slot +89896 latch the gates also test has no writer in the image,
// and the slot gates are the slot's alone (active, entity, state 6), so a
// punted slot still in state 6 is walked again each second: its suicide arm
// writes "#S>9" again while the description stays the first
// (stage_host_disconnect, SendChatMessage's first-event-wins), and a round
// over (state 7) walks nobody.
// [orig: Server_CheckPlayerViolations @0x51ABD0 — in-session @0x51abd9, slot
//  gates @0x51abf6/@0x51abff/@0x51ac0d, game types @0x51ac2c, counter
//  @0x51ac5a/@0x51ac63, limit @0x51ac75, drop/sync/health @0x51ac84/@0x51ac8a/
//  @0x51ac94, slot +5 @0x51ac9d / +96483 @0x51aca2, TK gate @0x51acc2, suicide
//  compare @0x51aced/@0x51acf0 (/NOPUNT @0x51ad00), punts @0x51ad17/@0x51acd9; the sole caller is
//  the periodic-second block of Server_TickUpdate @0x51df5f, behind the win check
//  @0x51df5a, whose round end puts every slot in state 7 first]
void check_player_violations(NapiNPServerCtx &ctx, world::World &world) {
	if (!ctx.is_in_session) return;
	const uint32_t gt = world.match.rules().game_type;
	const bool carry_limited = gt == game_type::kCaptureTheFlag ||
			gt == game_type::kFlagBall || gt == game_type::kFlagMe;
	// The slot table in slot order, an orphaned row too (D-NET-NEW-L1-24): its carry count,
	// its body's drop and death, its stats and its punt-log line (the row's +0x14 and +0x28)
	// are its own; its punt punts its net player (Server_LogCRCMismatchPunt describes the
	// row's +0x1C), none past the leave (Q7-36 (a)).
	// [orig: Server_CheckPlayerViolations @0x51ABD0 - the slot walk @0x51ABE0..0x51AD2E;
	//  Server_WritePuntLog @0x4F9C70 - the row's +0x28 @0x4F9CA2 (the name copy), +0x14 /
	//  +0x28 @0x4F9CCF / @0x4F9CD2 (the line's slot and name); Server_LogCRCMismatchPunt
	//  @0x517ED0 - the row's +0x1C @0x517F47]
	struct SlotRow {
		NapiNPConnection *row;
		NapiNPConnection *net_player;
	};
	std::vector<SlotRow> slots;
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) slots.push_back({&conn, &conn});
	for (OrphanedPlayerRow &orphan : ctx.orphaned_player_rows)
		slots.push_back({&orphan.row, orphan_net_player(ctx, orphan)});
	std::stable_sort(slots.begin(), slots.end(), [](const SlotRow &a, const SlotRow &b) {
		return a.row->reply.player_slot < b.row->reply.player_slot;
	});
	for (const SlotRow &slot : slots) {
		NapiNPConnection &conn = *slot.row;
		// Every row in state 6 alone, an orphaned row too: its state word is its own (copied
		// at the orphaning, stepped by its track and by the round end), so from the round end
		// (state 7) the sweep takes no row, through the linger's periodic seconds too.
		// [orig: Server_CheckPlayerViolations - `cmp dword ptr [esi+20h], 6` @0x51AC09, jnz
		//  @0x51AC0D]
		if (!player_slot_active(conn) || !conn.link.owned_entity.valid() ||
				conn.slot_state != PlayerSlotState::Joined)
			continue;
		world::Entity *player = world.registry.get(conn.link.owned_entity);
		if (player == nullptr) continue;
		if (carry_limited) {
			const world::Entity *child = world.registry.get(player->mounted_child);
			const bool carrying = child != nullptr && child->has_item_def &&
					(child->item_id == 4093 || child->item_id == 4091 ||
					 child->item_id == 4095);
			if (carrying)
				++conn.link.flag_carry_seconds;
			else
				conn.link.flag_carry_seconds = 0;
			if (static_cast<int32_t>(conn.link.flag_carry_seconds) >=
					ctx.config.flag_reset_seconds) {
				conn.link.flag_carry_seconds = 0;
				const world::EntityHandle flag = player->mounted_child;
				world.match.drop_carried_object(world, player->handle);
				world.match.sync_flag_to_authored_pose(world, flag);
				kill_player_body_at_frame_tail(world, player);
			}
		}
		if (conn.link.mode == replication::TransportMode::Loopback) continue;
		const world::MatchPlayer *stats = world.match.player(player->handle);
		if (stats == nullptr) continue;
		const int32_t limit = ctx.config.max_friendly_kills;
		NapiNPConnection *punted = slot.net_player;
		if (limit < 0 || stats->stats[world::MatchStats::kTeamKills] <= limit) {
			if (stats->stats[world::MatchStats::kSuicides] > 9) {
				server_logs_punt(ctx.logs, conn, "#S>9"); // [orig: @0x51ad0d]
				if (punted != nullptr) stage_host_punt(*punted, 6);
			}
		} else if (punted != nullptr) {
			stage_host_punt(*punted, 6);
		}
	}
}

// The 1 Hz round-robin flag-state refresh: walk pool 1 in slot order counting
// live-def entities whose ItemDef id is 4091/4093/4095; the one whose running
// index equals the cursor is re-broadcast through the same 19-B 0x2F sender
// the event path uses (mask 0x80: every state-6/7 slot, listen host included),
// the cursor advances, and the walk stops. A walk that sends nothing resets
// the cursor, so with N flags every flag's state is re-sent once per N+1
// seconds. Authority-gated only: not on is_in_session, the round-over latch or
// the pre-round timer, and the alive/dead bits are never tested.
// [orig: Server_RefreshNextFlagState @0x517B20 — authority @0x517b27, def @0x517b55, ids
//  @0x517b57..0x517b6d, cursor compare @0x517b71, ++ @0x517b81, reset
//  @0x517b90; sender Server_SendDestructibleDeathPacket @0x50D900 (mask 0x80
//  @0x50d922, SendFiltered(0x2F) @0x50d95a) -> NetPacket_SerializeEntityWithParentAndTarget
//  @0x505810; caller Server_TickUpdate @0x51df64]
void refresh_next_flag_state(NapiNPServerCtx &ctx, world::World &world) {
	if (!ctx.is_authority) return;
	uint32_t index = 0;
	bool sent = false;
	world.registry.for_each_in_pool(1, [&](const world::Entity &e) {
		if (sent || !e.has_item_def) return;
		if (e.item_id != 4091 && e.item_id != 4093 && e.item_id != 4095) return;
		if (index != world.flag_refresh_cursor) {
			++index;
			return;
		}
		ObjectiveEntityState state;
		state.entity_handle = e.handle.packed;
		state.flags_byte = static_cast<uint8_t>(e.flags);
		state.pos_x = e.position.x.q16;
		state.pos_y = e.position.y.q16;
		state.pos_z = e.position.z.q16;
		state.attach_handle = e.primary_occupant.packed;
		state.ground_handle = e.ground_target.packed;
		const std::vector<uint8_t> body = encode_objective_entity_state(state);
		for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
			if (!active_player_recipient(conn)) continue;
			conn.link.transport->host_send(s2c::OBJECTIVE_ENTITY_STATE, body);
		}
		++world.flag_refresh_cursor;
		sent = true;
	});
	if (!sent) world.flag_refresh_cursor = 0;
}

namespace {

uint16_t next_maintenance_prng16(world::World &world) {
	// Server_SendEntityHandleAndInputState @0x507B90 lazily consumes the
	// process-global PRNG_Next16/dword_31BFBB0 stream. World owns that stream
	// directly so AI, recoil, throwable bounce, and network control cannot fork
	// their call histories when no AiSystem happens to be installed.
	return world.next_prng16();
}

// The integrity family's slot gate: an active remote slot (not the host's own,
// +5; the bot byte +0x178E3 is written only as 0 in JO) in state 6 with a live
// entity. The gate reads the slot's own words alone, so a punted slot (its
// description gone, its teardown not yet run) still takes its draw, the request
// dropped by its closed queue (D-NET-652).
// [orig: Server_BroadcastWeaponSlotStateToPlayers @0x508540 - active @0x508562,
//  +5 @0x50856B..0x50856E, +0x178E3 @0x508574, state 6 @0x508580, the entity
//  @0x50858A, the draw PRNG_Next16 called @0x5085AE; the bot byte's one store
//  Server_PlayerAdd @0x51CD77 copies the add event's +4, zeroed by
//  Server_BuildPlayerInfoAndAdd @0x51D5E9]
const world::Entity *integrity_player(
		const NapiNPConnection &conn, const world::World &world) {
	if (!slot_in_match(conn) || slot_is_local(conn) || conn.link.transport == nullptr)
		return nullptr;
	const world::Entity *player = world.registry.get(conn.link.owned_entity);
	// The integrity slot walk tests only entity+36 bit 0x02. Health and the
	// reimpl convenience `alive` latch may be stale without suppressing retail.
	// [orig: Server_BroadcastWeaponSlotStateToPlayers @0x50858A..0x508598]
	if (player == nullptr ||
			((player->flags | player->engine_flags) & world::kEntityFlagDead) != 0)
		return nullptr;
	return player;
}

// The same gate over an orphaned row's own words: its state word, its +5 and its body (its
// net player is read only by the send).
const world::Entity *orphan_integrity_player(
		const NapiNPConnection &row, const world::World &world) {
	if (!slot_in_match(row) || slot_is_local(row)) return nullptr;
	const world::Entity *player = world.registry.get(row.link.owned_entity);
	if (player == nullptr ||
			((player->flags | player->engine_flags) & world::kEntityFlagDead) != 0)
		return nullptr;
	return player;
}

bool scoreboard_recipient(const NapiNPConnection &conn) {
	// The 0x16 send walk uses the broader active-player mask 0x20. It does not
	// reuse either integrity's state-6/entity gate or quality's state-6/7
	// filter: a player whose slot is already installed may receive the periodic
	// list while its initial-state stream is still in progress. PlayerAdded is
	// the point where this runtime has installed that slot and bound its entity.
	// [orig: Server_BuildAndBroadcastScoreboard send loop @0x50DDC0]
	return !conn.host_disconnect_sent &&
			conn.phase >= ConnectionPhase::PlayerAdded &&
			conn.phase < ConnectionPhase::Goodbye &&
			conn.link.owned_entity.valid() && conn.link.transport != nullptr;
}

} // namespace

namespace {

// The control quartet, in the walk's state-6 block after the frontier hint: in
// session, for a remote slot (not the host's own, +5) whose spectator latch is
// clear and whose live age has reached 1860 (slot+0x184, the play ticks the
// play-tick walk adds to later in the tick, so the previous ticks' value), the
// two silence punts (the character-attribute silence first), then, punt or no
// punt, the 744-tick countdown and, at its zero, the 0x39 / 0x42, the 0x43 and
// the 0x68 (none queued once the punt's description went). Once
// mature, the countdown runs on through death and deploy states. The +96481
// byte between the two tests has no writer on this host.
// [orig: Server_UpdateAllActivePlayerSlots @0x518820 - is_in_session @0x5189AD,
//  +5 @0x5189B9..0x5189BC, +0x178E1 @0x5189C2, the spectator latch +0x188D7
//  @0x5189CE, the live age `(int)slot[97] >= 1860` @0x5189DA..0x5189E4, the punts
//  @0x5189F5..0x518A3A, the countdown @0x518A42..0x518A5D, the sends @0x518A67 /
//  @0x518A73 / @0x518A79; the play-tick walk's slot test and add
//  @0x51D95E..0x51D97E, the add @0x51D977]
// An orphaned row runs it over its own words (its gates, counts, countdown, seed and stamps):
// its punts punt its net player and its sends go through it, none past the leave
// (`net_player` null; Q7-36 (a)), the counts and the draw stepping on (D-NET-NEW-L1-24).
void run_control_quartet(NapiNPServerCtx &ctx, world::World &world, NapiNPConnection &conn,
		NapiNPConnection *net_player, bool in_match) {
	const world::MatchPlayer *row = world.match.player(conn.link.owned_entity);
	if (!ctx.is_in_session || !in_match || slot_is_local(conn) ||
			conn.link.spectator || row == nullptr ||
			static_cast<int32_t>(row->play_ticks) <
					static_cast<int32_t>(CONTROL_REQUEST_LIVE_GATE_TICKS))
		return;
	SessionReplyState &reply = conn.reply;
	// A punt does not leave the slot's turn: the countdown below runs in the
	// same visit, and at its zero the quartet follows the description.
	// [orig: @0x5189EF..0x5189F5 (charattr < 8 takes the time-sync arm), the 16
	//  arm's push and jmp @0x518A11..0x518A13, the 24 arm's @0x518A37, the one
	//  call @0x518A3A, then the countdown @0x518A42]
	if (net_player != nullptr && reply.charattr_unanswered_count >= kPuntSilenceLimit)
		stage_host_punt(*net_player, kPuntCharattrSilence);
	else if (net_player != nullptr && reply.time_sync_unanswered_count >= kPuntSilenceLimit)
		stage_host_punt(*net_player, kPuntTimeSyncSilence);
	bool control_due = reply.control_request_countdown == 0;
	if (!control_due) {
		--reply.control_request_countdown;
		control_due = reply.control_request_countdown == 0;
	}
	if (control_due) {
		reply.control_request_countdown = CONTROL_REQUEST_PERIOD_TICKS;
		// Once a description went the connection queues no message (the
		// punt above, this visit or an earlier one), but the quartet's own
		// state still moves: the counts, the seed draw, the stamps and the
		// cursor [orig: CNapiNPConnection_QueueMessage @0x628640, the
		// `desc_sent` test @0x62864D (set by CNapiNPConnection_SendDescriptionOnce
		// @0x629750)].
		const bool queues = net_player != nullptr && !net_player->host_disconnect_sent &&
				conn.link.transport != nullptr;

		++reply.charattr_unanswered_count;
		++reply.time_sync_unanswered_count;
		if (reply.control_challenge_seed == 0)
			reply.control_challenge_seed = next_maintenance_prng16(world);
		std::vector<uint8_t> challenge;
		io::append_u32_le(challenge, reply.control_challenge_seed);
		if (queues)
			conn.link.transport->host_send(
					s2c::CHARATTR_CRC_CHALLENGE, std::move(challenge));

		// The authority's charattr disable latches, eight packed into bits
		// 0..5/12/13 (0 until a session's mp_No* word raised one, so every
		// healthy-LAN capture carries 0; D-NET-374).
		// [orig: Server_SendEntityHandleAndInputState @0x507BF2..0x507C29 ->
		//  CharAttr_PackDisabledProperties @0x412550 (IDB
		//  Input_PackStateFlags until 2026-10-12)]
		const uint16_t latches = world.tables.charattr_disabled_word;
		if (queues)
			conn.link.transport->host_send(s2c::CHARATTR_DISABLED_PROPERTIES,
					{static_cast<uint8_t>(latches), static_cast<uint8_t>(latches >> 8)},
					/*reliable=*/false);

		// The time-sync stamps are GetTickCount, the process clock the host binds (D-NET-NEW-L8-29)
		// [orig: Server_RecordCaptureAndSendScore @0x507CA0 - GetTickCount @0x507CC2 into
		//  +0x15F58 @0x507CC4 when zero, @0x507CDA into +0x15F5C @0x507CDC with the sequence's
		//  add @0x507CD3].
		const uint32_t host_ms = world::authority_tick_count_ms(world);
		if (reply.time_sync_host_baseline_ms == 0)
			reply.time_sync_host_baseline_ms = host_ms;
		if (reply.time_sync_round_host_ms == 0) {
			++reply.time_sync_sequence;
			reply.time_sync_round_host_ms = host_ms;
		}
		std::vector<uint8_t> time_sync;
		io::append_u32_le(time_sync, reply.time_sync_sequence);
		if (queues)
			conn.link.transport->host_send(
					s2c::TIME_SYNC_PING, std::move(time_sync));

		// The 0x68 rides unless the host's game.cfg allowcustomskins word is
		// set, dedicated host or not: it advances the slot's cursor by 50 and
		// wraps it to 0 at the host's own frozen loaded-model list count (the
		// IDB's "viewport height" getter returns that count,
		// g_LoadedModelChallengeCount), unsigned. The set branch's one store
		// (slot+0x17D18 = dword_A87060 @0x5113E7) has no reader.
		// [orig: Server_SendLoadedModelPageRequest @0x511360 - the allowcustomskins test
		//  @0x511363..0x51136A (g_GameConfigState+0x524, the cfg's allowcustomskins:
		//  Config_ParseSettingsLine @0x54F818 / @0x54F837),
		//  CEffectWorld_GetLoadedModelPage @0x511375 (g_LoadedModelChallengeCount
		//  @0x5B1578), +50 @0x51137E, `jb` @0x511391, the wrap @0x511393, mask 32
		//  @0x5113C5, the send @0x5113D4]
		if (ctx.config.allow_custom_skins == 0) {
			reply.loaded_model_page_cursor += 50u;
			if (reply.loaded_model_page_cursor >= ctx.loaded_model_list_count)
				reply.loaded_model_page_cursor = 0;
			std::vector<uint8_t> model_page;
			io::append_u32_le(model_page, reply.loaded_model_page_cursor);
			if (queues)
				conn.link.transport->host_send(
						s2c::LOADED_MODEL_PAGE_REQUEST, std::move(model_page),
						/*reliable=*/false);
		}
	}
}

} // namespace

// The 0x81 is the proximity pass's per-slot head: nothing while the round is
// over (g_RoundOver, which Match's outcome stands for), then each
// active slot in state 6 with an entity, with no session test, stores its
// Points when they changed and sends them through the filter's mask 32 to the
// slot's own connection (none once its description closed it, is_in_match).
// [orig: Server_UpdateCaptureZoneProximity @0x5086A0 - g_RoundOver
//  @0x5086A3..0x5086AA, active @0x508720, state 6 @0x50872A, the entity
//  @0x50873B, Points @0x508749, the compare @0x50874E, the store @0x50875F,
//  the send @0x508790; NapiNPServer_SendFiltered's mask-32 arm
//  @0x4C8A06..0x4C8A38]
void emit_requester_score_refreshes(NapiNPServerCtx &ctx,
		world::World &world) {
	if (world.match.outcome().ended) return;
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (!player_slot_active(conn) || conn.slot_state != PlayerSlotState::Joined ||
				world.registry.get(conn.link.owned_entity) == nullptr)
			continue;
		const world::MatchPlayer *player =
				world.match.player(conn.link.owned_entity);
		if (player == nullptr) continue;
		const int32_t score =
				player->stats[world::MatchStats::kPoints];
		if (score == conn.reply.score_delta_sound_value) continue;
		conn.reply.score_delta_sound_value = score;
		if (!is_in_match(conn) || conn.link.transport == nullptr) continue;
		std::vector<uint8_t> body;
		io::append_u32_le(body, static_cast<uint32_t>(score));
		conn.link.transport->host_send(
				s2c::SCORE_DELTA_SOUND, std::move(body),
				/*reliable=*/true);
	}
}

namespace {

// The "<localaddr>JOINTICKET" lookup in the joiner's CD identity pairs
// [orig: CNapiGameSession_SendPlayEnterRequest @0x4D0312..0x4D0362 —
//  String_ConcatTwoSafe(localAddress, "JOINTICKET") then
//  KeyValueBuffer_FindValue @0x4C2A30; a failed lookup rides an empty ticket].
std::string join_ticket_for(const NapiNPServerCtx &ctx, const NapiNPConnection &conn) {
	std::string local_address;
	if (!get_local_address(ctx, local_address)) return {};
	const std::string key = local_address + "JOINTICKET";
	for (const auto &pair : conn.join_identity_pairs) {
		if (strutil::iequals(pair.first, key)) return pair.second;
	}
	return {};
}

} // namespace

// The join-phase validation watchdog, once per periodic second, over the
// joiners whose 0x01 entered NetPlayer state 3 (join_ticket_validation) and
// are not yet added (states 3 and 4); a state-6 / 7 joiner is never visited.
// These are absolute deadlines from the validation stamp.
//   state 3, armed:    announce the joiner to the NovaWorld service as a
//                      ClientPlayerEnterRequest and enter state 4;
//   state 3, no arm:   after 120 s the chat-coded punt 42 "N.C:NONWTOVALU";
//   state 4, armed:    after 120 s the record {DC 2, "N.C:NWJTICKTMOUT", 44};
//   state 4, no arm:   at once the record {DC 2, "N.C:NONWTOVALU", 42}.
// The first latched record wins (stage_host_disconnect). The deadline base is
// the net player's +0xA4, which each game-state change stamps with
// GetTickCount: state 3's at the C2S 0x01, state 4's at the enter request
// here; the walk measures GetTickCount against it, the process clock, which
// runs on through a host load (D-NET-NEW-L8-34).
// [orig: CNapiNetwork_CheckPlayerTimeouts @0x4C8AD0 — caller @0x51DBF3 inside
//  the periodic block; gates @0x4C8B0B; state 3 @0x4C8B76, the arm @0x4C8B88,
//  SendPlayEnterRequest @0x4C8BC1 + SetGameState(4) @0x4C8BCA, stamp read
//  @0x4C8B8A, GetTickCount @0x4C8B90, 0x1D4C0 compare @0x4C8B98 / `jbe` @0x4C8B9D,
//  SendChatMessage(42) @0x4C8BB1; state 4: stamp read @0x4C8CB3, GetTickCount
//  @0x4C8CB9, 0x1D4C0 compare @0x4C8CC1, armed record @0x4C8CD7..0x4C8D68,
//  no-arm record @0x4C8BF8..0x4C8C9E; CNetPlayer_SetGameState @0x4C4060 -
//  GetTickCount @0x4C408A, +0xA4 @0x4C4156]
void Server_CheckPlayerTimeouts(NapiNPServerCtx &ctx) {
	if (!ctx.is_authority) return;
	const uint32_t host_ms = host_tick_count_ms(ctx);
	const bool armed = ctx.novaworld_join_tickets_armed;
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (conn.type != NapiNPConnection::kTypeServerSide ||
				!conn.join_ticket_validation || // [orig: @0x4C8B7C, @0x4C8BD4]
				conn.phase >= ConnectionPhase::PlayerAdded ||
				conn.host_disconnect_sent)
			continue;
		const bool expired = host_ms - conn.join_validated_host_ms > 0x1D4C0u;
		DisconnectEvent event;
		event.ds = 1;
		event.dc = 2;
		if (!conn.player_enter_requested) {
			if (armed) {
				if (ctx.on_player_enter_request) {
					NapiNPServerCtx::PlayerEnterRequest request;
					request.connection_id = conn.connection_id;
					request.peer = conn.peer;
					request.join_ticket = join_ticket_for(ctx, conn);
					ctx.on_player_enter_request(request);
				}
				// State 4's entry restamps +0xA4: its 120 s run from the request.
				// [orig: CNetPlayer_SetGameState(4) called @0x4C8BCA]
				conn.player_enter_requested = true;
				conn.join_validated_host_ms = host_ms;
				continue;
			}
			if (!expired) continue;
			event.dpc = 42;
			event.ddstr = "N.C:NONWTOVALU";
			stage_host_disconnect(conn, event);
			continue;
		}
		if (conn.player_enter_admitted) continue;
		if (armed) {
			if (!expired) continue;
			event.dpc = 44;
			event.ddstr = "N.C:NWJTICKTMOUT";
		} else {
			event.dpc = 42;
			event.ddstr = "N.C:NONWTOVALU";
		}
		stage_host_disconnect(conn, event);
	}
}

// The ServerPlayerEnterResult for a held joiner: Success admits it into the
// spawn pump's population (state 6) and sends the 0x02 probe; a failure latches {DC 2, DP1 MsgCode,
// "NWU:NWPENTERFAIL", DPC 44} and tears the peer down. False when no held
// connection carries that id. The gate is the player's state 4, which only the enter request
// sets, on a joiner the C2S 0x01 held in state 3 under the ticket gate's three tests
// (join_tickets_required): player_enter_pending. The success arm's var-list parse ahead of
// the state store discards every token and clears a word already 0 at the player's create:
// nothing to port (its reader is WacCmd_PlayerIsGold, ported as 0).
// [orig: CNapiGameSession_HandlePlayEnterResponse @0x4D1940 — the player by connection id
//  @0x4D1B20, the state-4 gate @0x4D1B40, NapiNPPlayer_ParseServerVarList (called @0x4D1C08),
//  CNetPlayer_SetGameState(6) (called @0x4D1C11), the failure record @0x4D1B62..0x4D1BD0,
//  CNapiNPConnection_SendDescriptionOnce (called @0x4D1BF7); the request's state 4
//  CNapiNetwork_CheckPlayerTimeouts @0x4C8BCA]
bool Server_ApplyPlayerEnterResult(NapiNPServerCtx &ctx, uint32_t connection_id,
		bool success, int32_t msg_code, uint32_t now_tick) {
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (conn.type != NapiNPConnection::kTypeServerSide ||
				conn.connection_id != connection_id || !conn.player_enter_pending() ||
				conn.host_disconnect_sent)
			continue;
		if (success) {
			conn.player_enter_admitted = true;
			// State 6, then the probe the C2S 0x01 held under the ticket gate.
			// [orig: CNapiGameSession_HandlePlayEnterResponse @0x4D1940 - CNetPlayer_SetGameState(6)
			//  called @0x4D1C11, CNetPlayer_SendFileTransferChunk(0) called @0x4D1C19]
			if (conn.admission_stage == GameAdmissionStage::AwaitPlayEnter &&
					conn.link.transport != nullptr) {
				ProtocolMessage probe = admission_padding_probe(conn, &ctx);
				conn.link.transport->host_send(probe.tag, std::move(probe.payload), probe.reliable,
						probe.flags.raw, probe.capacity_exempt, probe.retention_flushes);
			}
			return true;
		}
		DisconnectEvent event;
		event.ds = 1;
		event.dc = 2;
		event.dp1 = static_cast<uint32_t>(msg_code);
		event.dpc = 44;
		event.ddstr = "NWU:NWPENTERFAIL";
		stage_host_disconnect(conn, event);
		return true;
	}
	return false;
}

// One row's 0x57 leg of the per-slot block (Server_TickUpdate walks the rows in slot order,
// each row's frame then this). The row's cooldown advances even while its net player's send
// boundary is closed; state 6, an open boundary and a live round admit one reliable 62-flush
// RTT request to the row's net player (send_mask 2, the row's +0x1C), so a player is sent one
// per row. Death/deploy do not reset or suppress this clock. A connection's own row steps
// from its add until its description is sent; an orphaned row (D-NET-NEW-L1-24) steps for as
// long as it lives and, past its net player's leave, reads no net player (Q7-36 (a)). The
// cooldown's step ahead of the pump call is read by nothing the frame writes.
// [orig: Server_TickUpdate @0x51D7E0 - the row's +0x1C @0x51E3AA, the cooldown
//  @0x51E3BB..0x51E3C8, the connection's boundary @0x51E3D6, the session @0x51E3F3, the
//  round, the cooldown and state 6 @0x51E40B, the reload @0x51E40D, the 0x57 to the net
//  player @0x51E44A / @0x51E450]
void emit_row_rtt(NapiNPServerCtx &ctx, const world::World &world, NapiNPConnection &row,
		NapiNPConnection *net_player) {
	if (net_player == &row && (row.phase < ConnectionPhase::PlayerAdded ||
			row.phase >= ConnectionPhase::Goodbye || row.host_disconnect_sent))
		return;
	if (row.reply.rtt_request_countdown > 0) --row.reply.rtt_request_countdown;
	if (net_player == nullptr || !ctx.is_in_session || world.match.outcome().ended ||
			!row.burst.spawned || net_player->host_disconnect_sent ||
			net_player->link.transport == nullptr ||
			net_player->s2c_send_holdoff_countdown != 0 ||
			row.reply.rtt_request_countdown != 0)
		return;
	row.reply.rtt_request_countdown = 62;
	// The request's body is GetTickCount, the process clock the host binds, which the return
	// leg subtracts from its own (D-NET-NEW-L8-30) [orig: Server_TickUpdate - GetTickCount
	// @0x51E417, the store @0x51E42E].
	std::vector<uint8_t> body;
	io::append_u32_le(body, world::authority_tick_count_ms(world));
	body.push_back(1);
	net_player->link.transport->host_send(s2c::RTT_ECHO, std::move(body), true, 0, false, 62);
}

// The server-info refresh: the countdown steps down once a server tick; at zero or below it
// reloads 0x744 ticks, advances the cookie-key ring (GetTickCount, the process clock the host
// binds on the World, D-NET-NEW-L8-31, plus the session CRT stream's next draw; on every
// authority, a LAN host's included) and runs the server-info update, which only a NovaWorld host's embedder hooks
// (on_update_server_info: retail's update returns outside a NovaWorld session). The round init
// cleared the countdown, so a round's first server tick refreshes.
// [orig: Server_TickUpdate @0x51D7E0 - the decrement @0x51D91D..0x51D927, `jg` @0x51D92C, the
//  reload 0x744 @0x51D933, CSessionIdRing_AdvanceAndGenerate on g_NapiCookieKeyTable (the call
//  @0x51D93D), Lobby_UpdateServerInfo(0) (the call @0x51D943); CSessionIdRing_AdvanceAndGenerate
//  @0x4DBB80 - GetTickCount @0x4DBB97, rand @0x4DBB9F, the store @0x4DBBB5;
//  Lobby_UpdateServerInfo @0x4FE8C0 - the session and transport gates @0x4FE8D8..0x4FE8FA]
void run_server_info_refresh(NapiNPServerCtx &ctx, world::World &world) {
	if (--ctx.server_info_update_timer > 0) return;
	ctx.server_info_update_timer = static_cast<int32_t>(SESSION_HOST_INFO_REFRESH_TICKS);
	ctx.cookie_key_table.advance(world::authority_tick_count_ms(world),
			static_cast<int>(world.crt_rand.next()));
	if (ctx.on_update_server_info)
		ctx.on_update_server_info();
}

// The scoreboard boundary, every tick after the script pass and ahead of the
// play-tick walk, ahead of the session and round-over tests with none of its
// own: the scoreboard clock (incremented before its compare) and on each
// boundary the 0x16 rebuilt and broadcast to every active slot (no rows past
// the round end, D-NET-635), then the integrity family to the state-6 slots
// with a live entity (none past the round end, all in state 7), its toggle
// flipping each boundary even when the walk sends nothing. Each send fills the
// slot's request word (+0x15F40 for the 0x30, +0x15F44 for the 0x31) from the
// shared PRNG_Next16 stream when it is zero and zeroes it again before the
// request carries it, so every send spends one draw and carries 0; the reply
// handlers only read the word (the XOR salt, 0).
// [orig: Server_TickUpdate @0x51D8F9..0x51D917 - the 0x16 @0x51D90D, the
//  family @0x51D912; Server_BroadcastWeaponSlotStateToPlayers @0x508540 (state 6
//  @0x508584; the 0x30 draw @0x5085A6..0x5085B9, its word @0x5085CB..0x5085E1;
//  the 0x31's ADM row @0x508605..0x508611, its draw @0x508613..0x508626 and word
//  @0x508638..0x50864D; the toggle @0x50868E); Server_ValidateWeaponCRC
//  @0x501F70 reads +89920 (@0x501FD6); Server_ProcessRoundEnd @0x51685E]
void emit_scoreboard_boundary(NapiNPServerCtx &ctx, world::World &world) {
	++ctx.scoreboard_broadcast_timer;
	if (ctx.scoreboard_broadcast_timer <= 0x136u) return;
	ctx.scoreboard_broadcast_timer = 0;
	const bool entity_integrity_family = ctx.integrity_entity_family_next;
	ctx.integrity_entity_family_next = !ctx.integrity_entity_family_next;
	const ProtocolMessage scoreboard =
			build_player_list_message(ctx.config, ctx.np_protocol.connection_list, &world,
					&ctx.orphaned_player_rows);
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (scoreboard_recipient(conn))
			conn.link.transport->host_send(
					scoreboard.tag, scoreboard.payload, scoreboard.reliable);
	}
	// The send walk sends to every active row, an orphaned one through its net player, so that
	// player takes one list per row; the bytes are the same for each (no recipient byte is set
	// in JO). None past the player's leave (D-NET-NEW-L1-24, Q7-36 (a)).
	// [orig: Server_BuildAndBroadcastScoreboard @0x50D960 - the active byte @0x50DDC0, the row
	//  as the target @0x50DDDC, NetPacket_SerializeScoreboard0x16 per row @0x50DDE2, the send
	//  @0x50DE00; NapiNPServer_SendFiltered's 0x20 arm, the row's +0x1C @0x4C8A1F]
	for (OrphanedPlayerRow &orphan : ctx.orphaned_player_rows) {
		NapiNPConnection *net_player = orphan_net_player(ctx, orphan);
		if (net_player != nullptr && !net_player->host_disconnect_sent &&
				net_player->link.transport != nullptr)
			net_player->link.transport->host_send(
					scoreboard.tag, scoreboard.payload, scoreboard.reliable);
	}
	if (world.match.outcome().ended) return;
	// The family walks every active row in slot order, an orphaned one too (D-NET-NEW-L1-24):
	// the row's gate and draw are its own (no net-player read, so they run past the player's
	// leave too), and its request goes to its net player (send_mask 0x20 sends through the
	// row's +0x1C), none past the leave (Q7-36 (a)).
	// [orig: Server_BroadcastWeaponSlotStateToPlayers @0x508540 - the row walk
	//  @0x508562..0x508686, the draws @0x5085A6..0x5085B9 / @0x508613..0x508626, the sends to
	//  the row (send_mask 0x20 @0x5085BF / @0x50862C, SendFiltered @0x50866A)]
	struct FamilyRow {
		NapiNPConnection *row;
		NapiNPConnection *net_player;
	};
	std::vector<FamilyRow> rows;
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) rows.push_back({&conn, &conn});
	for (OrphanedPlayerRow &orphan : ctx.orphaned_player_rows)
		rows.push_back({&orphan.row, orphan_net_player(ctx, orphan)});
	std::stable_sort(rows.begin(), rows.end(), [](const FamilyRow &a, const FamilyRow &b) {
		return a.row->reply.player_slot < b.row->reply.player_slot;
	});
	for (const FamilyRow &r : rows) {
		NapiNPConnection &conn = *r.row;
		const bool own_row = r.net_player == &conn;
		const world::Entity *player =
				own_row ? integrity_player(conn, world) : orphan_integrity_player(conn, world);
		if (player == nullptr) continue;
		replication::ISessionTransport *transport = own_row ? conn.link.transport
				: r.net_player != nullptr &&
								r.net_player->type == NapiNPConnection::kTypeServerSide &&
								!r.net_player->host_disconnect_sent
						? r.net_player->link.transport
						: nullptr;
		SessionReplyState &reply = conn.reply;
		if (entity_integrity_family) {
			if (reply.integrity_weapon_crc_salt == 0)
				reply.integrity_weapon_crc_salt = next_maintenance_prng16(world);
			reply.integrity_weapon_crc_salt = 0;
			const uint32_t word = reply.integrity_weapon_crc_salt;
			if (transport != nullptr)
				transport->host_send(s2c::ENTITY_CHECKSUM_REQ,
						{0xFF, static_cast<uint8_t>(word), static_cast<uint8_t>(word >> 8)},
						/*reliable=*/false);
			continue;
		}
		// AdmDef_GetEntryByIndex(*(u8 *)(entity+688)), then the body's byte
		// is the low byte of adm+4, the row's round index. An unresolved
		// equipped ADM skips the request (and its draw) exactly as retail
		// does; no fallback row exists. A resolved row is the only test: a
		// weapon with no round type keeps adm+4's default -1, so its 0x31
		// carries 0xFF and still spends the draw (D-NET-700).
		// [orig: Server_BroadcastWeaponSlotStateToPlayers @0x508540 (the row
		//  @0x508605..0x508611, its byte `mov ecx, [edi+4]` @0x50863E);
		//  AdmDef_InitEntryDefaults @0x53FEF0 (`or ecx, -1` @0x53FF16, the
		//  store @0x53FF58)]
		const world::WeaponTableEntry *adm =
				world.tables.weapons.by_index(player->equipped_adm_index);
		if (adm != nullptr) {
			if (reply.integrity_ammo_crc_salt == 0)
				reply.integrity_ammo_crc_salt = next_maintenance_prng16(world);
			reply.integrity_ammo_crc_salt = 0;
			const uint32_t word = reply.integrity_ammo_crc_salt;
			if (transport != nullptr)
				transport->host_send(s2c::LOADOUT_CRC_REQ,
						{static_cast<uint8_t>(adm->ammo_index), static_cast<uint8_t>(word),
								static_cast<uint8_t>(word >> 8)},
						/*reliable=*/false);
		}
	}
}

// The periodic second's slot walk, after the capture transaction, slot by slot
// in slot order: for a state-6 slot with an entity, the dead-age counter (the
// consecutive periodic seconds its body carries the dead bit, zeroed by a live
// second) and the S2C 0x6E (sent once) when the body's dead bit or the slot's
// hold is set, with no session test; then in session the t35 deploy-idle punt
// and the t7 dead-too-long punt, the second reached after the first. Both arms
// test a latch (+0x15F28) that nothing in the image writes, so a slot already
// punted meets them again (t7 after t35 in the same second, both in each later
// one); neither writes a punt-log line, and the repeat sends nothing, since
// SendChatMessage keeps the connection's first description
// (stage_host_disconnect, which a second call meets). Ours's walk skips a
// punted slot (is_in_match), whose 0x6E would go to the connection that
// description closed. The round end moves every slot to state 7, which the
// match outcome stands for. The t35 arm: a remote slot held (+89912 & 0x10)
// strictly over six minutes from its state-6 entry stamp, not a spectator. The
// t7 arm's exclusions, all at the punt and none at the counter: permanent
// death, the local slot (+0x5, ours the type-1 test), the bot byte (+0x178E3,
// written only as 0), the spectator latch (+0x188D7) and /NOPUNT (dword_B4C698, not
// modelled); the counter is compared after its increment, so the punt lands
// after 361 dead seconds.
// [orig: Server_TickUpdate - the walk @0x51DFB0..0x51E1A5; state 6 and the
//  entity @0x51E062..0x51E06C, the counter @0x51E06E..0x51E07D, the 0x6E
//  @0x51E083..0x51E0CF (userParam 1 @0x51E0C4; NetPacket_WriteSpawnWaveStatus
//  @0x507490 re-tests the dead bit @0x5074BC and the hold @0x5074C2);
//  is_in_session @0x51E0D4, state 6
//  and the entity @0x51E0E0..0x51E0EC; t35: the hold @0x51E0F2, the stamp
//  @0x51E0FD..0x51E10E (0x57E40), +0x5 @0x51E110, +0x178E3 @0x51E116, the
//  spectator latch @0x51E11F, +0x15F28 @0x51E128, the punt @0x51E13D; t7: the
//  dead bit @0x51E145..0x51E14B, the counter `cmp 168h` @0x51E14D,
//  permanentDeath @0x51E159, +0x5 @0x51E161, +0x178E3 @0x51E167, the spectator
//  latch @0x51E170, +0x15F28 @0x51E179, dword_B4C698 @0x51E181, the punt
//  @0x51E18E; Server_ProcessRoundEnd's state 7 @0x51685E]
void update_periodic_slot_walk(NapiNPServerCtx &ctx, world::World &world) {
	if (world.match.outcome().ended) return;
	// Every active row in slot order: each connection's own row and every orphaned row
	// (D-NET-NEW-L1-24), whose 0x6E goes to its net player (send_mask 0x20 sends through
	// the row's +0x1C) and whose punts punt that net player (Server_LogCRCMismatchPunt
	// describes the row's +0x1C); past the player's leave the row's counter still steps and
	// nothing is sent or punted (Q7-36 (a)).
	// [orig: Server_TickUpdate - the 0x6E's send_mask 0x20 and the row @0x51E09D..0x51E0A7,
	//  sent @0x51E0CF; Server_LogCRCMismatchPunt @0x517ED0 - the row's +0x1C @0x517F47,
	//  CNapiNPConnection_QueueDisconnectEvent called @0x517F5A; NapiNPServer_SendFiltered's
	//  0x20 arm, the row's +0x1C @0x4C8A1F]
	struct SlotRow {
		NapiNPConnection *row;
		NapiNPConnection *net_player;
	};
	std::vector<SlotRow> slots;
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) slots.push_back({&conn, &conn});
	for (OrphanedPlayerRow &orphan : ctx.orphaned_player_rows)
		slots.push_back({&orphan.row, orphan_net_player(ctx, orphan)});
	std::stable_sort(slots.begin(), slots.end(), [](const SlotRow &a, const SlotRow &b) {
		return a.row->reply.player_slot < b.row->reply.player_slot;
	});
	for (const SlotRow &slot : slots) {
		NapiNPConnection &conn = *slot.row;
		NapiNPConnection *net_player = slot.net_player;
		SessionReplyState &reply = conn.reply;
		// State 6 with an entity (ours's added, in-match slot with its body; an orphaned
		// row's own state word).
		const bool own_row = net_player == &conn;
		const bool state6 = own_row ? is_in_match(conn) &&
						conn.phase >= ConnectionPhase::PlayerAdded &&
						conn.phase < ConnectionPhase::Goodbye
				: slot_in_match(conn);
		const world::Entity *body =
				state6 ? world.registry.get(conn.link.owned_entity) : nullptr;
		if (body == nullptr) continue;
		const bool dead = ((body->flags | body->engine_flags) & world::kEntityFlagDead) != 0;
		reply.dead_live_seconds = dead ? reply.dead_live_seconds + 1 : 0;
		// The 0x20 arm sends to a server-side net player; a described one's queue drops it.
		const bool reaches_net_player = net_player != nullptr &&
				net_player->link.transport != nullptr &&
				(own_row || (net_player->type == NapiNPConnection::kTypeServerSide &&
						!net_player->host_disconnect_sent));
		if ((dead || conn.link.respawn_pending) && reaches_net_player)
			net_player->link.transport->host_send(
					s2c::SPAWN_WAVE_STATUS,
					build_spawn_wave_status_body(world, conn), // the slot's rows (D-NET-581)
					/*reliable=*/false);
		if (!ctx.is_in_session || net_player == nullptr) continue;
		const bool join_deploy_idle_gate =
				conn.type == NapiNPConnection::kTypeServerSide &&
				net_player->link.transport != nullptr &&
				conn.link.respawn_pending &&
				!conn.link.spectator &&
				reply.state6_entry_host_ms_valid;
		// GetTickCount against the state-6 stamp, the process clock, so a host load counts
		// (D-NET-NEW-L8-35) [orig: Server_TickUpdate - GetTickCount @0x51E0FB, the stamp
		//  @0x51E0FD, `sub` @0x51E107, 0x57E40 @0x51E109].
		if (join_deploy_idle_gate &&
				static_cast<uint32_t>(host_tick_count_ms(ctx) - reply.state6_entry_host_ms) >
						kJoinDeployIdleLimitMs)
			stage_host_punt(*net_player, kPuntJoinDeployIdle);
		if (dead && reply.dead_live_seconds > kDeadLiveSecondLimit &&
				!ctx.config.permanent_death &&
				conn.type == NapiNPConnection::kTypeServerSide &&
				!conn.link.spectator)
			stage_host_punt(*net_player, kPuntDeadTooLong);
	}
}

// The 0x79 countdown, every tick after the periodic second's block, ahead of the
// session test with none of its own: S2C 0x79 is the low byte of
// CNetStats.host_quality (despite its old "spectator flag" label). The global
// countdown starts at zero, reloads to 0x136 and sends with filter 0x80
// (player-slot states 6/7, dead or held slots included; no entity-health
// gate). This is one GLOBAL host boundary: a player filtered out at that
// instant waits for the next boundary and never receives a per-peer catch-up.
// The duplicate-PCID kick follows.
// [orig: Server_TickUpdate - the countdown @0x51E1B2..0x51E1C5, the send
//  @0x51E1C7..0x51E202, CNapiServer_KickDuplicatePcids @0x4C9290 (the call @0x51E20C), is_in_session
//  @0x51E211]
void emit_network_quality_boundary(NapiNPServerCtx &ctx) {
	bool network_quality_boundary = ctx.network_quality_broadcast_countdown == 0;
	if (!network_quality_boundary) {
		--ctx.network_quality_broadcast_countdown;
		network_quality_boundary = ctx.network_quality_broadcast_countdown == 0;
	}
	if (network_quality_boundary) {
		ctx.network_quality_broadcast_countdown = NETWORK_QUALITY_BROADCAST_PERIOD_TICKS;
		for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
			if (active_player_recipient(conn))
				conn.link.transport->host_send(
						s2c::NETWORK_QUALITY, {ctx.host_network_quality});
		}
	}
	Server_KickDuplicatePcids(ctx, network_quality_boundary);
}

// The play-tick walk after the script pass: every active slot in state 6
// whose entity is present and not hidden (Flags bit 0) adds one to its play
// ticks, the unsaturated slot dword WAC onptick reads in whole seconds. No
// session, deploy or punt gate: it runs before the is_in_session test, so
// single player counts too. The same walk then calls
// GameEvent_ProcessScoring(g_GameType, entity, 0x16, 0, 0) for each such slot,
// whose case 22 only stamps the slot's stats entity word and counts the
// per-weapon and per-vehicle use sub-tables (CPlayerStats_RecordEvent 35/36,
// the CPlayerStats_InitWeaponTracking arrays). Nothing reads those tables: the
// weapon index map and its count are referenced only by the tracking's init,
// its reset and RecordEvent, and the kill accumulators' metric counters past
// MOSTPLAYERS are printed by no reporter; so that call is not ported.
// [orig: the weapon index dword_24E2ED8, its list dword_24E1F38 and count dword_24E3E80 (xrefs:
//  CPlayerStats_InitWeaponTracking @0x52BF60, CPlayerStats_ResetAllArrays @0x52C4C0,
//  CPlayerStats_RecordEvent @0x52C8E0 only); Score_AccumulateKillByEntityType
//  @0x4FA300 / Score_AccumulateKillWithWeaponStats @0x4FA3B0 into g_ProcessServerTick and
//  g_MissionServerTick (GameEvent_PlayerDeath @0x516F14 / @0x51713D), whose rows the SERVER
//  and SERVERMISSION reporters never print (Server_SendServerMetricsToGate @0x4FA4B0,
//  Server_SendMissionMetrics @0x4FB3A0)]
// [orig: Server_TickUpdate @0x51D94B..0x51D9A3 -- the active byte @0x51D960,
//  state 6 @0x51D966, the entity @0x51D96B..0x51D96F, `test byte ptr
//  [eax+24h],1` @0x51D971, `add [esi+184h],1` @0x51D977, the scorer call
//  @0x51D98A; the is_in_session test @0x51D9A5; GameEvent_ProcessScoring
//  case 22 @0x52F550]
void advance_play_ticks(NapiNPServerCtx &ctx, world::World &world) {
	// The round end moves every state-6 slot to state 7, which the walk then
	// skips; Match's outcome gate stands for that transition here, as it does
	// for the rest of the slot-state-7 stop (announce_round_end).
	// [orig: Server_ProcessRoundEnd -- the slot state 6 -> 7 store @0x51685E]
	if (world.match.outcome().ended) return;
	// Every active row's, an orphaned row's too (D-NET-NEW-L1-24): its own state and body
	// [orig: Server_TickUpdate - the walk @0x51D95E..0x51D98A over every row].
	std::vector<const NapiNPConnection *> rows;
	for (const NapiNPConnection &conn : ctx.np_protocol.connection_list) rows.push_back(&conn);
	for (const OrphanedPlayerRow &orphan : ctx.orphaned_player_rows) rows.push_back(&orphan.row);
	for (const NapiNPConnection *row : rows) {
		const NapiNPConnection &conn = *row;
		// The slot's own state: a punted slot plays on until the teardown (D-NET-652).
		if (!slot_in_match(conn)) continue;
		const world::Entity *player = world.registry.get(conn.link.owned_entity);
		if (player == nullptr || ((player->flags | player->engine_flags) & 1u) != 0)
			continue;
		if (world::MatchPlayer *slot = world.match.player(player->handle))
			++slot->play_ticks;
	}
}

bool Server_StageHostDisconnect(
		NapiNPConnection &connection, const DisconnectEvent &event) {
	return stage_host_disconnect(connection, event);
}

bool Server_StageHostPunt(
		NapiNPConnection &connection, uint32_t mismatch_type) {
	return stage_host_punt(connection, mismatch_type);
}

// [orig: Server_PlayerPuntCRCMisMatch @0x50F380]
void Server_CountCRCMismatchPunt(NapiNPServerCtx &ctx, const NapiNPConnection &connection,
		uint32_t mismatch_type) {
	if (ctx.punt_crc_counters == nullptr) return;
	if (connection.reply.player_slot >= ctx.config.total_player_slot_capacity()) return;
	if (!player_slot_active(connection) || slot_is_local(connection)) return;
	if (mismatch_type == 9) ++(*ctx.punt_crc_counters)[1];
	else if (mismatch_type == 10) ++(*ctx.punt_crc_counters)[2];
}

void Server_RearmMinimapInitialScan(NapiNPServerCtx &ctx) {
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		conn.reply.minimap_initial_scan_pending = true;
	}
}

// The kit-weight recompute (the IDB's misnamed Server_RecalculateAllPlayerScores):
// for every active slot with an entity — no state gate — walk the host-side
// weapon-slot rows summing weaponweight + WeaponSlot_GetTotalClips (the LIVE
// pool + loaded-clip count) x clipweight, plus the first sub-variant of a
// different ammo class at the main clipweight, and store the 16.16 sum into
// entity+892 (the run-promotion band input). The listen host's own player rows
// ARE its local inventory (LocalPlayerLoadout::weight_fp16 mirrors into its
// body every tick); a remote player's rows are its rebuilt slot table (the kit
// and its sub-variants, D-NET-509) with their live clips over the authority
// pool table, stamped straight onto the body the host already runs
// player_body_select for; every clip count reads through the player's seat.
// Called once per periodic second and after every accepted loadout.
// [orig: Server_RecalculateAllPlayerScores @0x5014E0 — slot walk @0x5014f9..0x501604,
//  byte+4 @0x501502, weaponweight @0x501538, WeaponSlot_GetTotalClips @0x501546
//  (-> @0x5425F0), clipweight @0x501564, sub-variant loop @0x501568..0x5015cf,
//  skip @0x5015d1, store entity+892 @0x5015f7; callers Server_TickUpdate
//  @0x51e1ab and NapiNPServerMsg_HandlePlayerLoadout @0x515f9d]
void Server_RecalculateAllPlayerKitWeights(
		std::vector<NapiNPConnection> &roster, world::World &world) {
	const world::WeaponTable &table = world.tables.weapons;
	if (table.empty()) return;
	for (NapiNPConnection &conn : roster) {
		if (conn.phase < ConnectionPhase::PlayerAdded ||
				conn.phase >= ConnectionPhase::Goodbye ||
				!conn.link.owned_entity.valid())
			continue;
		if (world.cached.local_player.valid() &&
				conn.link.owned_entity == world.cached.local_player) {
			world::LocalPlayer *local = world.local_player_state;
			if (local == nullptr || !local->inventory_valid) continue;
			const world::Entity *own = world.registry.get(conn.link.owned_entity);
			local->loadout.weight_fp16 = world::weapon_inventory_loadout_weight_fp16(table,
					local->inventory,
					own != nullptr ? world::entity_ammo_seat(world, *own) : world::WeaponAmmoSeat{});
			continue;
		}
		world::AiEntity *body = world.ai.for_handle(conn.link.owned_entity);
		if (body == nullptr) continue;
		const world::Entity *entity = world.registry.get(conn.link.owned_entity);
		world::WeaponInventory inventory;
		inventory.reset(table);
		if (entity != nullptr) for (const auto &[combo, row] : entity->weapon_slots) {
			world::WeaponInventorySlot *slot = inventory.slot(combo);
			if (slot == nullptr) continue;
			slot->adm_index = row.adm_index;
			slot->clip = row.clip;
		}
		const size_t pool_count =
				std::min(inventory.pools.size(), conn.reply.ammo_pools.size());
		for (size_t i = 0; i < pool_count; ++i)
			inventory.pools[i] = conn.reply.ammo_pools[i];
        for (size_t i = 0; i < pool_count; ++i)
            inventory.shared_clips[i] = conn.reply.shared_clips[i];
		body->inf.loadout_weight_fp16 = world::weapon_inventory_loadout_weight_fp16(table,
				inventory,
				entity != nullptr ? world::entity_ammo_seat(world, *entity) : world::WeaponAmmoSeat{});
	}
}

} // namespace opennova::inmatch
