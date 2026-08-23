#include "npruntime/server_tick.h"
#include "npruntime/end_round_protocol.h"
#include "npruntime/server_message_dispatch.h" // build_player_list_message

#include <cmath>
#include <cstdint>
#include <vector>

#include <npwire/ingame_encode.h>
#include <npwire/ingame_message_id.h>
#include <npwire/protocol_message.h>
#include <npwire/session_hello.h>
#include <netsim/entity_wire_bridge.h> // snapshot_world / GameEntitySnapshot
#include <netsim/connection_fan.h>     // drain_connection_c2s / emit_connection_s2c
#include <world/ai.h>                  // AiEntity / AiSystem::for_handle (the motor store)
#include <world/angle.h>               // bam_heading_from_mission_yaw_deg
#include <world/entity_spawn.h>        // entity_reset_to_spawn_state (respawn release)
#include <world/geom.h>                // to_fixed
#include <world/infantry.h>            // infantry_respawn_snap (the motor half of a respawn)
#include <world/minimap_overlay.h>      // portable Entity_ClassifyForMinimap result
#include <world/spawn_select.h>         // sorted SpawnZoneList index for capture events
#include <world/vehicle_motor.h>       // VehicleTraits (the 0x40 vehicle-blip icons)
#include <world/world.h>               // World::run_logic_tick
#include <world/zone_capture.h>        // the 1 Hz AS capture pass (slice 2)

#include <algorithm>
#include <string>

namespace opennova::np {

namespace {

// Pool-0 index byte for the S2C 0x1E kill-feed actor fields (§5.26: u8 pool-0 index,
// 0xFF = none).
uint8_t pool0_index_byte(uint16_t handle) {
	const world::EntityHandle h{handle};
	if (!h.valid() || h.pool() != 0) return 0xFF;
	const int slot = h.slot();
	return slot <= 0xFE ? static_cast<uint8_t>(slot) : 0xFF;
}

void put_u16le(std::vector<uint8_t> &v, uint16_t x) {
	v.push_back(static_cast<uint8_t>(x & 0xFF));
	v.push_back(static_cast<uint8_t>(x >> 8));
}

void put_u32le(std::vector<uint8_t> &v, uint32_t x) {
	v.push_back(static_cast<uint8_t>(x & 0xFF));
	v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
	v.push_back(static_cast<uint8_t>((x >> 16) & 0xFF));
	v.push_back(static_cast<uint8_t>((x >> 24) & 0xFF));
}

constexpr uint32_t kPuntCharattrSilence = 16;
constexpr uint32_t kPuntTimeSyncSilence = 24;
constexpr uint32_t kPuntDeadTooLong = 7;
constexpr uint32_t kPuntJoinDeployIdle = 35;
constexpr uint32_t kPuntSilenceLimit = 8;
constexpr uint32_t kDeadLiveTickLimit = 360;
constexpr uint32_t kJoinDeployIdleLimitMs = 360000;
constexpr uint8_t kDescriptionFlags =
		PROTOCOL_MSG_FLAG_SETTINGS_UPDATE | PROTOCOL_MSG_FLAG_LEN8;
static_assert(kDescriptionFlags == 0xA0);

// The first description event owns the connection's disconnect slot. Closing
// the gameplay predicate here prevents later producers from following it with
// an ordinary frame while HostOwner still has one chance to flush the record.
bool stage_host_disconnect(
		NapiNPConnection &conn, const DisconnectEvent &event) {
	if (conn.host_disconnect_sent || conn.type != 1 ||
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

void send_minimap_overlay_batches(NapiNPConnection &conn,
		const std::vector<world::MinimapOverlayClassification> &entries) {
	for (size_t first = 0; first < entries.size(); first += 16) {
		const size_t count = std::min<size_t>(16, entries.size() - first);
		std::vector<uint8_t> body;
		body.reserve(1 + count * 6);
		body.push_back(static_cast<uint8_t>(count));
		for (size_t i = 0; i < count; ++i) {
			const world::MinimapOverlayClassification &entry = entries[first + i];
			put_u16le(body, entry.handle);
			body.push_back(entry.icon);
			body.push_back(entry.color);
			body.push_back(entry.flags);
			body.push_back(entry.source);
		}
		conn.link.transport->host_send(
				s2c::CAPTURE_ZONE_STATE, std::move(body), /*reliable=*/false);
	}
}

// Retail invokes Server_BuildOverlayStateForPlayer every 14 host ticks, then
// advances a 0..127 phase. The dynamic pool-1 loop visits phase, phase+128, ...
// rather than sweeping all actors every invocation. This produces 00TRg's five
// one-entry EWEAP packets close together and repeats them every 1792 ticks.
void emit_minimap_overlay_state(NapiNPServerCtx &ctx, world::World &world) {
	if (!ctx.is_in_session) return;
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if ((conn.type != 1 &&
				conn.link.mode != netsim::TransportMode::Loopback) ||
				!is_in_match(conn) || conn.link.transport == nullptr)
			continue;

		SessionReplyState &reply = conn.reply;
		if (reply.minimap_overlay_cooldown != 0) {
			--reply.minimap_overlay_cooldown;
			continue;
		}

		std::vector<world::MinimapOverlayClassification> entries;
		auto append = [&](const world::Entity &e, bool persistent) {
			world::MinimapOverlayClassification entry =
					world::classify_minimap_overlay(e);
			if (!entry.visible) return;
			if (persistent) entry.flags |= 0x10;
			entries.push_back(entry);
		};

		if (reply.minimap_initial_scan_pending) {
			const size_t capacity = world.registry.pool_capacity(2);
			for (size_t slot = 0; slot < capacity; ++slot) {
				const world::Entity *e = world.registry.get(
						world::EntityHandle::make(2, static_cast<int>(slot)));
				if (e == nullptr || !world::minimap_overlay_entity_enabled(*e) ||
						(e->item_attrib & world::kItemAttribSpawnPoint) != 0)
					continue;
				append(*e, true);
			}
			reply.minimap_initial_scan_pending = false;
		}

		// SpawnPoint rows from both static and actor pools are persistent and
		// refreshed on every producer invocation.
		for (const int pool : {2, 1}) {
			const size_t capacity = world.registry.pool_capacity(pool);
			for (size_t slot = 0; slot < capacity; ++slot) {
				const world::Entity *e = world.registry.get(
						world::EntityHandle::make(pool, static_cast<int>(slot)));
				if (e == nullptr || !world::minimap_overlay_entity_enabled(*e) ||
						(e->item_attrib & world::kItemAttribSpawnPoint) == 0)
					continue;
				append(*e, true);
			}
		}

		uint8_t recipient_team = 0;
		if (const world::Entity *recipient =
					world.registry.get(conn.link.owned_entity))
			recipient_team = recipient->team;
		const size_t pool1_capacity = world.registry.pool_capacity(1);
		for (size_t slot = reply.minimap_pool1_phase;
				slot < pool1_capacity; slot += 128) {
			const world::Entity *e = world.registry.get(
					world::EntityHandle::make(1, static_cast<int>(slot)));
			if (e == nullptr || !world::minimap_overlay_entity_enabled(*e) ||
					(e->item_attrib & world::kItemAttribSpawnPoint) != 0)
				continue;
			// In a live MP session retail suppresses occupied enemy vehicles.
			if (e->item_type == 1 && e->team != 0 && e->team != recipient_team)
				continue;
			// A child EWEAP mounted under a non-Building parent is represented
			// by that carrier rather than as an independent map blip.
			if ((e->item_attrib & world::kItemAttribEweap) != 0) {
				const world::EntityHandle parent = e->emplacement_parent.valid()
						? e->emplacement_parent : e->mount_target;
				if (const world::Entity *carrier = world.registry.get(parent);
						carrier != nullptr && carrier->item_type != 5)
					continue;
			}
			append(*e, false);
		}

		send_minimap_overlay_batches(conn, entries);
		reply.minimap_pool1_phase =
				static_cast<uint8_t>((reply.minimap_pool1_phase + 1u) & 0x7Fu);
		reply.minimap_overlay_cooldown = 13;
	}
}

// Route the deaths the damage pass raised this tick [orig: health<=0 detection in the
// entity update -> Entity_CheckAndProcessDeath @0x51b550 -> GameEvent_PlayerDeath
// @0x516dd0 (players, Flags & 0x100) / the 0x13-only AI leg @0x51b573]. Emits, per
// death: the S2C 0x13 death notify `[u16 victim][u16 killerSource]`
// [orig: BuildDeathNotifyPayload @0x5036e0, mask 0x90 = alive+not-host] and — for
// player victims — the S2C 0x1E kill-feed event (§5.26 8-B body; standard-kill
// event_type 4: the original picks rand(0-2)+4 @0x517237, a presentation-only variant;
// zone-flag types (headshot 32 / vehicle 10 / knife 13) wait on the bone-hit port).
// Deferred (§5.60): 0x52 kill-detail stats, 0x54 death/wounded markers, and
// 0x32 name broadcast. Core player/team scoring is consumed here by world::Match.
// A dead HOST player (the loopback's own entity) is queued for the respawn
// release; a joiner's respawn rides its own deploy request instead.
// Route placed-device lifetimes created or retired by this authoritative tick.
// Retail's filtered send excludes the host itself (mask 0x90): the listen
// client's native World already owns the row, while every remote client needs
// the 0x59 spawn/update and the preceding-to-destroy 0x12 removal record.
// [orig: Entity_ConvertRoundToPlacedEntity @0x5455B0 -> S2C 0x59;
// Server_RemoveEntityAndNotify @0x50A270 -> S2C 0x12]
void route_throwable_events(NapiNPServerCtx &ctx, const world::World &world) {
	if (!ctx.is_in_session) return;
	auto fan_remote = [&](uint8_t tag, const std::vector<uint8_t> &body) {
		for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
			if (!is_in_match(conn) || conn.link.transport == nullptr ||
					conn.link.mode == netsim::TransportMode::Loopback)
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

// Drain the match domain's objective transitions through retail's two wire
// lanes. Pickup/drop/save/return and non-CTF capture publish the complete 19-B
// flag state (0x2F). A CTF capture retires the captured flag with 0x12 instead.
// Save/capture also precede that state mutation with the 8-B 0x1E event record,
// matching the original transaction order.
// [orig: Entity_AttachToVehicle @0x43C130 -> Server_SendDestructibleDeathPacket
// @0x50D900; Server_BroadcastEntityDeathEvent @0x517A90 (save event 0x15 then
// 0x2F); Server_ProcessScoringAndBroadcast @0x5169C0 (capture event 0x13 then
// CTF remove / other-mode reset)]
void route_match_gameplay_events(NapiNPServerCtx &ctx, world::World &world) {
	std::vector<world::MatchGameplayEvent> events =
			world.match.drain_gameplay_events();
	if (!ctx.is_in_session || events.empty()) return;

	for (const world::MatchGameplayEvent &event : events) {
		std::vector<uint8_t> feed;
		if (event.kind == world::MatchGameplayEventKind::FlagCapture ||
				event.kind == world::MatchGameplayEventKind::FlagSave) {
			feed.push_back(event.kind == world::MatchGameplayEventKind::FlagCapture
					? uint8_t{0x13} : uint8_t{0x15});
			feed.push_back(pool0_index_byte(event.actor.packed));
			feed.push_back(0xFF);
			feed.push_back(0xFF);
			put_u16le(feed, static_cast<uint16_t>(static_cast<int16_t>(
					std::lround(event.position.x))));
			put_u16le(feed, static_cast<uint16_t>(static_cast<int16_t>(
					std::lround(event.position.y))));
		}

		std::vector<uint8_t> state_body;
		if (!event.remove_objective) {
			ObjectiveEntityState state;
			state.entity_handle = event.objective.packed;
			state.flags_byte = event.objective_flags;
			state.pos_x = world::to_fixed(event.objective_position.x);
			state.pos_y = world::to_fixed(event.objective_position.y);
			state.pos_z = world::to_fixed(event.objective_position.z);
			state.attach_handle = event.parent.packed;
			state.ground_handle = event.ground.packed;
			state_body = encode_objective_entity_state(state);
		}

		for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
			if (!is_in_match(conn) || conn.link.transport == nullptr ||
					conn.link.mode == netsim::TransportMode::Loopback)
				continue;
			if (!feed.empty())
				conn.link.transport->host_send(s2c::GAME_EVENT, feed);
			if (event.remove_objective) {
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

void route_round_deaths(NapiNPServerCtx &ctx, world::World &world) {
	if (world.round_sim.deaths.empty()) return;
	for (const world::RoundDeath &d : world.round_sim.deaths) {
		// The authoritative score ledger consumes the same death transaction as
		// the kill-feed; non-roster actors are ignored by Match.
		world.match.record_death(world, d.victim, d.killer);
		// Mark the victim DEAD on the entity: Flags bit1 is the wire-dead signal — the
		// victim's OWN client learns of its death from its record byte13 bit 0x02
		// (the LOCAL apply's dead path stores the anim + zeroes Health -> the death
		// screen + the redeploy flow), and everyone else's dead-state masks read it too.
		// Cleared by entity_reset_to_spawn_state at the deploy — the wire 1->0 edge IS
		// the client spawn hook (pose snap + reset). Without this bit the victim never
		// knows it died (v33). [orig: the death path sets entity+36 bit1; §5.10 off-13
		// "bit 0x02 = DEAD/UNDEPLOYED", apply @0x4c1005-0x4c1027, edge @0x4c1109]
		if (world::Entity *victim = world.registry.get(d.victim)) {
			victim->flags |= 2u;
			victim->alive = false;
			victim->damage_state = -1;
		}
		// Who controls the victim? Player-controlled == some connection owns it — the
		// semantic behind the original's Flags & 0x100 check [orig: @0x51b55d].
		bool victim_is_player = false;
		bool victim_is_host_player = false;
		for (NapiNPConnection &c : ctx.np_protocol.connection_list) {
			if (!c.link.owned_entity.valid() || c.link.owned_entity.packed != d.victim_handle)
				continue;
			victim_is_player = true;
			victim_is_host_player = (c.link.mode == netsim::TransportMode::Loopback);
			break;
		}

		if (ctx.is_in_session) {
			std::vector<uint8_t> body13;
			put_u16le(body13, d.victim_handle);
			put_u16le(body13, d.killer_handle); // the killerSource stamp [orig: entity+704]
			std::vector<uint8_t> body1e;
			if (victim_is_player) {
				const world::Entity *victim = world.registry.get(d.victim);
				body1e.push_back(4); // standard kill [orig: @0x517237]
				body1e.push_back(pool0_index_byte(d.killer_handle));
				body1e.push_back(pool0_index_byte(d.victim_handle));
				body1e.push_back(0xFF); // aux actor: none
				const int16_t px = victim ? static_cast<int16_t>(std::lround(victim->position.x)) : 0;
				const int16_t py = victim ? static_cast<int16_t>(std::lround(victim->position.y)) : 0;
				put_u16le(body1e, static_cast<uint16_t>(px));
				put_u16le(body1e, static_cast<uint16_t>(py));
			}
			for (NapiNPConnection &c : ctx.np_protocol.connection_list) {
				if (!is_in_match(c) || c.link.transport == nullptr) continue;
				// mask 0x90 NOT_HOST: the host's in-process view skips the wire.
				if (c.link.mode == netsim::TransportMode::Loopback) continue;
				c.link.transport->host_send(s2c::ENTITY_DEATH, body13);
				if (!body1e.empty()) c.link.transport->host_send(s2c::GAME_EVENT, body1e);
			}
		}

		// Kill tallies — the SP mission-stat buckets (the epilog score screen's count
		// source) and the WAC bluekills/greenkills builtins. SP only [orig:
		// Score_ProcessKillEvent @0x4fd400 — the !is_in_session gate @0x4fd447].
		// NB: our SP-as-listen-server always runs with ctx.is_in_session=1 (the
		// in-process loopback IS a session), so the retail SP discriminator here is
		// world.mp_session — false for SP, stamped true by real MP hosts.
		// Killer == the host/local player -> the by-player buckets
		// [orig: Score_TallyKillByLocalPlayer @0x4fd160], anyone else -> the by-others
		// family [orig: Score_TallyKillByOthers @0x4fd300]. Blue/green buckets take
		// only PERSON victims (itemdef class 3 == our Organic kind) by the team byte
		// [orig: victim+354; 0 = green, 1 = blue]; a blue/green NON-person tallies
		// nothing (witnessed); any team >= 2 victim tallies as an enemy kill (the
		// original's infantry/vehicle/aircraft split folds into one count; the epilog
		// sums the split anyway). Point values (def+404, difficulty-scaled) and the
		// human-player-victim bucket (victim+534 -> 0xC846A0) are unmodeled — counts
		// only, which is what the WAC predicates and the epilog columns consume
		// (D-AI-10; world-wac-ai-re §20.4).
		if (!world.mp_session) {
			if (const world::Entity *victim2 = world.registry.get(d.victim)) {
				bool killer_is_host_player = false;
				for (NapiNPConnection &c : ctx.np_protocol.connection_list) {
					if (c.link.owned_entity.valid() &&
					    c.link.owned_entity.packed == d.killer_handle &&
					    c.link.mode == netsim::TransportMode::Loopback) {
						killer_is_host_player = true;
						break;
					}
				}
				world::MissionKillStats &ks = world.kill_stats;
				const bool person = victim2->kind == world::EntityKind::Organic;
				if (killer_is_host_player) {
					if (victim2->team == 1) {
						if (person) ++ks.bluekills_by_player;
					} else if (victim2->team == 0) {
						if (person) ++ks.greenkills_by_player;
					} else {
						++ks.enemy_kills_by_player;
					}
				} else {
					if (victim2->team == 1) {
						if (person) ++ks.team_kills_by_others;
					} else if (victim2->team == 0) {
						if (person) ++ks.friendly_kills_by_others;
					} else {
						++ks.enemy_kills_by_others;
					}
				}
			}
		}

		if (victim_is_host_player) {
			// [orig: respawn timer floor 3 / g_respawn_timeout / the 620-tick
			// recent-spawn rule @0x516ec4 — the session respawn setting is unplumbed, so
			// 620 ticks (10 s) stands in; tracked §5.60.]
			NapiNPServerCtx::PendingRespawn pr;
			pr.victim = d.victim;
			pr.due_tick = world.logic_tick + 620;
			ctx.respawn_queue.push_back(pr);
		}
	}
	world.round_sim.deaths.clear();
}

// The server win-condition check, on the original's 1 Hz periodic cadence [orig:
// Server_CheckWinConditions @0x51ad40, called from the periodic-second block in
// Server_TickUpdate @0x51df5a]. The round-over latch no-ops it [orig: @0x51ad4a].
// SP carries exactly ONE auto condition: the local player is DEAD and the mission
// does not allow SP-respawn (attrib 0x40) -> Server_ProcessRoundEnd(2) — every other
// SP outcome comes from the WAC win/lose handlers or the BMS Blue/Red/GreenWin
// actions [orig: @0x51ad6f]. Multiplayer delegates the witnessed uniform-zone
// check and the complete game-type switch to world::Match.
void check_win_conditions(NapiNPServerCtx &ctx, world::World &world) {
	(void)ctx;
	if (world.match.outcome().ended) return;
	if (world.mp_session) {
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
	const bool dead = !local->alive || (local->flags & 2u) != 0;
	if (dead && (world.mission_attrib_flags & world::World::kMissionAttribSinglePlayerRespawn) == 0)
		world.process_round_end(2);
}

bool announce_round_end(NapiNPServerCtx &ctx, world::World &world) {
	if (!world.mp_session || ctx.round_end_announced ||
			!world.match.result().ready)
		return false;
	const world::MatchResult &result = world.match.result();
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (!is_in_match(conn) || conn.link.transport == nullptr) continue;
		// The zero 0x61 precedes each recipient-specific 0x1D header, then the
		// player enters game-state 11. Match's sole outcome gate supplies the
		// original slot-state-7 replication stop without duplicating lifecycle
		// state. The client pulls 0x56 independently, so no board chunk is pushed.
		// [orig: Server_ProcessRoundEnd @0x516790..0x51685E]
		conn.link.transport->host_send(
				s2c::TICK_SEED, std::vector<uint8_t>(4, 0));
		conn.link.transport->host_send(
				s2c::END_ROUND_HEADER,
				encode_end_round_header(build_end_round_header(
						result, conn.reply.player_slot)));
		conn.burst.game_state = 11;
	}
	ctx.round_end_announced = true;
	ctx.round_end_linger_ticks = 2790;
	return true;
}

// Release due respawns: back to the spawn point at full health [orig:
// Server_ProcessPlayerDeath -> Entity_ResetToSpawnState @0x4B9610; the D-NET-66
// death/respawn teleport — a snap, never motion].
void release_due_respawns(NapiNPServerCtx &ctx, world::World &world) {
	// Respawns are gate-blocked once the round has ended — the queue simply holds
	// [orig: the respawn request path checks g_spawn_success_gate,
	// Server_ProcessClientRequestRespawn @0x519af6].
	if (world.match.outcome().ended) return;
	for (auto it = ctx.respawn_queue.begin(); it != ctx.respawn_queue.end();) {
		if (world.logic_tick < it->due_tick) {
			++it;
			continue;
		}
		world::Entity *e = world.registry.get(it->victim);
		if (e != nullptr) {
			// Respawn placement = the recorded spawn point (the D-NET-66 death/respawn
			// TELEPORT — a snap, never motion); entity_reset_to_spawn_state then re-backs
			// it up and clears the movement gate + the dead bit [orig: the deploy flow
			// places the entity, then Entity_ResetToSpawnState @0x4B9610 records Position].
			e->position = e->spawn_position;
			world::entity_reset_to_spawn_state(*e);
			e->alive = true; // the route_round_deaths dead mark lifts with the respawn
			e->hidden = false;
			e->death_anim_state = 0;
			e->corpse_timer = 0;
			// Spawn health = the item template's healthMax [orig: Entity_InitFromItemDef
			// @0x49e550; the player_item_hp mirror, D-NET-144]. Same signed-i16 gate as
			// the first spawn (player_spawn.cpp) — healthMax is a signed WORD in retail.
			if (world.player_has_item_def && world.player_item_hp != 0)
				e->health = world::retail_signed_i16(world.player_item_hp);
			else if (e->health_max > 0)
				e->health = e->health_max;
			else
				e->health = 100;
			// The listen host's own player is MOTOR-simulated, and the motor is the writer
			// of the Entity/AiEntity pose pair (finish_infantry_tick mirrors AiEntity.pos
			// into Entity.position every tick). Writing only the registry store above put
			// the player back at full health but left the body — and its death clip — at
			// the spot where it was killed, which reads as "I cannot respawn". Reset the
			// motor half to the same deployed pose so the one original store is modelled.
			world::AiEntity *ae =
					world.ai ? world.ai->for_handle(it->victim) : nullptr;
			if (ae != nullptr && ae->inf.active) {
				const int32_t pos[3] = {
						world::to_fixed(e->position.x),
						world::to_fixed(e->position.y),
						world::to_fixed(e->position.z),
				};
				world::infantry_respawn_snap(
						*ae, pos,
						world::bam_heading_from_mission_yaw_deg(e->yaw),
						e->health);
			}
		}
		it = ctx.respawn_queue.erase(it);
	}
}

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

bool network_quality_recipient(const NapiNPConnection &conn) {
	// NapiNPServer_SendFiltered first requires a connected node with a player
	// context, then its 0x80 arm accepts slot state 6 or 7. It does not exclude
	// the listen host and does not inspect entity health. `burst.spawned` is this
	// runtime's shared in-match/live-slot model, including a dead or
	// respawn-pending player. [orig: @0x4C8874..0x4C8894,
	// @0x4C893E..0x4C8953]
	return is_in_match(conn) && conn.link.transport != nullptr;
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
		put_u32le(body, static_cast<uint32_t>(score));
		conn.link.transport->host_send(
				s2c::SCORE_DELTA_SOUND, std::move(body),
				/*reliable=*/true);
	}
}

// Emit the stock host's player maintenance requests. Integrity is a global
// scoreboard-cadence broadcast and remains active while a live player holds the
// deployment UI. Network quality owns a separate global countdown. The control
// quartet has an authoritative slot-state/live-age gate and is never derived
// from a world-clock epoch.
// [orig: Server_TickUpdate @0x51D7E0 -> @0x508540; per-player quartet
// Server_UpdateAllActivePlayerSlots @0x518820]
void emit_periodic_session_maintenance(NapiNPServerCtx &ctx, world::World &world) {
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
	// The process-global clocks above run before retail's session gate, so their
	// phase survives an interval with no live match. Everything below is a
	// per-player session maintenance block and must remain silent (and must not
	// age/reset per-player state) once the session has closed.
	// [orig: Server_TickUpdate @0x51D8F9..0x51D97E]
	if (!ctx.is_in_session) return;
	const ProtocolMessage scoreboard = integrity_boundary
			? build_player_list_message(
					ctx.config, ctx.np_protocol.connection_list, &world)
			: ProtocolMessage{};

	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		SessionReplyState &reply = conn.reply;
		const world::Entity *player = integrity_player(conn, world);
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
						world.weapons.by_index(player->equipped_adm_index);
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
		if (network_quality_boundary && network_quality_recipient(conn)) {
			conn.link.transport->host_send(
					s2c::NETWORK_QUALITY, {ctx.host_network_quality});
		}
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
				put_u32le(challenge, reply.control_challenge_seed);
				conn.link.transport->host_send(
						s2c::CHARATTR_CRC_CHALLENGE, std::move(challenge));

				// Input_PackStateFlags @0x412550 packs global input modes into bits
				// 0..5/12/13. Those globals are not modeled; healthy-LAN captures are 0.
				conn.link.transport->host_send(
						s2c::INPUT_STATE_FLAGS, {0x00, 0x00}, /*reliable=*/false);

				const uint32_t host_ms =
						host_milliseconds_for_logic_tick(world.logic_tick);
				if (reply.time_sync_host_baseline_ms == 0)
					reply.time_sync_host_baseline_ms = host_ms;
				if (reply.time_sync_round_host_ms == 0) {
					++reply.time_sync_sequence;
					reply.time_sync_round_host_ms = host_ms;
				}
				std::vector<uint8_t> time_sync;
				put_u32le(time_sync, reply.time_sync_sequence);
				conn.link.transport->host_send(
						s2c::TIME_SYNC_PING, std::move(time_sync));

				// The dedicated path does not query renderer state or send 0x68.
				// Non-dedicated hosts must have an explicitly installed viewport seam;
				// zero height suppresses the request rather than inventing one.
				if (ctx.connection_mode != ConnectionMode::HostOnly &&
						ctx.loaded_model_viewport_height != 0) {
					reply.loaded_model_page_cursor += 50u;
					if (reply.loaded_model_page_cursor >=
							ctx.loaded_model_viewport_height)
						reply.loaded_model_page_cursor = 0;
					std::vector<uint8_t> model_page;
					put_u32le(model_page, reply.loaded_model_page_cursor);
					conn.link.transport->host_send(
							s2c::LOADED_MODEL_PAGE_REQUEST, std::move(model_page),
							/*reliable=*/false);
				}
			}
		}

		// A remote player held in the join-time state-byte 0x10 deployment
		// state for strictly more than six minutes receives t35. This is based
		// on the state-6 entry timestamp, not world ticks or entity damage time,
		// and remains eligible before the initial burst reaches InMatch.
		// [orig: Server_TickUpdate @0x51E109, compare 0x57E40]
		const bool join_deploy_idle_gate = ctx.is_in_session &&
				!conn.host_disconnect_sent && conn.type == 1 &&
				conn.phase >= ConnectionPhase::PlayerAdded &&
				conn.phase < ConnectionPhase::Goodbye &&
				conn.link.transport != nullptr &&
				conn.link.owned_entity.valid() &&
				world.registry.get(conn.link.owned_entity) != nullptr &&
				conn.link.respawn_pending &&
				reply.state6_entry_host_ms_valid;
		if (join_deploy_idle_gate &&
				static_cast<uint32_t>(ctx.np_protocol.host_run_duration_ms -
						reply.state6_entry_host_ms) > kJoinDeployIdleLimitMs) {
			stage_host_punt(conn, kPuntJoinDeployIdle);
			continue;
		}
		// The dead-age arm owns an independent consecutive-tick counter. It does
		// not derive elapsed time from Entity::death_tick: retail increments the
		// player-slot dword once per state-6 tick while Flags bit 0x02 is set,
		// resets it as soon as the bit clears, and compares after the increment.
		// The remaining retail exclusions (local player, bot/spectator, explicit
		// anti-cheat bypass) have no remote-player representation in this runtime;
		// a normal type-1 connection corresponds to all of them being clear.
		// [orig: counter @0x51E066..0x51E07D; punt @0x51E187..0x51E18E]
		const bool dead_state6 = ctx.is_in_session &&
				!conn.host_disconnect_sent && conn.type == 1 &&
				conn.phase >= ConnectionPhase::PlayerAdded &&
				conn.phase < ConnectionPhase::Goodbye &&
				age_player != nullptr &&
				((age_player->flags | age_player->engine_flags) &
						world::kEntityFlagDead) != 0;
		if (dead_state6) {
			++reply.dead_live_ticks;
		} else {
			reply.dead_live_ticks = 0;
		}
		if (dead_state6 &&
				reply.dead_live_ticks > kDeadLiveTickLimit &&
				!ctx.config.permanent_death) {
			stage_host_punt(conn, kPuntDeadTooLong);
			continue;
		}

		const bool age_eligible = age_player != nullptr &&
				!conn.link.respawn_pending &&
				((age_player->flags | age_player->engine_flags) & 1u) == 0;
		if (age_eligible &&
				reply.control_live_ticks < CONTROL_REQUEST_LIVE_GATE_TICKS)
			++reply.control_live_ticks;
	}
}

} // namespace

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

void Server_TickUpdate(NapiNPServerCtx &ctx) {
	// A joiner is a pure non-authority client (its frame is P5's Client_ProcessNetworkFrame); the
	// pre-World P2 unit-test path has no simulation to drive. Either way: no host frame. The host
	// tick runs under is_authority [orig: Game_ProcessMainFrame @0x5263f0 gates the call
	// `if (is_authority && !suspended) Server_TickUpdate(...)` @0x5266b4]. is_in_session (+0x58) is
	// NOT the gate here — it gates the per-frame replicate/broadcast at step (3) [D-NET-120]; the
	// C2S drain + sim tick run regardless (the orig recv/send pumps are not is_in_session-gated).
	if (ctx.world == nullptr || ctx.is_authority == 0) return;
	world::World &world = *ctx.world;
	const bool round_was_announced = ctx.round_end_announced;

	// (1) net-before-logic: drain each in-match connection's queued C2S 0x0C and read-apply (SNAP).
	// burst.spawned marks an in-match connection — a mid-burst peer is still receiving its §5.2a
	// initial-state stream via tick_connections (which skips spawned peers, napi_np_protocol.cpp:509)
	// and has no per-frame C2S 0x0C uplink yet. [orig: PumpRecvQueues walks connection_list.]
	// [D-NET-124] This fan assumes the spawned remote-peer (type-1) nodes stay resident in
	// connection_list; a mid-match configure_session_runtime() erases them (napi_np_protocol.cpp),
	// which would silently drop those peers from drain+replicate — revisit when reconfigure lands.
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
				netsim::Datagram stale;
				while (conn.link.transport->host_recv(stale)) {}
			}
			conn.discard_pre_deploy_uplinks = false;
		}
		netsim::drain_connection_c2s(world, conn.link);
	}

	// (1b) The WAC 'humans' count: active human player slots, rebuilt each server tick
	// just before the script pass — the original also uses it as the empty-dedicated-
	// server world-run gate (entities/WAC advance while humans > 0 || ticks == 0); an
	// SP host always counts its own loopback player. [orig: Server_BuildEntitySlotLists
	// @0x4f97a0 — zero @0x4f97c6, +1 per active human slot @0x4f98b1; called from
	// Server_TickUpdate @0x51d89a before the WAC pre-pass]
	{
		int32_t humans = 0;
		for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
			// Entity ownership stands in for the original's slot-state-6 check —
			// the SP host's loopback owns its player from the spawn on, while its
			// in-match phase flag rides the burst bookkeeping.
			if (conn.link.owned_entity.valid()) ++humans;
		}
		world.cached.humans = humans;
	}

	// (2) one logic tick (the host is always authority here). WAC/BMS/AI advance the world.
	// [D-NET-123] Server_TickUpdate OWNS this logic tick — the inverse of the legacy seam, where the
	// C2S drain ran INSIDE run_logic_tick (a net ISystem, retired P8). A binding driving the runtime
	// through Server_TickUpdate must NOT keep its own run_logic_tick() or a parallel connection-table
	// driver, or the sim advances twice per frame (and the C2S queue drains twice — header guardrail).
	world.run_logic_tick(/*is_authority=*/true);
	world.match.advance_tick(world);
	// Retail produces capture requests from movement collisions throughout the
	// logic tick, then drains them in the periodic block below. Remote authority
	// players are snapshot-driven here, so the world-owned contact pass consumes
	// their same MoveOrder bit and final host pose. [orig: collision callsite
	// @0x4B2F90..0x4B2FD0; Server_OnPlayerTouchCaptureZone @0x500BA0]
	if (ctx.is_in_session && !world.match.outcome().ended)
		world::zone_capture_contact_tick(world);

	// (2b) Death routing + respawn release — the deaths the round sim raised inside the
	// tick get their broadcasts staged before this frame's 0x0A fan (§5.60; the 0x0A
	// health byte carries the same-frame damage regardless).
	route_throwable_events(ctx, world);
	route_round_deaths(ctx, world);
	route_match_gameplay_events(ctx, world);
	// Retail's per-player proximity pass begins by comparing CRenderState field
	// 0x1C (raw accumulated Points) with a player-slot cache and targets reliable
	// S2C 0x81 to that requester on change. Keep it after the authority's event
	// scorers and before the 1 Hz capture transaction; zone-capture points are
	// therefore observed by the following authority pass, as in retail.
	// [orig: Server_UpdateCaptureZoneProximity @0x5086A0]
	emit_requester_score_refreshes(ctx, world);
	release_due_respawns(ctx, world);
	// Retail drains an already-ended round here, before its periodic automatic
	// win-condition pass. WAC/BMS can end the round during the world tick above,
	// so those script-driven outcomes consume this tick; automatic MP outcomes found
	// by the check below do not. Keep the phase fact even though our countdown
	// mutation is grouped at the tail of this function.
	// [orig: Server_TickUpdate @0x51D7E0: linger drain @0x51DA04 precedes
	// Server_CheckWinConditions @0x51DF5A]
	const bool round_ended_at_retail_linger_phase =
			world.match.outcome().ended;

	// (2c) Win conditions at 1 Hz [orig: the g_periodic_second_timer block in
	// Server_TickUpdate — reload 62 @0x51db93 — calls Server_CheckWinConditions
	// @0x51df5a once per second].
	if (world.logic_tick % 62u == 0) check_win_conditions(ctx, world);
	announce_round_end(ctx, world);

	// Queue per-peer retail maintenance before the ordinary 0x0A fan so the
	// requests share HostSession's next open S2C boundary.
	if (!world.match.outcome().ended) {
		emit_periodic_session_maintenance(ctx, world);
		emit_minimap_overlay_state(ctx, world);
	}

	// (2d) The capture transaction at 1 Hz [orig: the
	// Server_TickUpdate g_periodic_second_timer block @0x51DF50..0x51DF8C: proximity ->
	// Server_UpdateCaptureZoneEntities (0x6F + 0x1E 0x3B/0x3C) -> Server_EnforceZoneEntityTeams
	// -> Server_UpdateCaptureZones (instant numbered flips + timed active entries)].
	// The world side runs in zone_capture_second_tick; this block encodes its events:
	//   0x6F 15 B [u16 handle][u8 team][i32 control][i32 0x10000][i16 delta][u8 f][u8 e]
	//     [orig: NetPacket_WriteZoneTimerValue @0x506E70] — CHANGE-GATED to all in-match
	//     conns + the full set at 1 Hz to deploy-pending/dead ones (the golden carries
	//     0x6F in deploy-window bursts, not a steady per-second stream; D-NET-162);
	//   0x1E 8 B zone events [orig: GameEvent_BuildPayload @0x5054E0]: 0x3B/0x3C secure
	//     edges (attacker = sorted spawn-zone-list index; victim = zone team); flips
	//     50/51 (frontier held) or 52/53 (victim =
	//     the recipient side's NEW frontier), team-filtered; then the 56/57 banner to all
	//     [orig: GameEvent_FlagCapture @0x50F6F0];
	//   0x53 9 B for ACTIVE unnumbered captures [u16 handle][u8 curTeam][u8 capTeam]
	//     [u16 progress][u16 limit][u8 rate], and 0x6C [u16 handle][u8 presence]
	//     when the unique contact rate changes [orig: NetPacket_WriteZoneTimerWindow
	//     @0x506D00; CaptureCtx_UpdateActiveCaptureRate @0x53B600]. Numbered instant
	//     flips do not emit 0x53 in Server_UpdateCaptureZones @0x53B8F0.
	// The independent general 0x40 minimap-overlay producer runs above at its
	// retail 14-tick cadence. It is intentionally not gated on this AS chain.
	if (ctx.is_in_session && !world.match.outcome().ended &&
			world.logic_tick % 62u == 0 &&
			(world.match.rules().game_type & 0x30000u) != 0) {
		world::ZoneCaptureEvents ev;
		world::zone_capture_second_tick(world, ev);
		for (const world::ZoneCaptureEvents::Flip &flip : ev.flips)
			world.match.record_zone_capture(world, flip.scorers);
		for (const auto &completion : ev.timed_completions)
			world.match.record_zone_capture(
					world, {completion.capturer});

		// 0x6F bodies + the change gate.
		std::vector<std::pair<uint16_t, std::vector<uint8_t>>> zone_6f; // (handle, body)
		std::vector<uint16_t> changed_6f;
		for (const auto &c : ev.control) {
			std::vector<uint8_t> b;
			put_u16le(b, c.zone.packed);
			b.push_back(c.team);
			const uint32_t ctrl = static_cast<uint32_t>(c.control);
			b.push_back(static_cast<uint8_t>(ctrl & 0xFF));
			b.push_back(static_cast<uint8_t>((ctrl >> 8) & 0xFF));
			b.push_back(static_cast<uint8_t>((ctrl >> 16) & 0xFF));
			b.push_back(static_cast<uint8_t>((ctrl >> 24) & 0xFF));
			b.push_back(0x00); // limit = 0x10000 fixed [orig: @0x506e9d]
			b.push_back(0x00);
			b.push_back(0x01);
			b.push_back(0x00);
			put_u16le(b, static_cast<uint16_t>(c.delta));
			b.push_back(c.friendlies);
			b.push_back(c.enemies);
			auto it = ctx.zone_6f_cache.find(c.zone.packed);
			if (it == ctx.zone_6f_cache.end() || it->second != b) {
				changed_6f.push_back(c.zone.packed);
				ctx.zone_6f_cache[c.zone.packed] = b;
			}
			zone_6f.emplace_back(c.zone.packed, std::move(b));
		}

		const world::SpawnZoneRegistry spawn_zones =
				world::build_spawn_zone_list(world);
		auto zone_index_of = [&](world::EntityHandle h) -> uint8_t {
			const int index = world::spawn_zone_index_of(spawn_zones, h);
			return index >= 0 && index <= 0xFE
					? static_cast<uint8_t>(index)
					: uint8_t{0xFF};
		};
		auto event_body = [](uint8_t ev_type, uint8_t attacker, uint8_t victim) {
			return std::vector<uint8_t>{ev_type, attacker, victim, 0xFF, 0, 0, 0, 0};
		};

		std::vector<std::vector<uint8_t>> timer_53;
		for (const auto &window : ev.timer_windows) {
			std::vector<uint8_t> b;
			put_u16le(b, window.zone.packed);
			b.push_back(window.current_team);
			b.push_back(window.capturing_team);
			put_u16le(b, window.progress);
			put_u16le(b, window.limit);
			b.push_back(window.rate);
			timer_53.push_back(std::move(b));
		}
		std::vector<std::vector<uint8_t>> presence_6c;
		for (const auto &presence : ev.presence) {
			std::vector<uint8_t> b;
			put_u16le(b, presence.zone.packed);
			b.push_back(presence.count);
			presence_6c.push_back(std::move(b));
		}

		for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
			if (!is_in_match(conn) || conn.link.transport == nullptr) continue;
			if (conn.link.mode == netsim::TransportMode::Loopback) continue;
			bool dead = false;
			uint8_t conn_team = 0;
			if (conn.link.owned_entity.valid()) {
				if (const world::Entity *e = world.registry.get(conn.link.owned_entity)) {
					dead = e->health <= 0;
					conn_team = e->team;
				}
			}
			const bool deploy_screen = conn.link.respawn_pending || dead;

			// 0x6F: changed zones to everyone; the full set to deploy-screen recipients.
			for (const auto &zb : zone_6f) {
				const bool changed = std::find(changed_6f.begin(), changed_6f.end(),
				                               zb.first) != changed_6f.end();
				if (changed || deploy_screen)
					conn.link.transport->host_send(
							s2c::ZONE_TIMER_VALUE, zb.second,
							/*reliable=*/false);
			}

			// 0x1E secure edges (to all in-match) [orig: @0x519839/@0x51988E].
			for (const auto &se : ev.secure_edges) {
				conn.link.transport->host_send(
						0x1E, event_body(se.secured ? 0x3B : 0x3C,
						                         zone_index_of(se.zone), se.zone_team));
			}

			for (const auto &body : timer_53)
				conn.link.transport->host_send(s2c::ZONE_TIMER_WINDOW, body);
			for (const auto &body : presence_6c)
				conn.link.transport->host_send(s2c::ZONE_PRESENCE_COUNT, body);
			for (const auto &start : ev.timed_starts) {
				const uint8_t actor = start.capturer.pool() == 0 &&
						start.capturer.slot() <= 0xFE
						? static_cast<uint8_t>(start.capturer.slot())
						: uint8_t{0xFF};
				conn.link.transport->host_send(
						0x1E, event_body(start.team == 1 ? 41 : 42,
						                 actor, 0xFF));
			}
			for (const auto &completion : ev.timed_completions) {
				if (!completion.announce) continue;
				const uint8_t actor = completion.capturer.pool() == 0 &&
						completion.capturer.slot() <= 0xFE
						? static_cast<uint8_t>(completion.capturer.slot())
						: uint8_t{0xFF};
				conn.link.transport->host_send(
						0x1E, event_body(completion.new_team == 1 ? 43 : 44,
						                 actor, 0xFF));
			}

			// Numbered instant-flip events.
			for (const auto &f : ev.flips) {
				if (!f.announce) continue;
				if (f.suppressed) continue; // match decided [orig: @0x4A2920 gate]
				const uint8_t zone_idx = zone_index_of(f.zone);
				if (conn_team == f.capturer_team) {
					conn.link.transport->host_send(
							0x1E, f.frontier_changed
							              ? event_body(53, zone_idx, f.capturer_frontier)
							              : event_body(51, zone_idx, f.new_team));
				} else {
					conn.link.transport->host_send(
							0x1E, f.frontier_changed
							              ? event_body(52, zone_idx, f.loser_frontier)
							              : event_body(50, zone_idx, f.new_team));
				}
				// The banner pair keyed by the new owning team [orig: 0x38/0x39 @0x50F991].
				conn.link.transport->host_send(
						0x1E, event_body(f.new_team == conn_team ? 56 : 57, zone_idx,
				                         f.new_team));
			}

		}
	}

	// (2c) Spawn-wave status: S2C 0x6E at 1 Hz to every PENDING or DEAD in-match player —
	// the deploy/death screen's team-roster + wave panel feed. With no host wave options
	// configured the body is the empty-group form (a single 0x00 group-count byte); wave
	// groups land with g_spawn_wave_list (§5.61 deferral). [orig: Server_TickUpdate
	// @0x51e089 emits NetPacket_WriteSpawnWaveStatus @0x507490 at 1 Hz; the recipient mask
	// includes the respawn-pending bit4 (slot+89912 & 0x10 @0x5074c2) and dead players;
	// golden ASH_I5A deploy window carries 0x6E ×11 at ~1 Hz]
	if (ctx.is_in_session && !world.match.outcome().ended &&
			world.logic_tick % 62u == 0) {
		static const std::vector<uint8_t> kEmptyWaveStatus{0x00};
		for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
			if (!is_in_match(conn) || conn.link.transport == nullptr) continue;
			if (conn.link.mode == netsim::TransportMode::Loopback) continue;
			bool dead = false;
			if (conn.link.owned_entity.valid()) {
				const world::Entity *e = world.registry.get(conn.link.owned_entity);
				dead = e != nullptr && e->health <= 0;
			}
			if (conn.link.respawn_pending || dead)
				conn.link.transport->host_send(
						s2c::SPAWN_WAVE_STATUS, kEmptyWaveStatus,
						/*reliable=*/false);
		}
	}

	// (2d) Water-surface crossings: S2C 0x34 to every ALIVE in-match player, one
	// message per crossing the motor recorded this tick. Retail fans the splash
	// with send_mask 128 (alive players) the moment a hull crosses the plane, so
	// clients spawn the same effect at the same spot; the queue is drained and
	// cleared every tick whether or not anyone is listening, because a crossing
	// is presentation, never simulation state.
	// [orig: Server_SendOverlayActionToAlive @0x50a1b0, send_mask 128]
	if (ctx.is_in_session && !world.water_crossings.events.empty()) {
		const std::vector<std::vector<uint8_t>> splashes =
				netsim::build_water_cross_messages(world);
		for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
			if (!is_in_match(conn) || conn.link.transport == nullptr) continue;
			if (conn.link.mode == netsim::TransportMode::Loopback) continue;
			bool alive = true;
			if (conn.link.owned_entity.valid()) {
				const world::Entity *e = world.registry.get(conn.link.owned_entity);
				alive = e != nullptr && e->health > 0;
			}
			if (!alive) continue; // the mask-128 alive filter
			for (const std::vector<uint8_t> &body : splashes)
				conn.link.transport->host_send(s2c::PLAY_SOUND, body,
				                               /*reliable=*/false);
		}
	}
	world.water_crossings.clear();

	// (3) serialize-after — SESSION-ONLY [D-NET-120]: the original's per-frame replicate/broadcast
	// blocks are each gated on is_in_session (+0x58) inside Server_TickUpdate (@0x51d9ab..0x51e3f3),
	// while the C2S recv pump above is not — so a World kept alive past match-end (is_in_session 0,
	// world non-null) keeps ticking but stops fanning ghost 0x0A frames. Build the world snapshot
	// ONCE, then fan a per-connection-anchored 0x0A to every in-match connection
	// [orig: NapiNPServer_SendFiltered @0x4C87E0 once, SendToConn per node].
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
		const std::vector<GameEntitySnapshot> ents = netsim::snapshot_world(world);
		for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
			if (!is_in_match(conn)) continue;
			// The host's type-2 loopback is an in-process presentation seam and
			// remains full-rate. Type-1 peers receive one fresh 0x0A only when
			// their configured S2C send boundary opens; queuing all intervening
			// snapshots would burst stale frames at that boundary.
			if (conn.type == 1 && !conn.s2c_send_boundary_open) continue;
			netsim::emit_connection_s2c(world, conn.link, ents,
			                            ctx.config.game_type,
			                            conn.type == 1
						? kMaxFrameUpdateBodyBytes
						: 0);
		}
	}

	// Retail holds the multiplayer post-round state for 2790 server ticks. Its
	// drain precedes automatic win checks, so only an outcome already present at
	// that phase (including a WAC/BMS result from this tick) consumes the first
	// count; automatic multiplayer announcements start draining next tick. At expiry,
	// the session replication gate closes while the frozen result stays readable.
	// [orig: store @0x5166C4; phase/drain @0x51DA04; exit reason 3 @0x51DA91]
	if ((round_was_announced || round_ended_at_retail_linger_phase) &&
			ctx.round_end_linger_ticks > 0) {
		--ctx.round_end_linger_ticks;
		if (ctx.round_end_linger_ticks == 0) ctx.is_in_session = 0;
	}

	// (4) flush is implicit: host_send staged each 0x0A on its transport. The loopback's local client
	// reads it via client_recv / ClientReplicaPipeline::pump; a remote peer's transport outbound_ is popped +
	// framed into a 0x83 SESSION by the owner (frame_in_match_s2c). No socket I/O in engine/.
}

} // namespace opennova::np
