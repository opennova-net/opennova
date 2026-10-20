#include <runtime/inmatch/server_tick.h>
#include <runtime/devtools/tick_profile.h>
#include <runtime/inmatch/end_round_protocol.h>
#include <runtime/inmatch/mission_exit.h>         // the round end's exit reasons 3 / 4
#include <runtime/inmatch/mission_rotation.h>     // HostRotation::last_game
#include <runtime/inmatch/server_idle_timers.h>      // the every-32 breath samples
#include <runtime/inmatch/server_message_dispatch.h> // Server_ReleasePlayerDeployment, build_spawn_wave_status_body
#include <runtime/inmatch/server_net_quality.h>      // the host CNetQuality sample + the 0x46 quality resend
#include <runtime/inmatch/server_medic.h>            // the medic revive and heal transactions
#include <runtime/inmatch/server_player_death.h>     // the death queue drains and the frame tail's edges
#include <runtime/inmatch/server_powerup.h>          // the remote grant tables and the S2C 0x35 fan
#include <runtime/inmatch/server_entity_routes.h>    // the item events, crossings and guidance
#include <runtime/inmatch/server_entity_state.h>     // the per-slot 0x0A writer (Server_SendEntityStateToPlayer)
#include <runtime/inmatch/server_spawn.h>            // the admitted 0x51 spectator converts
#include <runtime/inmatch/server_tick_maintenance.h> // the per-tick and periodic-second maintenance legs

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include <base/io/le.h>                        // append_u16_le / append_u32_le
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/nw_session_framing.h> // per-player 0x61 tick-seed roll
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_hello.h>
#include <runtime/replication/entity_wire_bridge.h> // snapshot_world / GameEntitySnapshot
#include <runtime/replication/connection_fan.h>     // drain_connection_c2s / emit_connection_s2c
#include <runtime/world/world.h>
#include <runtime/world/collision.h>           // stable replication LOS view epoch
#include <runtime/world/geom.h>                // to_fixed
#include <runtime/world/powerup.h>             // the remote players' powerup grants
#include <runtime/world/minimap_overlay.h>      // portable Entity_ClassifyForMinimap result
#include <runtime/world/spawn_select.h>         // sorted SpawnZoneList index for capture events
#include <runtime/world/vehicle_motor.h>       // VehicleTraits (the 0x40 vehicle-blip icons)
#include <runtime/world/world.h>               // World::run_logic_tick
#include <runtime/world/zone_capture.h>        // the 1 Hz AS capture pass (slice 2)

namespace opennova::inmatch {

namespace {

// Route placed-device lifetimes created or retired by this authoritative tick.
// Retail's filtered send excludes the host itself (mask 0x90): the listen
// client's native World already owns the row, while every remote client needs
// the 0x59 spawn/update and the preceding-to-destroy 0x12 removal record.
// [orig: Entity_ConvertRoundToPlacedEntity @0x5455B0 -> S2C 0x59;
// Server_RemoveEntityAndNotify @0x50A270 -> S2C 0x12]
static void send_throwable_events(NapiNPServerCtx &ctx, const world::World &world) {
	if (!ctx.is_in_session) return;
	auto fan_remote = [&](uint8_t tag, const std::vector<uint8_t> &body) {
		for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
			if (!is_in_match(conn) || conn.link.transport == nullptr ||
					conn.link.mode == replication::TransportMode::Loopback)
				continue;
			conn.link.transport->host_send(tag, body);
		}
	};

	for (const world::ThrowableEvents::DeviceSpawn &event :
			world.throwables.events.spawns) {
		DeployedItemSpawn spawn;
		spawn.item_id = static_cast<uint16_t>(event.item_id);
		spawn.owner_handle = event.owner_handle;
		spawn.friendly_item_id = static_cast<uint16_t>(event.item_friendly);
		spawn.enemy_item_id = static_cast<uint16_t>(event.item_enemy);
		spawn.slot_handle = event.entity;
		spawn.parent_handle = event.parent_handle;
		spawn.pos_x = world::to_fixed(event.pos.x);
		spawn.pos_y = world::to_fixed(event.pos.y);
		spawn.pos_z = world::to_fixed(event.pos.z);
		// Words 12/13/14 are the high halves of the resting round's
		// entity+16/+20/+24 = the eulerZ/eulerX/eulerY triple (yaw heading,
		// pitch, roll) — the same order every spawn record carries; the client
		// stores them straight back into +16/+20/+24 [orig: the word reads
		// [esi+12h]/[esi+16h]/[esi+1Ah] into record words 12/13/14,
		// Entity_UpdateSatchelPhysics @0x448aeb..0x448b09 and
		// Entity_UpdateClaymorePhysics @0x447a6c..0x447a8a;
		// Entity_SpawnOrUpdateFromSlotPacket @0x5468cb..0x5468df].
		spawn.angle_x = static_cast<uint16_t>(
				static_cast<uint32_t>(event.yaw_bam) >> 16);
		spawn.angle_y = static_cast<uint16_t>(
				static_cast<uint32_t>(event.pitch_bam) >> 16);
		spawn.angle_z = static_cast<uint16_t>(
				static_cast<uint32_t>(event.roll_bam) >> 16);
		fan_remote(s2c::DEPLOYED_ITEM, encode_deployed_item_spawn(spawn));
	}
	for (const world::ThrowableEvents::DeviceRemove &event :
			world.throwables.events.removes) {
		EntityRemove removal;
		removal.entity_handle = event.entity;
		fan_remote(s2c::ENTITY_REMOVE, encode_entity_remove(removal));
	}
}

// The routed records are released: a frame whose entity update is held
// produces none and must not re-send the last one's.
void route_throwable_events(NapiNPServerCtx &ctx, world::World &world) {
	send_throwable_events(ctx, world);
	world.throwables.events.clear();
}

// Send the WAC commands the VM replicated this tick as S2C 0x23. A targeted
// record (send_mask 0x20) reaches the one connected, not-dropped connection
// whose active player slot owns the addressed entity, in-match or not; a
// broadcast record (send_mask 0x90) reaches every in-match remote and never
// the listen host, whose VM already ran the handler. A session-less local
// game has no recipient and the queue is simply released.
// [orig: WacScript_ExecuteBytecode @0x4F58B0 — SendFiltered(0x23) @0x4f5e74 /
//  @0x4f5ed1; NapiNPServer_SendFiltered @0x4C87E0 — mask 0x20 target slot
//  active/not-dropped/connected @0x4c8a06..0x4c8a38, mask 0x10 host exclusion
//  @0x4c88f6, mask 0x80 state 6/7 gate @0x4c893e]
void route_script_remote_commands(NapiNPServerCtx &ctx, world::World &world) {
	for (const world::ScriptRemoteCommand &command : world.out.script_remote_commands) {
		ScriptRemoteCommand wire;
		wire.command_index = command.command_index;
		wire.args.reserve(command.args.size());
		for (const world::ScriptRemoteArg &arg : command.args)
			wire.args.push_back({static_cast<uint32_t>(arg.value), arg.text});
		const std::vector<uint8_t> body = encode_script_remote_command(wire);
		for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
			if (conn.link.transport == nullptr) continue;
			if (command.targeted) {
				if (conn.link.owned_entity != command.target ||
						conn.host_disconnect_sent)
					continue;
			} else if (!is_in_match(conn) ||
					conn.link.mode == replication::TransportMode::Loopback) {
				continue;
			}
			conn.link.transport->host_send(s2c::SCRIPT_REMOTE_COMMAND, body);
		}
	}
	world.out.script_remote_commands.clear();
}

// Drain the match domain's objective transitions through retail's two wire
// lanes. Pickup/drop/save/return and non-CTF capture publish the complete 19-B
// flag state (0x2F). A CTF capture retires the captured flag with 0x12 instead.
// Pickup/save/capture/timeout-return also precede that state mutation with the
// corresponding 8-B 0x1E event record.
// The event and 0x2F state use active-player mask 0x80 (host included); CTF's
// following 0x12 removal uses 0x90 (host excluded).
// [orig: Entity_AttachCarriedObject @0x43C130 -> Server_HandleEntityDeath @0x517460
// (pickup event 0x14) -> Server_SendDestructibleDeathPacket @0x50D900;
// Server_BroadcastEntityDeathEvent @0x517A90 (save event 0x15 then 0x2F);
// Server_ProcessScoringAndBroadcast @0x5169C0 (capture event 0x13 then CTF
// remove / other-mode reset); Entity_UpdateIdleCheck @0x408430 ->
// Server_BroadcastOverlayDeathEvent @0x50F5A0 (return 0x23/0x24/0x25 then 0x2F);
// Server_RemoveEntityAndNotify @0x50A270]
void route_match_gameplay_events(NapiNPServerCtx &ctx, world::World &world) {
	std::vector<world::MatchGameplayEvent> events =
			world.match.drain_gameplay_events();
	if (!ctx.is_in_session || events.empty()) return;

	for (const world::MatchGameplayEvent &event : events) {
		uint8_t feed_type = 0;
		uint8_t feed_actor = 0xFF;
		world::EntityPosition feed_position;
		switch (event.kind) {
		case world::MatchGameplayEventKind::FlagPickup:
			feed_type = 0x14;
			feed_actor = world::pool0_index_byte(event.actor);
			feed_position = event.position;
			break;
		case world::MatchGameplayEventKind::FlagSave:
			feed_type = 0x15;
			feed_actor = world::pool0_index_byte(event.actor);
			feed_position = event.position;
			break;
		case world::MatchGameplayEventKind::FlagCapture:
			feed_type = 0x13;
			feed_actor = world::pool0_index_byte(event.actor);
			feed_position = event.position;
			break;
		case world::MatchGameplayEventKind::FlagReturn:
			if (event.objective_item_id == 4091) feed_type = 0x23;
			else if (event.objective_item_id == 4093) feed_type = 0x24;
			else if (event.objective_item_id == 4095) feed_type = 0x25;
			break;
		case world::MatchGameplayEventKind::FlagDrop:
			break;
		}
		std::vector<uint8_t> feed;
		if (feed_type != 0) {
			feed.push_back(feed_type);
			feed.push_back(feed_actor);
			feed.push_back(0xFF);
			feed.push_back(0xFF);
			io::append_u16_le(feed, static_cast<uint16_t>(
					static_cast<uint32_t>(feed_position.x.q16) >> 16));
			io::append_u16_le(feed, static_cast<uint16_t>(
					static_cast<uint32_t>(feed_position.y.q16) >> 16));
		}

		std::vector<uint8_t> state_body;
		if (!event.remove_objective) {
			ObjectiveEntityState state;
			state.entity_handle = event.objective.packed;
			state.flags_byte = event.objective_flags;
			// The flag's words, raw (D-NET-507). [orig: NetPacket_SerializeEntityWithParentAndTarget
			//  @0x505810 - entity+4 / +8 / +0xC @0x505878 / @0x505889 / @0x50589F]
			state.pos_x = event.objective_position.x.q16;
			state.pos_y = event.objective_position.y.q16;
			state.pos_z = event.objective_position.z.q16;
			state.attach_handle = event.parent.packed;
			state.ground_handle = event.ground.packed;
			state_body = encode_objective_entity_state(state);
		}

		for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
			if (!active_player_recipient(conn)) continue;
			if (!feed.empty())
				conn.link.transport->host_send(s2c::GAME_EVENT, feed);
			if (event.remove_objective) {
				if (conn.link.mode == replication::TransportMode::Loopback) continue;
				EntityRemove removal;
				removal.entity_handle = event.objective.packed;
				conn.link.transport->host_send(
						s2c::ENTITY_REMOVE, encode_entity_remove(removal));
			} else {
				conn.link.transport->host_send(
						s2c::OBJECTIVE_ENTITY_STATE, state_body);
			}
		}
	}
}

// The server win-condition check, on the original's 1 Hz periodic cadence [orig:
// Server_CheckWinConditions @0x51ad40, called from the periodic-second block in
// Server_TickUpdate @0x51D7E0 (the call @0x51df5a)]. The round-over latch no-ops
// it [orig: @0x51ad4a].
// SP carries exactly ONE auto condition: the local player is DEAD and the mission
// does not allow SP-respawn (attrib 0x40) -> Server_ProcessRoundEnd(2) — every other
// SP outcome comes from the WAC win/lose handlers or the BMS Blue/Red/GreenWin
// actions [orig: @0x51ad6f]. Multiplayer delegates the witnessed uniform-zone
// check and the complete game-type switch to world::Match.
void check_win_conditions(NapiNPServerCtx &ctx, world::World &world) {
	(void)ctx;
	if (world.match.outcome().ended) return;
	if (world.rules.mp_session) {
		if (const std::optional<int32_t> winner =
					world.match.winner_if_finished(world);
				winner.has_value())
			world.process_round_end(*winner);
		return;
	}
	// mp_session (not ctx.is_in_session — always 1 on our listen server) is the
	// retail SP discriminator.
	const world::Entity *local = world.registry.get(world.cached.local_player);
	if (local == nullptr) return;
	// The local player's dead bit alone, never its health word
	// [orig: Server_CheckWinConditions `test byte ptr [eax+24h], 2` @0x51AD5E].
	const bool dead = (local->flags & 2u) != 0;
	if (dead && (world.tables.mission_attrib_flags & world::MissionTables::kMissionAttribSinglePlayerRespawn) == 0)
		world.process_round_end(2);
}

bool announce_round_end(NapiNPServerCtx &ctx, world::World &world) {
	if (!world.rules.mp_session || ctx.round_end_announced ||
			!world.match.result().ready)
		return false;
	const world::MatchResult &result = world.match.result();
	// The board stream is frozen ONCE here, before the per-slot push; the C2S
	// 0x2B service only cuts chunks from it.
	// [orig: Server_ProcessRoundEnd @0x5164F0 (the
	// Server_BuildEndOfRoundScoreboard(1, winTeam) call @0x516590)]
	ctx.round_end_board_stream =
			encode_end_round_stats(build_end_round_stats(result));
	// The header form is session state: the in-session non-team (DM/KOTH
	// family) header carries the top three frozen-board names/scores instead
	// of the winner/team-score words. world.rules.mp_session is our SP-as-listen-
	// server stand-in for the retail is_in_session (ctx.is_in_session is
	// always 1 here — the in-process loopback IS a session).
	// [orig: EndRoundScoreboard_SerializeHeader form pick @0x5052a6]
	const bool non_team_header =
			world.rules.mp_session && (result.game_type & 0x10000u) == 0;
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (!is_in_match(conn) || conn.link.transport == nullptr) continue;
		// The zero 0x61 precedes each recipient-specific 0x1D header, then the
		// player enters game-state 11. Match's sole outcome gate supplies the
		// original slot-state-7 replication stop without duplicating lifecycle
		// state. The client pulls 0x56 independently, so no board chunk is pushed.
		// [orig: Server_ProcessRoundEnd @0x516790..0x51685E]
		conn.link.transport->host_send(
				s2c::TICK_SEED, Server_DisarmPlayerTickSeed(conn, world.logic_tick));
		conn.link.transport->host_send(
				s2c::END_ROUND_HEADER,
				encode_end_round_header(
						build_end_round_header(result, conn.reply.player_slot,
								non_team_header),
						non_team_header));
		conn.burst.game_state = 11;
	}
	// The round tallies: a Team Deathmatch, Team KOTH or CTF round counts,
	// and the winning side 1..4 scores a win [orig: Server_ProcessRoundEnd
	// @0x516883..0x5168d8 — `add g_TotalRoundsPlayed, 1` @0x5168a7, the
	// winner switch @0x5168ad..0x5168d8].
	if (result.game_type == 0x10000u || result.game_type == 0x10001u ||
			result.game_type == 0x10004u) {
		++ctx.rounds_played;
		if (result.winner_team >= 1 && result.winner_team <= 4)
			++ctx.round_wins[static_cast<size_t>(result.winner_team - 1)];
	}
	ctx.round_end_announced = true;
	ctx.round_end_linger_ticks = round_end_linger_ticks_for(ctx.config.title);
	// A ServerCommand Cycle / EndMission / GameOver overrides the stored
	// linger right after the round end [orig: CNapiGameSession_HandleServerCommand @0x4D31CA].
	if (ctx.round_end_linger_override_ticks != 0) {
		ctx.round_end_linger_ticks = ctx.round_end_linger_override_ticks;
		ctx.round_end_linger_override_ticks = 0;
	}
	return true;
}

// A refused touch arms its slot's nag: not while one is held, and only when the
// slot's +100360 stamp is set and over ten seconds old; it sets bits
// 0x04|0x08 and restamps the word.
// [orig: Server_OnPlayerTouchCaptureZone @0x500BA0 — the slot
//  @0x500BE4..0x500BF1, bit 0x08 @0x500C10, the stamp @0x500C19..0x500C33, the
//  set @0x500C35..0x500C3C]
void arm_refused_capture_nags(NapiNPServerCtx &ctx, world::World &world) {
	const uint32_t now_ms = io::host_milliseconds_for_logic_tick(world.logic_tick);
	for (const world::EntityHandle toucher : world.zones.capture.refused_touches) {
		for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
			if (!conn.link.owned_entity.valid() || conn.link.owned_entity != toucher)
				continue;
			SessionReplyState &reply = conn.reply;
			if (!reply.capture_nag_held && reply.chat_last_ms != 0 &&
					now_ms - reply.chat_last_ms > 10000u) {
				reply.frontier_hint_pending = true;
				reply.capture_nag_held = true;
				reply.chat_last_ms = now_ms;
			}
			break;
		}
	}
	world.zones.capture.refused_touches.clear();
}

// EntityPool_ClearDirtyFlags: strip the priority-target mark (Flags 0x4000)
// from every used row of pools 0 and 1 — no alive/flags test, pools 2..4
// untouched. Both portable views of the Flags dword carry the bit because
// both writers set it (ai_combat.cpp / infantry_combat.cpp).
// [orig: EntityPool_ClearDirtyFlags @0x508E30 — pool 0 loop @0x508e59, pool 1
//  loop @0x508e79, `&= ~0x4000`]
void clear_priority_target_marks(world::World &world) {
	for (const int pool : {0, 1}) {
		const size_t capacity = world.registry.pool_capacity(pool);
		for (size_t slot = 0; slot < capacity; ++slot) {
			world::Entity *e = world.registry.get(
					world::EntityHandle::make(pool, static_cast<int>(slot)));
			if (e == nullptr) continue;
			e->flags &= ~world::kEntityFlagPriorityTarget;
			e->engine_flags &= ~world::kEntityFlagPriorityTarget;
		}
	}
}

} // namespace

std::vector<uint8_t> Server_RerollPlayerTickSeed(
		NapiNPConnection &connection) {
	connection.tick_seed =
			((make_random_session_u32() & 0xFEu) + 1u) << 16;
	connection.fire_tick_floor = connection.tick_seed;
	connection.fire_disarmed_at = 0;
	connection.fire_tick_mode = true;
	std::vector<uint8_t> body;
	body.reserve(4);
	io::append_u32_le(body, connection.tick_seed);
	return body;
}

// The enable==0 arm: clear the slot's seed/freshness stamp and ship the
// four-zero 0x61. [orig: Server_SendRandomSeedToPlayer @0x5101A0,
// enable==0 arm @0x510237..0x510278]
std::vector<uint8_t> Server_DisarmPlayerTickSeed(
		NapiNPConnection &connection, uint32_t host_tick) {
	connection.fire_disarmed_at = host_tick;
	connection.fire_tick_mode = false;
	connection.tick_seed = 0;
	return std::vector<uint8_t>(4, 0);
}

// The comparisons are signed x86 compares, including wrap at bit 31. A
// zero client tick never passes, even during the death/round-end grace.
// [orig: PlayerSlot_IsActive @0x4FC760]
bool Server_AcceptsPlayerFireTick(const NapiNPConnection &connection,
		uint32_t client_tick, uint32_t host_tick, uint32_t send_holdoff_ticks) {
	if (client_tick == 0) return false;
	if (connection.fire_tick_mode)
		return connection.fire_tick_floor != 0 &&
				static_cast<int32_t>(client_tick) > static_cast<int32_t>(connection.fire_tick_floor);
	const uint32_t deadline = connection.fire_disarmed_at + 3u * send_holdoff_ticks;
	return connection.fire_disarmed_at != 0 &&
			static_cast<int32_t>(host_tick) < static_cast<int32_t>(deadline);
}

// The records the entity update produces, routed at the head of the next
// server tick: retail's entity update sends them inline after that frame's
// send pump, so they lead the next frame's queue. The placed-device records,
// the deaths and their tallies, the medic interactions the kill-zone pass
// admitted (after the deaths it also produced), the match's gameplay events and
// the water crossings. (The guidance waits behind this tick's 0x0A.)
// [orig: Game_ProcessMainFrame — the Entity_UpdateAllEntities call @0x52674B
//  follows Server_TickUpdate's send pump @0x51E487; Projectile_ProcessExplosionQueue
//  @0x4EADFC -> GameEvent_HandleMedicInteraction @0x4E6790 ->
//  GameEvent_RevivePlayer @0x517CD0 / GameEvent_HealPlayer @0x50DE30]
void route_entity_pass_records(NapiNPServerCtx &ctx, world::World &world) {
	Server_FanEntityEvents(ctx, world);
	route_throwable_events(ctx, world);
	// The receive pump's player-body edges (the C2S 0x41's) are tested here,
	// ahead of the medic heals below, and wait for the drain after the
	// spectator converts.
	route_round_deaths(ctx, world, EdgeDrain::HoldAtHead);
	Server_RouteMedicInteractions(ctx, world);
	// Retail stored the previous frame's heal (Health = max, in that frame's
	// entity update) ahead of this pump's 0x41 (Health 0); ours routes the heal
	// only now, after the pump, so the 0x41's store goes back over it on each
	// body whose edge is held, and its death keeps the 0x41's Health 0.
	// [orig: GameEvent_HealPlayer @0x50DE7F; NapiNPServerMsg_DeathTimeoutReset
	//  @0x510568]
	for (const world::RoundDeath &d : world.round_sim.deaths)
		if (d.player_edge)
			if (world::Entity *body = world.registry.get(d.victim)) body->health = 0;
	Server_ApplyPowerupGrants(ctx.np_protocol.connection_list, world);
	Server_BroadcastWeaponOverlayUpdates(ctx, world);
	route_match_gameplay_events(ctx, world);
	Server_RouteWaterCrossings(ctx, world);
}

void Server_TickUpdate(NapiNPServerCtx &ctx) {
	// A joiner is a pure non-authority client (its frame is P5's Client_ProcessNetworkFrame); the
	// pre-World P2 unit-test path has no simulation to drive. Either way: no host frame. The host
	// tick runs under is_authority [orig: Game_ProcessMainFrame @0x5263f0 gates the call
	// `if (is_authority && !suspended) Server_TickUpdate(...)` @0x5266b4]. is_in_session (+0x58) is
	// NOT the gate here — it gates the per-frame replicate/broadcast at step (3) [D-NET-120]; the
	// C2S drain + sim tick run regardless (the orig recv/send pumps are not is_in_session-gated).
	if (ctx.world == nullptr || ctx.is_authority == 0) return;
	world::World &world = *ctx.world;
	// The previous frame's entity update sent inline, so its records lead
	// this frame's queue, ahead of every message this tick produces.
	route_entity_pass_records(ctx, world);
	// The phases lap onto the SIM_SERVER_* / SIM_MATCH / SIM_REPLICATION_*
	// rows of the world's profile (ADR 0043 d5); the script and entity passes
	// attribute their own SIM_WORLD_* / SIM_UPDATE_* rows.
	devtools::ProfileLap lap(world.profile);
	const bool round_was_announced = ctx.round_end_announced;
	// Snapshot the phase at frame entry. Retail decrements the timer later on
	// the shared second boundary, after the entity-update gate has already been
	// tested, so the 1 -> 0 transition frame remains frozen.
	// [orig: entity gate @0x51D8BD; decrement @0x51DC20..0x51DC33]
	const bool preround_active = world.preround_delay_seconds != 0;

	// (0) The head-of-function timers, ahead of the per-player walk and the
	// receive pump. The 744-tick priority-target sweep is unconditional: zero-armed
	// per mission, it fires when the countdown is (or decrements to) zero, strips
	// Flags 0x4000 off pools 0/1 and reloads 744 — the only decay for pool-1
	// shooters and for rows that never scan again.
	// [orig: Server_TickUpdate @0x51d82b..0x51d840 -> EntityPool_ClearDirtyFlags
	//  @0x508E30; no is_in_session gate]
	{
		bool sweep = world.priority_target_clear_countdown == 0;
		if (world.priority_target_clear_countdown != 0)
			sweep = --world.priority_target_clear_countdown == 0;
		if (sweep) {
			clear_priority_target_marks(world);
			world.priority_target_clear_countdown = 744;
		}
	}
	// Then the per-player maintenance walk
	// [orig: Server_UpdateAllActivePlayerSlots @0x518820, called @0x51d88b].
	update_all_active_player_slots(ctx, world);
	// The host CNetQuality send window, on its own 62-frame countdown (retail
	// samples it from the main frame beside this tick).
	Server_SampleHostNetQuality(ctx);

	// (1) net-before-logic: drain each in-match connection's queued C2S 0x0C and read-apply (SNAP).
	// burst.spawned marks an in-match connection — a mid-burst peer is still receiving its §5.2a
	// initial-state stream via tick_connections (which skips spawned peers, napi_np_protocol.cpp:509)
	// and has no per-frame C2S 0x0C uplink yet. [orig: PumpRecvQueues walks connection_list.]
	// Spawned remote-peer (type-1) nodes stay resident until explicit disconnect/drop. The only
	// whole-table reset is create_session, so a live configuration update cannot evict this fan.
	// drain_connection_c2s null-checks conn.link.transport internally + enforces the owner gate.
	// [D-NET-122] is_in_match(conn) is the single predicate shared with the emit fan below (was an
	// inline burst.spawned in each); the host's own loopback satisfies it once its §5.2a burst
	// completes, so it is drained + emitted like any peer (its C2S is empty — is_authority suppresses
	// the host's own 0x0C — but its 0x0A local view is no longer starved).
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (!is_in_match(conn)) continue;
		if (conn.discard_pre_deploy_uplinks) {
			// C2S 0x0E is applied synchronously in the receive dispatch, whereas
			// already-decoded 0x0C records wait on this transport FIFO. At a
			// successful deploy edge every queued uplink predates the release;
			// discard it before the normal drain can overwrite the selected pose.
			if (conn.link.transport != nullptr) {
				replication::Datagram stale;
				while (conn.link.transport->host_recv(stale)) {}
			}
			conn.discard_pre_deploy_uplinks = false;
		}
		replication::drain_connection_c2s(world, conn.link);
	}

	// The C2S 0x51 spectator converts the dispatcher admitted this frame run
	// inline in retail's receive dispatch, so ahead of the humans count below
	// and the state fan: a converted body is hidden before it is counted.
	// [orig: Server_TickUpdate, the receive pump call @0x51D895]
	Server_ProcessSpectatorRespawnRequests(ctx, world);
	// The receive pump's player-body edges (the C2S 0x41's Health 0) route
	// after its converts: retail's edge runs in the entity update after the
	// whole pump, and skips a body the convert hid (Flags |= 1, which the
	// convert's deploy leg keeps; it clears only the dead bit).
	// [orig: Server_ConvertPlayerToSpectator @0x519E92, Health 1 @0x519EA4, the deploy
	//  @0x519ECE; Server_DeployPlayer @0x51787A; the edge's hidden bit
	//  Entity_UpdateInfantryPlayerBody @0x4B411E..0x4B4127]
	route_round_deaths(ctx, world, EdgeDrain::AfterConverts);

	// (1b) The WAC 'humans' count, rebuilt each server tick right after the
	// receive pump, just before the script pass. The original also uses it as
	// the empty-server world-run gate (entities/WAC advance while humans > 0 ||
	// ticks == 0).
	// [orig: Server_BuildEntitySlotLists @0x4f97a0, called from Server_TickUpdate
	//  @0x51d89a before the WAC pre-pass]
	world.cached.humans = world.registry.count_humans();
	lap.mark(devtools::Slot::SIM_SERVER_INPUT);


	// (2) The script pass: the WAC tick, the every-32 legs (the spawn markers and
	// this session's breath samples) and the BMS quarter pass, under one
	// admission (the pre-round phase and the SP epilog hold them). The entity
	// update is the frame's next leg after retail's server tick: it runs at
	// step (4), after this frame's 0x0A is queued.
	// [D-NET-123] Server_TickUpdate OWNS this tick — the inverse of the legacy seam, where the
	// C2S drain ran INSIDE run_logic_tick (a net ISystem, retired P8). A binding driving the runtime
	// through Server_TickUpdate must NOT keep its own run_logic_tick() or a parallel connection-table
	// driver, or the sim advances twice per frame (and the C2S queue drains twice — header guardrail).
	// [orig: Server_TickUpdate — the admission @0x51D89F..0x51D8BD, the
	//  WacScript_AdvanceTick call @0x51D8BF, the Server_UpdatePlayerBreathTimers
	//  call @0x51D8D7, the EventTrigger_UpdateQuarterRoundRobin call @0x51D8F4]
	ServerIdleTimers idle_timers(ctx);
	world.entity_idle_timers = &idle_timers;
	const world::TickContext tick = world.begin_tick(
			/*is_authority=*/true,
			preround_active ? world::TickPhase::PreRound
			                : world::TickPhase::Gameplay);
	world.run_script_pass(tick);
	world.entity_idle_timers = nullptr;
	Server_FanEntityEvents(ctx, world);
	// WAC punts are connection descriptions, not gameplay damage or chat.
	// The original slot wrapper ignores departed/retired slots; the live
	// connection owner likewise rejects stale allocations and loopback nodes.
	// [orig: WacCmd_PlayerPunt @0x4F0DA0; WacCmd_PlayerKillPunt @0x4F0D30;
	// CNapiNPConnection_TrySendChatMessage @0x5006C0;
	// CNapiNPConnection_SendChatMessage @0x4C7EF0]
	for (const world::MatchPlayerPunt &punt : world.match.drain_player_punts()) {
		const world::Entity *entity = world.registry.get(punt.entity);
		if (entity == nullptr || entity->registry_spawn_id != punt.spawn_id ||
				world.match.player(punt.entity) == nullptr)
			continue;
		for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
			if (conn.link.owned_entity != punt.entity ||
					(conn.link.owned_entity_spawn_id != 0 &&
					 conn.link.owned_entity_spawn_id != punt.spawn_id))
				continue;
			DisconnectEvent event;
			event.ds = 1;
			event.dc = 2;
			event.dpc = punt.reason;
			event.ddstr = "wac punt";
			Server_StageHostDisconnect(conn, event);
			break;
		}
	}
	advance_play_ticks(ctx, world);
	lap.restart();
	world.match.advance_tick(
			world,
			preround_active ? world::TickPhase::PreRound
			                : world::TickPhase::Gameplay);
	lap.mark(devtools::Slot::SIM_MATCH);
	// Retail produces capture requests from exact Change Team Box contacts in
	// the movement resolver, then drains that collision-owned stream here. The
	// callback has no MoveOrder gate; snapshot-owned remote players run the same
	// authority collision tail at their final host pose. [orig: collision
	// callsite @0x4B31DD..0x4B3238;
	// Server_OnPlayerTouchCaptureZone @0x500BA0]
	if (!preround_active && ctx.is_in_session &&
			!world.match.outcome().ended) {
		world.zones.capture_contact_tick();
		arm_refused_capture_nags(ctx, world);
	}

	// (2b) Death routing + respawn release — the deaths the script pass raised get
	// their broadcasts staged before this frame's 0x0A fan (§5.60); the entity
	// pass's were routed at the head of this call.
	route_script_remote_commands(ctx, world);
	route_round_deaths(ctx, world);
	route_match_gameplay_events(ctx, world);
	// Retail drains an already-ended round here, before its periodic automatic
	// win-condition pass. WAC/BMS can end the round during the world tick above,
	// so those script-driven outcomes consume this tick; automatic MP outcomes found
	// by the check below do not. Keep the phase fact even though our countdown
	// mutation is grouped at the tail of this function.
	// [orig: Server_TickUpdate @0x51D7E0: linger drain @0x51DA04 precedes
	// the Server_CheckWinConditions call @0x51DF5A]
	const bool round_ended_at_retail_linger_phase =
			world.match.outcome().ended;

	// The pre-round seconds dword shares the ordinary 62-tick periodic
	// boundary. A non-session authority clears it there; an active session
	// decrements it once. The phase snapshot above deliberately remains true
	// for this whole frame even when this store reaches zero.
	// Match owns the one countdown (a zero-armed global that fires on the
	// first frame, then every 62); this frame's verdict was settled by
	// advance_tick above, so the world and the wire share one phase.
	// [orig: Server_TickUpdate @0x51DB6D..0x51DC33; reload 62 @0x51DB93]
	const bool periodic_second = world.match.periodic_second();
	if (periodic_second) {
		if (!ctx.is_in_session)
			world.preround_delay_seconds = 0;
		else if (world.preround_delay_seconds != 0)
			--world.preround_delay_seconds;
	}

	// Retail's per-player proximity pass (item 1 of the one-second block)
	// begins by comparing CRenderState field 0x1C (raw accumulated Points) with
	// a player-slot cache and targets reliable S2C 0x81 to that requester on
	// change. Match ran the pass itself in advance_tick; the wire half rides the
	// same service, after the authority's event scorers and before the capture
	// transaction, so zone-capture points are observed by the following pass.
	// [orig: Server_UpdateCaptureZoneProximity @0x5086A0; the active-slot loop
	// head @0x508720..0x508724; the 0x81 sync @0x508790]
	if (periodic_second) emit_requester_score_refreshes(ctx, world);

	// The periodic block's first two callees: the join-phase watchdog, then the
	// pending-player spawn pump (run by tick_connections on this same
	// boundary) [orig: @0x51DBF3 CNapiNetwork_CheckPlayerTimeouts, @0x51DBFD
	// CNapiServer_ProcessPendingPlayerSpawns].
	if (periodic_second) Server_CheckPlayerTimeouts(ctx);
	// The live group recount, the periodic block's third callee (the pending
	// spawns between them ride tick_connections on this boundary).
	// [orig: Server_TickUpdate — the EntityPool_RecountLiveByGroup call @0x51DC02]
	if (periodic_second) world.recount_group_live();
	// The 1 Hz 0x46 quality resend walk sits after the periodic block's
	// admission callees and ahead of the capture/win pass
	// [orig: Server_TickUpdate @0x51DE79..0x51DF4A, before @0x51DF50].
	if (periodic_second) Server_EmitQualityResends(ctx, world);

	// (2c) Win conditions at 1 Hz [orig: the g_PeriodicSecondTimer block in
	// Server_TickUpdate @0x51D7E0 — reload 62 @0x51db93 — calls
	// Server_CheckWinConditions @0x51AD40 (the call @0x51df5a) once per second].
	if (periodic_second) check_win_conditions(ctx, world);
	// Retail's next two periodic-second callees, in order: the violation sweep
	// (flag carry limit, MaxFriendlyKills / suicide punts) and the round-robin
	// 0x2F flag refresh, both ahead of the spawn-wave release.
	// [orig: Server_TickUpdate @0x51df5f Server_CheckPlayerViolations,
	//  @0x51df64 Server_RefreshNextFlagState, then @0x51df6e SpawnWaveList_Tick]
	if (periodic_second) {
		check_player_violations(ctx, world);
		refresh_next_flag_state(ctx, world);
	}
	tick_respawn_holds(ctx, world);
	announce_round_end(ctx, world);

	// Spawn-wave release precedes capture-zone mutation on the shared 1 Hz
	// cadence. Each release runs the same deployment transaction as an
	// immediate C2S 0x0E pick, then stages its private bundle on that player's
	// transport. [orig: SpawnWaveList_Tick @0x52A550 from Server_TickUpdate;
	// SpawnWaveList_TickEntry @0x52A330]
	// [orig: Server_TickUpdate — the in-session and round-over tests
	//  @0x51DE3E/@0x51DE58 skip only to @0x51DF50, so the spawn-wave tick
	//  @0x51DF6E runs every periodic second]
	if (periodic_second) {
		for (const world::SpawnWaveRelease &release :
				world.zones.spawn_waves.tick(world)) {
			for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
				if (!is_in_match(conn) || conn.link.transport == nullptr ||
						conn.link.owned_entity != release.player)
					continue;
				// The leg takes the zone's handle; its placement resolves it (D-NET-592)
				// [orig: SpawnWaveList_TickEntry, the Server_DeployPlayer call @0x52A3C0].
				std::vector<ProtocolMessage> deployment =
						Server_ReleasePlayerDeployment(ctx.config, conn, world, release.zone,
								release.zone.packed, ctx.logs.profile, ctx.np_protocol.identity);
				for (ProtocolMessage &message : deployment)
					conn.link.transport->host_send(
							message.tag, std::move(message.payload),
							message.reliable, message.flags.raw,
							message.capacity_exempt, message.retention_flushes);
				break;
			}
		}
	}

	// Queue per-peer retail maintenance so the requests share HostSession's next open
	// S2C boundary with the 0x0A fan above; it runs through the linger too (D-NET-654).
	emit_periodic_session_maintenance(ctx, world, periodic_second);

	// (2d) The capture transaction at 1 Hz [orig: the
	// Server_TickUpdate g_PeriodicSecondTimer block @0x51DF50..0x51DF8C: proximity ->
	// Server_UpdateCaptureZoneEntities (0x6F + 0x1E 0x3B/0x3C) -> Server_EnforceZoneEntityTeams
	// -> Server_UpdateCaptureZones (instant numbered flips + timed active entries)].
	// The world side runs in zone_capture_second_tick; this block encodes its one
	// ordered event stream without regrouping messages by tag:
	//   0x6F 15 B [u16 handle][u8 team][i32 control][i32 0x10000][i16 delta][u8 f][u8 e]
	//     [orig: NetPacket_WriteZoneTimerValue @0x506E70] — CHANGE-GATED to all in-match
	//     conns + the full set at 1 Hz to deploy-pending/dead ones (the golden carries
	//     0x6F in deploy-window bursts, not a steady per-second stream; D-NET-162);
	//   0x50 6 B [u16 handle][u8 team][u16 netId][u8 animSlot] for every actual
	//     ownership mutation, including the ordered team-0/new-team instant pair;
	//     non-player identity is zero [orig: Server_ChangeEntityTeam @0x518D70;
	//     NetPacket_WriteEntityHandlePacket @0x506AD0];
	//   0x1E 8 B zone events [orig: GameEvent_BuildPayload @0x5054E0]: 0x3B/0x3C secure
	//     edges (attacker = sorted spawn-zone-list index; victim = zone team); a
	//     numbered flip's pair [zone number][rank] 51 to the capturer's team and 50
	//     to its enemy when the enemy mask held, else 53/52 [zone number][the
	//     capturer's frontier]; then the 56/57 banner [capturer index] keyed on the
	//     zone's new owner to all [orig: GameEvent_FlagCapture @0x50F6F0];
	//   0x53 9 B for ACTIVE unnumbered captures [u16 handle][u8 curTeam][u8 capTeam]
	//     [u16 progress][u16 limit][u8 rate], and 0x6C [u16 handle][u8 presence]
	//     when the unique contact rate changes [orig: NetPacket_WriteZoneTimerWindow
	//     @0x506D00; CaptureCtx_UpdateActiveCaptureRate @0x53B600]. Numbered instant
	//     flips do not emit 0x53 in Server_UpdateCaptureZones @0x53B8F0.
	// The independent general 0x40 minimap-overlay producer runs above at its
	// retail 14-tick cadence. It is intentionally not gated on this AS chain.
	// Every periodic second, in every game type and through the post-round
	// linger. [orig: Server_TickUpdate — the in-session and round-over tests
	//  @0x51DE3E/@0x51DE58 skip only to @0x51DF50; the calls
	//  @0x51DF73/@0x51DF7D/@0x51DF87]
	if (periodic_second) {
		world::ZoneCaptureEvents ev;
		world.zones.capture_second_tick(ev);
		// The capture scoring: a numbered flip's event-24 recipients, an
		// unnumbered flip's or timed completion's event 14 on the capturer.
		// [orig: CaptureZone_CheckProximityScoring @0x500C50 — event 24
		//  @0x500D84, event 14 @0x500DC5]
		for (const world::ZoneCaptureEvents::Event &event : ev.ordered) {
			if (const auto *flip =
						std::get_if<world::ZoneCaptureEvents::Flip>(&event)) {
				world.match.record_zone_capture(world, flip->scorers);
				if (flip->takeover)
					world.match.record_psp_takeover(world, flip->capturer);
			} else if (const auto *completion =
						std::get_if<world::ZoneCaptureEvents::TimedCompletion>(&event)) {
				if (completion->takeover)
					world.match.record_psp_takeover(world, completion->capturer);
			}
		}

		const world::SpawnZoneRegistry spawn_zones =
				world.zones.build_spawn_zone_list();
		auto zone_index_of = [&](world::EntityHandle h) -> uint8_t {
			const int index = world::spawn_zone_index_of(spawn_zones, h);
			return index >= 0 && index <= 0xFE
					? static_cast<uint8_t>(index)
					: uint8_t{0xFF};
		};
		auto event_body = [](uint8_t ev_type, uint8_t attacker, uint8_t victim) {
			return std::vector<uint8_t>{ev_type, attacker, victim, 0xFF, 0, 0, 0, 0};
		};
		auto send_all = [&](uint8_t tag, const std::vector<uint8_t> &body,
		                    bool reliable = true) {
			for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
				if (!is_in_match(conn) || conn.link.transport == nullptr) continue;
				conn.link.transport->host_send(tag, body, reliable);
			}
		};
		// Mask 0x180: in-match slots whose own team byte, zero-extended, is the
		// filter dword (no entity read). [orig: NapiNPServer_SendFiltered bits
		//  0x80/0x100, `movzx eax, [esi+1A0h]` @0x4C896A..0x4C8977]
		auto send_team = [&](int32_t filter, const std::vector<uint8_t> &body) {
			for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
				if (!is_in_match(conn) || conn.link.transport == nullptr ||
						static_cast<int32_t>(conn.assigned_team) != filter)
					continue;
				conn.link.transport->host_send(0x1E, body);
			}
		};

		// Consume the semantic stream once, in retail callsite order. In
		// particular 0x50 neutral/new pairs must remain ahead of their 0x1E
		// announcement, and timed completion is 0x53 -> 0x50 -> 0x1E.
		for (const world::ZoneCaptureEvents::Event &event : ev.ordered) {
			if (const auto *control =
						std::get_if<world::ZoneCaptureEvents::Control>(&event)) {
				std::vector<uint8_t> body;
				io::append_u16_le(body, control->zone.packed);
				body.push_back(control->team);
				const uint32_t value = static_cast<uint32_t>(control->control);
				body.push_back(static_cast<uint8_t>(value & 0xFF));
				body.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
				body.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
				body.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
				body.push_back(0x00); // fixed 0x10000 limit [orig: @0x506E9D]
				body.push_back(0x00);
				body.push_back(0x01);
				body.push_back(0x00);
				io::append_u16_le(body, static_cast<uint16_t>(control->delta));
				body.push_back(control->friendlies);
				body.push_back(control->enemies);
				auto cached = ctx.zone_6f_cache.find(control->zone.packed);
				const bool changed = cached == ctx.zone_6f_cache.end() ||
						cached->second != body;
				if (changed) ctx.zone_6f_cache[control->zone.packed] = body;
				for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
					if (!is_in_match(conn) || conn.link.transport == nullptr) continue;
					bool dead = false;
					if (conn.link.owned_entity.valid()) {
						const world::Entity *entity =
								world.registry.get(conn.link.owned_entity);
						dead = entity != nullptr && entity->health <= 0;
					}
					if (changed || conn.link.respawn_pending || dead)
						conn.link.transport->host_send(
								s2c::ZONE_TIMER_VALUE, body,
								/*reliable=*/false);
				}
				continue;
			}
			if (const auto *secure =
						std::get_if<world::ZoneCaptureEvents::Secure>(&event)) {
				send_all(0x1E, event_body(secure->secured ? 0x3B : 0x3C,
				                            zone_index_of(secure->zone),
				                            secure->zone_team));
				continue;
			}
			if (const auto *change =
						std::get_if<world::ZoneCaptureEvents::TeamChange>(&event)) {
				TeamAssign assign;
				assign.entity_handle = change->entity.packed;
				assign.team = change->team;
				assign.net_id = change->net_id;
				assign.anim_slot = change->anim_slot;
				send_all(s2c::TEAM_ASSIGN, encode_team_assign(assign));
				// A zone flip runs the same Server_ChangeEntityTeam, so the zone
				// joins the late-joiner team-change list too
				// [orig: CBufferList_AddOrFind @0x518EEC].
				std::vector<world::EntityHandle> &changed = ctx.team_change_entities;
				if (std::find(changed.begin(), changed.end(), change->entity) == changed.end())
					changed.push_back(change->entity);
				continue;
			}
			if (const auto *window =
						std::get_if<world::ZoneCaptureEvents::TimerWindow>(&event)) {
				std::vector<uint8_t> body;
				io::append_u16_le(body, window->zone.packed);
				body.push_back(window->current_team);
				body.push_back(window->capturing_team);
				io::append_u16_le(body, window->progress);
				io::append_u16_le(body, window->limit);
				body.push_back(window->rate);
				send_all(s2c::ZONE_TIMER_WINDOW, body);
				continue;
			}
			if (const auto *presence =
						std::get_if<world::ZoneCaptureEvents::Presence>(&event)) {
				std::vector<uint8_t> body;
				io::append_u16_le(body, presence->zone.packed);
				body.push_back(presence->count);
				send_all(s2c::ZONE_PRESENCE_COUNT, body);
				continue;
			}
			if (const auto *start =
						std::get_if<world::ZoneCaptureEvents::TimedStart>(&event)) {
				send_all(0x1E, event_body(start->team == 1 ? 41 : 42,
				                            world::pool0_index_byte(start->capturer),
				                            0xFF));
				continue;
			}
			if (const auto *completion =
						std::get_if<world::ZoneCaptureEvents::TimedCompletion>(&event)) {
				if (completion->announce)
					send_all(0x1E, event_body(
							completion->new_team == 1 ? 43 : 44,
							world::pool0_index_byte(completion->capturer), 0xFF));
				continue;
			}
			const auto *flip =
					std::get_if<world::ZoneCaptureEvents::Flip>(&event);
			if (flip == nullptr || !flip->announce) continue;
			const uint8_t capturer_idx = world::pool0_index_byte(flip->capturer);
			if (!flip->numbered) {
				// An unnumbered instant flip announces as a completion does.
				// [orig: GameEvent_FlagCapture @0x50F936..0x50F94B, the send
				//  @0x50F991]
				if (flip->new_team == 1 || flip->new_team == 2)
					send_all(0x1E, event_body(flip->new_team == 1 ? 43 : 44,
					                          capturer_idx, 0xFF));
				continue;
			}
			// The pair, unless the round is decided or the capturer holds no
			// slot: the capturer's team, then enemy_of(that team).
			// [orig: GameEvent_FlagCapture — the decided test @0x50F764, the
			//  slot @0x50F7BA, the capturer-team filter @0x50F7E1, 51/53
			//  @0x50F8A5/@0x50F82B, the enemy filter @0x50F851/@0x50F8D9, 50/52
			//  @0x50F8E9/@0x50F872, the sends @0x50F830/@0x50F8AA/@0x50F907]
			// Before the pair every slot's refused-touch hold clears; after it
			// the capturer's +100360 stamp is set.
			// [orig: GameEvent_FlagCapture — the clear @0x50F786..0x50F7B1, the
			//  stamp @0x50F90C..0x50F912]
			if (!flip->decided) {
				for (NapiNPConnection &conn : ctx.np_protocol.connection_list)
					conn.reply.capture_nag_held = false;
			}
			if (!flip->decided && flip->capturer_is_player) {
				// The capturer ENTITY's team byte, sign-extended, then 2 for
				// team 1 and 1 otherwise [orig: `movsx ecx, [esi+162h]`
				//  @0x50F7D6; `sub cl,1 / neg cl / sbb / add 2` @0x50F83B..0x50F84E].
				const int32_t own_team = static_cast<int8_t>(flip->capturer_team);
				const int32_t enemy_team = flip->capturer_team == 1 ? 2 : 1;
				send_team(own_team, flip->unchanged
						? event_body(51, flip->zone_number, flip->rank)
						: event_body(53, flip->zone_number, flip->frontier));
				send_team(enemy_team, flip->unchanged
						? event_body(50, flip->zone_number, flip->rank)
						: event_body(52, flip->zone_number, flip->frontier));
				for (NapiNPConnection &conn : ctx.np_protocol.connection_list)
					if (conn.link.owned_entity.valid() &&
							conn.link.owned_entity == flip->capturer)
						conn.reply.chat_last_ms =
								io::host_milliseconds_for_logic_tick(world.logic_tick);
			}
			// The banner, keyed on the zone's new owner, goes to every in-match
			// player even once the round is decided.
			// [orig: @0x50F919..0x50F991]
			if (flip->new_team == 1 || flip->new_team == 2)
				send_all(0x1E, event_body(flip->new_team == 2 ? 57 : 56,
				                          capturer_idx, 0xFF));
		}
	}

	// (2c) Spawn-wave status: requester-specific S2C 0x6E at 1 Hz to every
	// PENDING or DEAD in-match player. [orig: Server_TickUpdate
	// @0x51e089 emits NetPacket_WriteSpawnWaveStatus @0x507490 at 1 Hz; the recipient mask
	// includes the respawn-pending bit4 (slot+89912 & 0x10 @0x5074c2) and dead players;
	// golden ASH_I5A deploy window carries 0x6E ×11 at ~1 Hz]
	if (ctx.is_in_session && !world.match.outcome().ended &&
			periodic_second) {
		for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
			if (!is_in_match(conn) || conn.link.transport == nullptr) continue;
			bool dead = false;
			if (conn.link.owned_entity.valid()) {
				const world::Entity *e = world.registry.get(conn.link.owned_entity);
				dead = e != nullptr && e->health <= 0;
			}
			if (conn.link.respawn_pending || dead)
				conn.link.transport->host_send(
						s2c::SPAWN_WAVE_STATUS,
						build_spawn_wave_status_body(
								world, conn), // the slot's rows (D-NET-581)
						/*reliable=*/false);
		}
	}

	// The last statement of retail's periodic-second block: every player's kit
	// weight is recomputed from its live rows [orig: Server_TickUpdate @0x51e1ab
	// -> Server_RecalculateAllPlayerScores @0x5014E0].
	if (periodic_second)
		Server_RecalculateAllPlayerKitWeights(ctx.np_protocol.connection_list, world);

	// The in-session team-mode downed-state resend, on its own 62-tick countdown
	// [orig: Server_TickUpdate @0x51E2D0..0x51E378].
	Server_ResendTeamDownedStates(ctx, world);

	lap.mark(devtools::Slot::SIM_SERVER_RULES);

	// (3) The per-slot 0x0A: built from the world as the script pass and every
	// maintenance leg above left it, and queued behind their messages (the
	// capture-witnessed [0x16][0x31][0x79][..][0x0A] datagram order). The
	// entity update runs only after this at step (4), so every 0x0A precedes
	// this frame's motor step. SESSION-ONLY [D-NET-120]: the original's
	// per-frame replicate/broadcast blocks are each gated on is_in_session
	// (+0x58) inside Server_TickUpdate (@0x51d9ab..0x51e3f3), while the C2S recv
	// pump above is not — so a World kept alive past match-end (is_in_session
	// 0, world non-null) keeps ticking but stops fanning ghost 0x0A frames.
	// [orig: Game_ProcessMainFrame @0x5263F0 — Server_TickUpdate (gated by the
	//  jnz @0x5266B4) sends its 0x0A (the @0x51E3D6..0x51E450 per-slot block),
	//  Entity_UpdateAllEntities follows @0x52674B]
	// Build the world snapshot ONCE, then fan a per-connection-anchored 0x0A to every in-match
	// connection [orig: NapiNPServer_SendFiltered @0x4C87E0 once, SendToConn per node].
	// [D-NET-122 RESOLVED at P5] This fan uses the shared is_in_match(conn) predicate (was an inline
	// burst.spawned). The host's own type-2 loopback now satisfies it once its §5.2a burst completes,
	// so Server_TickUpdate (the SP/host driver) fans it a per-frame 0x0A — its local view is no longer
	// starved (the gap the legacy net-ISystem emit filled by emitting to every transport-bearing
	// connection). Its 0x0A anchors to its owned_entity (the host player, bound by
	// Server_BuildPlayerInfoAndAdd). An absent, freed, or lifetime-stale owner
	// emits nothing and cannot advance per-recipient frame state.
	// [orig: Server_SendEntityStateToPlayer @0x517BA0 state==6 gate, recipient
	// eye stores @0x517BF5..0x517C13, phase increment @0x517BE8]
	if (ctx.is_in_session && !world.match.outcome().ended) {
		// Every 0x0A written for a spectator slot first restamps its body, ahead
		// of the snapshot (server_entity_state.h).
		for (const NapiNPConnection &conn : ctx.np_protocol.connection_list) {
			if (!is_in_match(conn) || !conn.link.spectator) continue;
			if (conn.type == NapiNPConnection::kTypeServerSide && !conn.s2c_send_boundary_open) continue;
			restamp_spectator_body(world, conn);
		}
		// The snapshot is built only for recipients that take entity records.
		// Type-1 peers take one only at their open send boundary (see the fan below).
		bool any_record_recipient = false;
		for (const NapiNPConnection &conn : ctx.np_protocol.connection_list) {
			if (!is_in_match(conn)) continue;
			if (conn.type == NapiNPConnection::kTypeServerSide && !conn.s2c_send_boundary_open) continue;
			if (!entity_state_takes_records(world, conn)) continue;
			any_record_recipient = true;
			break;
		}
		std::vector<GameEntitySnapshot> ents;
		if (any_record_recipient) ents = snapshot_entity_states(ctx, world);
		for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
			if (!is_in_match(conn)) continue;
			// The host's type-2 loopback is an in-process seam and remains
			// full-rate (its own player takes the header-only frame inside the
			// fan). Type-1 peers receive one fresh 0x0A only when their
			// configured S2C send boundary opens; queuing all intervening
			// snapshots would burst stale frames at that boundary.
			if (conn.type == NapiNPConnection::kTypeServerSide && !conn.s2c_send_boundary_open) continue;
			// Built here and queued just below: the frame's CONTENT is the world
			// after the script pass and the maintenance, before the motor step.
			conn.frame_update_staged =
					write_entity_state(ctx, world, conn, ents, conn.staged_frame_update);
		}
	}
	// Queue the frames just built, retail's per-slot send.
	// [orig: Server_TickUpdate per-slot block @0x51E3D6..0x51E450]
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (!conn.frame_update_staged) continue;
		conn.frame_update_staged = false;
		// A slot punted by a maintenance leg above is no longer state 6 when
		// retail reaches its per-slot writer: the description record is the
		// last thing it is sent.
		// [orig: Server_SendEntityStateToPlayer @0x517BA0 state==6 gate]
		if (is_in_match(conn) && conn.link.transport != nullptr) {
			conn.link.transport->host_send(s2c::PER_FRAME_UPDATE,
					std::move(conn.staged_frame_update), /*reliable=*/false);
		}
		conn.staged_frame_update.clear();
	}

	// Retail holds the multiplayer post-round state for 2790 server ticks. Its
	// drain precedes automatic win checks, so only an outcome already present at
	// that phase (including a WAC/BMS result from this tick) consumes the first
	// count; automatic multiplayer announcements start draining next tick. At expiry
	// the authority stores its mission exit: 4 when REPLAY is on and the LASTGAME
	// toggle off, else 3. Both take the map change (inmatch/map_change.h); the
	// session stays up through it (§5.70.3).
	// [orig: store @0x5166C4; Server_TickUpdate -- the session and gate tests
	//  @0x51D9A5..0x51D9B7, the drain @0x51D9BD..0x51D9CC, the reason
	//  @0x51DB47..0x51DB63 (`cmp g_ReplayEnabled` @0x51DB47, `cmp
	//  g_LastGameToggle` @0x51DB50, 4 @0x51DB57, 3 @0x51DB63)]
	if ((round_was_announced || round_ended_at_retail_linger_phase) &&
			ctx.round_end_linger_ticks > 0) {
		--ctx.round_end_linger_ticks;
		if (ctx.round_end_linger_ticks == 0 && ctx.mission_exit_reason == kMissionExitNone) {
			const bool last_game = ctx.rotation != nullptr && ctx.rotation->last_game;
			ctx.mission_exit_reason = ctx.config.replay_enabled != 0 && !last_game
					? kMissionExitRoundOver
					: kMissionExitMapCycle;
		}
	}
	Server_RouteGuidance(ctx, world);

	emit_periodic_rtt(ctx, world);
	// The tail ages the designation table [orig: Server_TickUpdate @0x51e496].
	Server_TickDesignations(ctx.designations);
	lap.mark(devtools::Slot::SIM_SERVER_REPLICATION);

	// (4) The entity pass: Game_ProcessMainFrame runs the gated entity update
	// once Server_TickUpdate has sent this frame's 0x0A, then the weapon pump
	// and the frame's tail. Its inline sends reach the wire in the next frame,
	// routed at the head of the next call.
	// [orig: Game_ProcessMainFrame — the Server_TickUpdate call @0x5266B6, the
	//  entity gate @0x526703..0x526742, the Entity_UpdateAllEntities call
	//  @0x52674B]
	// The pickup's refusal walks read a remote picker's connection tables
	// through this seam (world/powerup.cpp).
	ServerRemoteWeaponTables remote_weapon_tables(ctx.np_protocol.connection_list);
	world.remote_weapon_tables = &remote_weapon_tables;
	ctx.entity_update_ran = tick.phase == world::TickPhase::Gameplay &&
			world.entity_update_admitted(/*is_authority=*/true);
	if (ctx.entity_update_ran) arm_frame_tail_edges(world);
	world.run_entity_pass(tick);
	world.remote_weapon_tables = nullptr;

	// (5) flush is implicit: host_send staged each 0x0A on its transport. The loopback's local client
	// reads it via client_recv / ClientReplicaPipeline::pump; a remote peer's transport outbound_ is popped +
	// framed into a 0x83 SESSION by the owner (frame_in_match_s2c). No socket I/O in engine/.
}

} // namespace opennova::inmatch
