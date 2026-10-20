#include <runtime/inmatch/server_tick_maintenance.h>
#include <runtime/inmatch/server_tick.h>
#include <runtime/inmatch/server_message_dispatch.h> // build_player_list_message
#include <runtime/inmatch/server_overlay_state.h>     // the 14-tick minimap overlay visits
#include <runtime/inmatch/server_player_death.h>     // the flag reset's Health store (kill_player_body_at_frame_tail)

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <base/gameprofile/game_type.h>        // the CTF / FlagBall / Flag Me carry-limit modes
#include <base/io/le.h>                        // append_u32_le
#include <base/io/strutil.h>                   // iequals (the JOINTICKET key lookup)
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

// The first description event owns the connection's disconnect slot. Closing
// the gameplay predicate here prevents later producers from following it with
// an ordinary frame while HostOwner still has one chance to flush the record.
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
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
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
//  (!g_SpawnSuccessGate && !g_PreRoundDelayTimer), non-session zero
//  @0x51889c, latch -1 @0x5188ac, decrement @0x5188b8..0x5188c5; caller
//  Server_TickUpdate @0x51d88b ahead of the recv pump @0x51d895]
void tick_spawn_protection(world::World &world, NapiNPConnection &conn) {
	if (world.match.outcome().ended || world.preround_delay_seconds != 0) return;
	if (!is_in_match(conn) || !conn.link.owned_entity.valid()) return;
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
void emit_frontier_hint(world::World &world, NapiNPConnection &conn) {
	if (!is_in_match(conn) || !conn.reply.frontier_hint_pending) return;
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

} // namespace

// The per-player maintenance walk at the tick head, ahead of the receive pump:
// per slot, the spawn-protection arm, the minimap overlay visit, then the
// frontier hint. All three sit in the walk's state-6 block, and the round end
// moves every slot to state 7, which the match outcome stands for here, so
// none runs through the end-round linger. The walk is the slot table's, in
// slot order: a visit's carrier pass can flush its ring to another slot, so
// the order shows on that slot's wire.
// [orig: Server_UpdateAllActivePlayerSlots @0x518820 - the slot-index walk
//  @0x518824..0x518A81, the state test @0x518878, the protection
//  @0x5188AC..0x5188C5, the overlay visit @0x5188D6, the frontier hint
//  @0x518929; called @0x51D88B; Server_ProcessRoundEnd's state 7 @0x51685E]
void update_all_active_player_slots(NapiNPServerCtx &ctx, world::World &world) {
	if (world.match.outcome().ended) return;
	std::vector<NapiNPConnection *> slots;
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) slots.push_back(&conn);
	std::stable_sort(slots.begin(), slots.end(),
			[](const NapiNPConnection *a, const NapiNPConnection *b) {
				return a->reply.player_slot < b->reply.player_slot;
			});
	for (NapiNPConnection *conn : slots) {
		tick_spawn_protection(world, *conn);
		emit_minimap_overlay_for_slot(ctx, world, *conn);
		emit_frontier_hint(world, *conn);
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
// it); stage_host_punt already carries the first-event latch (slot dword
// +89896), which also gates the log line.
// [orig: Server_CheckPlayerViolations @0x51ABD0 — in-session @0x51abd9, slot
//  gates @0x51abf6/@0x51abff/@0x51ac0d, game types @0x51ac2c, counter
//  @0x51ac5a/@0x51ac63, limit @0x51ac75, drop/sync/health @0x51ac84/@0x51ac8a/
//  @0x51ac94, slot +5/+96483 @0x51aca2, TK gate @0x51acc2, suicide gate
//  @0x51ad00, punts @0x51ad17/@0x51acd9; the sole caller is the periodic-second
//  block of Server_TickUpdate @0x51df5f]
void check_player_violations(NapiNPServerCtx &ctx, world::World &world) {
	if (!ctx.is_in_session) return;
	const uint32_t gt = world.match.rules().game_type;
	const bool carry_limited = gt == game_type::kCaptureTheFlag ||
			gt == game_type::kFlagBall || gt == game_type::kFlagMe;
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (!is_in_match(conn) || !conn.link.owned_entity.valid()) continue;
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
		if (limit < 0 || stats->stats[world::MatchStats::kTeamKills] <= limit) {
			if (stats->stats[world::MatchStats::kSuicides] > 9) {
				server_logs_punt(ctx.logs, conn, "#S>9"); // [orig: @0x51ad0d]
				stage_host_punt(conn, 6);
			}
		} else {
			stage_host_punt(conn, 6);
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

const world::Entity *integrity_player(
		const NapiNPConnection &conn, const world::World &world) {
	if (!is_in_match(conn) || conn.link.transport == nullptr)
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

const world::Entity *control_age_player(
		const NapiNPConnection &conn, const world::World &world) {
	if (!is_in_match(conn) || conn.link.transport == nullptr)
		return nullptr;
	return world.registry.get(conn.link.owned_entity);
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

void emit_requester_score_refreshes(NapiNPServerCtx &ctx,
		world::World &world) {
	if (!ctx.is_in_session) return;
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (!is_in_match(conn) || conn.link.transport == nullptr ||
				!conn.link.owned_entity.valid())
			continue;
		const world::MatchPlayer *player =
				world.match.player(conn.link.owned_entity);
		if (player == nullptr) continue;
		const int32_t score =
				player->stats[world::MatchStats::kPoints];
		if (score == conn.reply.score_delta_sound_value) continue;
		conn.reply.score_delta_sound_value = score;
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
// The first latched record wins (stage_host_disconnect); the stamp is the
// one netPlayer+0xA4 word for both states.
// [orig: CNapiNetwork_CheckPlayerTimeouts @0x4C8AD0 — caller @0x51DBF3 inside
//  the periodic block; gates @0x4C8B0B; state 3 @0x4C8B76, the arm @0x4C8B88,
//  SendPlayEnterRequest @0x4C8BC1 + SetGameState(4) @0x4C8BCA, stamp read
//  @0x4C8B8A, 0x1D4C0 compare @0x4C8B9D, SendChatMessage(42) @0x4C8BB1;
//  state 4 armed record @0x4C8CD7..0x4C8D68, no-arm record @0x4C8BF8..0x4C8C9E]
void Server_CheckPlayerTimeouts(NapiNPServerCtx &ctx) {
	if (!ctx.is_authority) return;
	const uint32_t host_ms = ctx.np_protocol.host_run_duration_ms;
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
				conn.player_enter_requested = true;
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
// spawn pump's population (state 6); a failure latches {DC 2, DP1 MsgCode,
// "NWU:NWPENTERFAIL", DPC 44} and tears the peer down. False when no held
// connection carries that id.
// [orig: CNapiGameSession_HandlePlayEnterResponse @0x4D1940 — the state-4
//  gate @0x4D1B3E, SetGameState(6) @0x4D1C11, the failure record
//  @0x4D1B62..0x4D1BD0, TrySendSessionInit @0x4D1BF7]
bool Server_ApplyPlayerEnterResult(NapiNPServerCtx &ctx, uint32_t connection_id,
		bool success, int32_t msg_code) {
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (conn.type != NapiNPConnection::kTypeServerSide ||
				conn.connection_id != connection_id || !conn.player_enter_pending() ||
				conn.host_disconnect_sent)
			continue;
		if (success) {
			conn.player_enter_admitted = true;
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

// [orig: Server_TickUpdate @0x51D7E0] The player-slot
// cooldown advances even while the connection's send boundary is closed.
// State 6, an open boundary, and a live round admit one reliable 62-flush
// RTT request. Death/deploy do not reset or suppress this clock.
void emit_periodic_rtt(NapiNPServerCtx &ctx, const world::World &world) {
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (conn.phase < ConnectionPhase::PlayerAdded ||
				conn.phase >= ConnectionPhase::Goodbye || conn.host_disconnect_sent) continue;
		if (conn.reply.rtt_request_countdown > 0) --conn.reply.rtt_request_countdown;
		if (!ctx.is_in_session || world.match.outcome().ended ||
				!is_in_match(conn) || conn.link.transport == nullptr ||
				conn.s2c_send_holdoff_countdown != 0 || conn.reply.rtt_request_countdown != 0) continue;
		conn.reply.rtt_request_countdown = 62;
		std::vector<uint8_t> body;
		io::append_u32_le(body, io::host_milliseconds_for_logic_tick(world.logic_tick));
		body.push_back(1);
		conn.link.transport->host_send(s2c::RTT_ECHO, std::move(body), true, 0, false, 62);
	}
}

// Emit the stock host's player maintenance requests. Integrity is a global
// scoreboard-cadence broadcast and remains active while a live player holds the
// deployment UI. Network quality owns a separate global countdown. The control
// quartet has an authoritative slot-state/live-age gate and is never derived
// from a world-clock epoch.
// [orig: Server_TickUpdate @0x51D7E0 -> @0x508540; per-player quartet
// Server_UpdateAllActivePlayerSlots @0x518820]
// `periodic_second` is the shared one-second boundary: the dead-age counter
// below lives inside retail's g_PeriodicSecondTimer block and advances once
// per second, not once per tick.
void emit_periodic_session_maintenance(NapiNPServerCtx &ctx, world::World &world,
		bool periodic_second) {
	// Retail advances both global clocks before its is_in_session gates. The
	// scoreboard counter is increment-before-compare and the family toggle flips
	// once per crossed boundary even if the recipient walk sends nothing.
	// [orig: Server_TickUpdate @0x51D8F9..0x51D917;
	// Server_BroadcastWeaponSlotStateToPlayers @0x50868E]
	bool integrity_boundary = false;
	bool entity_integrity_family = false;
	++ctx.scoreboard_broadcast_timer;
	if (ctx.scoreboard_broadcast_timer > 0x136u) {
		ctx.scoreboard_broadcast_timer = 0;
		integrity_boundary = true;
		entity_integrity_family = ctx.integrity_entity_family_next;
		ctx.integrity_entity_family_next = !ctx.integrity_entity_family_next;
	}
	bool network_quality_boundary =
			ctx.network_quality_broadcast_countdown == 0;
	if (!network_quality_boundary) {
		--ctx.network_quality_broadcast_countdown;
		network_quality_boundary =
				ctx.network_quality_broadcast_countdown == 0;
	}
	if (network_quality_boundary)
		ctx.network_quality_broadcast_countdown =
				NETWORK_QUALITY_BROADCAST_PERIOD_TICKS;
	// Ahead of retail's session and round-over tests: the 0x16 to every active
	// slot (no rows past the round end, D-NET-635), the integrity family to
	// state-6 slots alone (none past it, all in state 7; the toggle above flips
	// regardless), the 0x79 to states 6 / 7. [orig: the 0x16 @0x51D90D, the family
	//  @0x51D912, is_in_session @0x51D9A5, g_SpawnSuccessGate @0x51D9B1; the 0x79
	//  @0x51E1B2 ahead of @0x51E211; state 6 @0x508584; Server_ProcessRoundEnd @0x51685E]
	const bool round_over = world.match.outcome().ended;
	const ProtocolMessage scoreboard = integrity_boundary
			? build_player_list_message(
					ctx.config, ctx.np_protocol.connection_list, &world)
			: ProtocolMessage{};
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		const world::Entity *player = round_over ? nullptr : integrity_player(conn, world);
		// The retail boundary rebuilds and broadcasts the transient 0x16 before
		// entering the integrity family walk. Keep both semantic records on the
		// same owner boundary and in that order. [orig: @0x51D90D/@0x51D912]
		if (integrity_boundary && scoreboard_recipient(conn)) {
			conn.link.transport->host_send(
					scoreboard.tag, scoreboard.payload, scoreboard.reliable);
		}
		if (player != nullptr && integrity_boundary) {
			if (entity_integrity_family) {
				conn.link.transport->host_send(
						s2c::ENTITY_CHECKSUM_REQ, {0xFF, 0x00, 0x00},
						/*reliable=*/false);
			} else {
				// AdmDef_GetEntryByIndex(*(u8 *)(entity+688)), then row =
				// adm_entry[1] [orig: @0x508540]. An unresolved equipped ADM
				// skips the request exactly as retail does; no fallback row exists.
				const world::WeaponTableEntry *adm =
						world.tables.weapons.by_index(player->equipped_adm_index);
				if (adm != nullptr && adm->ammo_index >= 0 && adm->ammo_index <= 0xFF) {
					conn.link.transport->host_send(
							s2c::LOADOUT_CRC_REQ,
							{static_cast<uint8_t>(adm->ammo_index), 0x00, 0x00},
							/*reliable=*/false);
				}
			}
		}

		// Despite its old "spectator flag" label, S2C 0x79 is the low byte
		// of CNetStats.host_quality. Retail's global countdown starts at zero,
		// reloads to 0x136, and sends with filter 0x80 (player-slot states 6/7,
		// including dead or respawn-pending slots; no entity-health gate).
		// This is one GLOBAL host boundary: a player filtered out at that instant
		// waits for the next boundary and never receives a per-peer catch-up.
		if (network_quality_boundary && active_player_recipient(conn)) {
			conn.link.transport->host_send(
					s2c::NETWORK_QUALITY, {ctx.host_network_quality});
		}
	}
	Server_KickDuplicatePcids(ctx, network_quality_boundary); // [orig: @0x51E20C]
	// State-6 work: the quartet and the punts under retail's session tests, the two age
	// counters under state 6 alone, so their session gate is ours (D-NET-696; no production
	// path clears is_in_session) [orig: @0x5189AD, @0x51E0D4; @0x51D966, @0x51E062].
	if (!ctx.is_in_session || round_over) return;
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		SessionReplyState &reply = conn.reply;
		// UpdateAllActivePlayerSlots checks the PREVIOUS live-age value. Age is
		// incremented later in Server_TickUpdate only for a state-6 player entity
		// whose Flags bit 0 is clear. Pending/hidden pauses rather than resets age;
		// health and Flags bit 0x02 do not enter either gate. Once age is mature,
		// the quartet countdown continues through death/deploy presentation state.
		// [orig: quartet gate @0x5189E4; age increment @0x51D95E..0x51D97E]
		const world::Entity *age_player = control_age_player(conn, world);
		const bool control_gate = ctx.is_in_session && is_in_match(conn) &&
				conn.link.transport != nullptr &&
				reply.control_live_ticks >= CONTROL_REQUEST_LIVE_GATE_TICKS;
		// Retail tests both silence counters before touching the quartet
		// countdown. Character-attribute silence has precedence when both are
		// over the limit, and the event is not delayed until another request is
		// due. [orig: gates/calls @0x5189E4]
		if (control_gate &&
				reply.charattr_unanswered_count >= kPuntSilenceLimit) {
			stage_host_punt(conn, kPuntCharattrSilence);
			continue;
		}
		if (control_gate &&
				reply.time_sync_unanswered_count >= kPuntSilenceLimit) {
			stage_host_punt(conn, kPuntTimeSyncSilence);
			continue;
		}
		if (control_gate) {
			bool control_due = reply.control_request_countdown == 0;
			if (!control_due) {
				--reply.control_request_countdown;
				control_due = reply.control_request_countdown == 0;
			}
			if (control_due) {
				reply.control_request_countdown = CONTROL_REQUEST_PERIOD_TICKS;

				++reply.charattr_unanswered_count;
				++reply.time_sync_unanswered_count;
				if (reply.control_challenge_seed == 0)
					reply.control_challenge_seed = next_maintenance_prng16(world);
				std::vector<uint8_t> challenge;
				io::append_u32_le(challenge, reply.control_challenge_seed);
				conn.link.transport->host_send(
						s2c::CHARATTR_CRC_CHALLENGE, std::move(challenge));

				// The authority's charattr disable latches, eight packed into bits
				// 0..5/12/13 (0 until a session's mp_No* word raised one, so every
				// healthy-LAN capture carries 0; D-NET-374).
				// [orig: Server_SendEntityHandleAndInputState @0x507BF2..0x507C29 ->
				//  CharAttr_PackDisabledProperties @0x412550 (IDB
				//  Input_PackStateFlags until 2026-10-12)]
				const uint16_t latches = world.tables.charattr_disabled_word;
				conn.link.transport->host_send(s2c::CHARATTR_DISABLED_PROPERTIES,
						{static_cast<uint8_t>(latches), static_cast<uint8_t>(latches >> 8)},
						/*reliable=*/false);

				const uint32_t host_ms =
						io::host_milliseconds_for_logic_tick(world.logic_tick);
				if (reply.time_sync_host_baseline_ms == 0)
					reply.time_sync_host_baseline_ms = host_ms;
				if (reply.time_sync_round_host_ms == 0) {
					++reply.time_sync_sequence;
					reply.time_sync_round_host_ms = host_ms;
				}
				std::vector<uint8_t> time_sync;
				io::append_u32_le(time_sync, reply.time_sync_sequence);
				conn.link.transport->host_send(
						s2c::TIME_SYNC_PING, std::move(time_sync));

				// A dedicated host sends no 0x68; any other advances the slot's
				// cursor by 50 and wraps it to 0 at its own frozen loaded-model
				// list count (the IDB's "viewport height" getter returns that
				// count, g_LoadedModelChallengeCount), unsigned.
				// [orig: Server_SendRandomSeedSync @0x511360 - the dedicated test
				//  @0x511363..0x51136A, CEffectWorld_GetViewportDimensions @0x511375
				//  (g_LoadedModelChallengeCount @0x5B1578), +50 @0x51137E, `jb`
				//  @0x511391, the wrap @0x511393, mask 32 @0x5113C5, the send @0x5113D4]
				if (ctx.connection_mode != ConnectionMode::HostOnly) {
					reply.loaded_model_page_cursor += 50u;
					if (reply.loaded_model_page_cursor >= ctx.loaded_model_list_count)
						reply.loaded_model_page_cursor = 0;
					std::vector<uint8_t> model_page;
					io::append_u32_le(model_page, reply.loaded_model_page_cursor);
					conn.link.transport->host_send(
							s2c::LOADED_MODEL_PAGE_REQUEST, std::move(model_page),
							/*reliable=*/false);
				}
			}
		}

		// A remote player held in the join-time state-byte 0x10 deployment
		// state for strictly more than six minutes receives t35. This is based
		// on the state-6 entry timestamp, not world ticks or entity damage time,
		// and remains eligible before the initial burst reaches InMatch. A
		// spectator holds the deploy bit forever and is explicitly EXEMPT
		// [orig: Server_TickUpdate @0x51E109, compare 0x57E40; the spectator
		// bail `cmp slot+100567, 0` @0x51e11f].
		const bool join_deploy_idle_gate = ctx.is_in_session &&
				!conn.host_disconnect_sent && conn.type == NapiNPConnection::kTypeServerSide &&
				conn.phase >= ConnectionPhase::PlayerAdded &&
				conn.phase < ConnectionPhase::Goodbye &&
				conn.link.transport != nullptr &&
				conn.link.owned_entity.valid() &&
				world.registry.get(conn.link.owned_entity) != nullptr &&
				conn.link.respawn_pending &&
				!conn.link.spectator &&
				reply.state6_entry_host_ms_valid;
		if (join_deploy_idle_gate &&
				static_cast<uint32_t>(ctx.np_protocol.host_run_duration_ms -
						reply.state6_entry_host_ms) > kJoinDeployIdleLimitMs) {
			stage_host_punt(conn, kPuntJoinDeployIdle);
			continue;
		}
		// The dead-age arm owns an independent consecutive-SECOND counter. It
		// does not derive elapsed time from Entity::death_tick: the counter and
		// its compare sit inside the g_PeriodicSecondTimer block (reload 62),
		// so retail increments the player-slot dword once per periodic second
		// while a state-6 entity has Flags bit 0x02 set, resets it as soon as
		// the bit clears, and compares after the increment — punt type 7 lands
		// after 361 seconds dead, not 361 ticks (5.8 s). The punt's exclusions,
		// in retail all at the punt and none at the counter: permanent death (ours
		// `config.permanent_death`), the local slot (+0x5, ours the type-1 test),
		// the bot byte (+0x178E3, never set), the spectator latch (+0x188D7, ours
		// `conn.link.spectator`, as the t35 arm tests it), the punt latch
		// (+0x15F28, ours `conn.host_disconnect_sent`) and /NOPUNT (dword_B4C698,
		// not modelled).
		// [orig: Server_TickUpdate @0x51D7E0 — periodic gate reload 62
		//  @0x51DB93; counter @0x51E062..0x51E07D; the exclusions: permanentDeath
		//  @0x51E159, +0x5 @0x51E161, +0x178E3 @0x51E167, the spectator latch
		//  `cmp byte ptr [esi+188D7h], 0` @0x51E170, +0x15F28 @0x51E179,
		//  dword_B4C698 @0x51E181; punt @0x51E187..0x51E18E]
		if (periodic_second) {
			const bool dead_state6 = ctx.is_in_session &&
					!conn.host_disconnect_sent && conn.type == NapiNPConnection::kTypeServerSide &&
					conn.phase >= ConnectionPhase::PlayerAdded &&
					conn.phase < ConnectionPhase::Goodbye &&
					age_player != nullptr &&
					((age_player->flags | age_player->engine_flags) &
							world::kEntityFlagDead) != 0;
			if (dead_state6) {
				++reply.dead_live_seconds;
			} else {
				reply.dead_live_seconds = 0;
			}
			if (dead_state6 &&
					reply.dead_live_seconds > kDeadLiveSecondLimit &&
					!ctx.config.permanent_death && !conn.link.spectator) {
				stage_host_punt(conn, kPuntDeadTooLong);
				continue;
			}
		}

		const bool age_eligible = age_player != nullptr &&
				!conn.link.respawn_pending &&
				((age_player->flags | age_player->engine_flags) & 1u) == 0;
		if (age_eligible &&
				reply.control_live_ticks < CONTROL_REQUEST_LIVE_GATE_TICKS)
			++reply.control_live_ticks;
	}
}

// The play-tick walk after the script pass: every active slot in state 6
// whose entity is present and not hidden (Flags bit 0) adds one to its play
// ticks, the unsaturated slot dword WAC onptick reads in whole seconds. No
// session, deploy or punt gate: it runs before the is_in_session test, so
// single player counts too. The same walk then calls
// GameEvent_ProcessScoring(g_GameType, entity, 0x16, 0, 0) for each such slot,
// whose case 22 only stamps the slot's stats entity word and counts the
// per-weapon and per-vehicle use sub-tables (CPlayerStats_RecordEvent 35/36,
// the CPlayerStats_InitWeaponTracking arrays); the port models neither, so
// that call is not ported.
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
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (!is_in_match(conn)) continue;
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
		world::WeaponInventory inventory;
		inventory.reset(table);
		for (const auto &[combo, row] : conn.weapon_slots) {
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
		const world::Entity *entity = world.registry.get(conn.link.owned_entity);
		body->inf.loadout_weight_fp16 = world::weapon_inventory_loadout_weight_fp16(table,
				inventory,
				entity != nullptr ? world::entity_ammo_seat(world, *entity) : world::WeaponAmmoSeat{});
	}
}

} // namespace opennova::inmatch
