#include <net/npruntime/server_tick.h>
#include <runtime/devtools/tick_profile.h>
#include <net/npruntime/end_round_protocol.h>
#include <net/npruntime/server_message_dispatch.h> // build_player_list_message
#include <base/io/perf_clock.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/nw_session_framing.h> // per-player 0x61 tick-seed roll
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_hello.h>
#include <net/netsim/entity_wire_bridge.h> // snapshot_world / GameEntitySnapshot
#include <net/netsim/connection_fan.h>     // drain_connection_c2s / emit_connection_s2c
#include <runtime/world/ai.h>                  // AiEntity::see_all (the team-kill exemption)
#include <runtime/world/world.h>
#include <runtime/world/collision.h>           // stable replication LOS view epoch
#include <runtime/world/geom.h>                // to_fixed
#include <runtime/world/infantry.h>            // drown death animation selection
#include <runtime/world/minimap_overlay.h>      // portable Entity_ClassifyForMinimap result
#include <runtime/world/spawn_select.h>         // sorted SpawnZoneList index for capture events
#include <runtime/world/vehicle_motor.h>       // VehicleTraits (the 0x40 vehicle-blip icons)
#include <runtime/world/world.h>               // World::run_logic_tick
#include <runtime/world/zone_capture.h>        // the 1 Hz AS capture pass (slice 2)

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

// NapiNPServer_SendFiltered's 0x80 arm accepts player-slot state 6 or 7. It
// includes the listen host and does not inspect entity health; this runtime's
// completed initial-state burst is the shared representation of that active
// slot state. [orig: NapiNPServer_SendFiltered @0x4C8874..0x4C8894,
// @0x4C893E..0x4C8953]
bool active_player_recipient(const NapiNPConnection &conn) {
	return is_in_match(conn) && conn.link.transport != nullptr;
}

uint8_t death_family_variant(world::World &world, uint8_t base) {
	return static_cast<uint8_t>(
			base + ((3u * uint32_t(world.next_prng16())) >> 16));
}

bool is_retail_flag_type(const world::World &world,
		world::EntityHandle carried) {
	const world::Entity *item = world.registry.get(carried);
	return item != nullptr &&
			(item->item_id == 4091 || item->item_id == 4093 ||
			 item->item_id == 4095);
}

struct PlayerDeathFeed {
	uint8_t event_type = 22;
	uint8_t attacker = 0;
	uint8_t victim = 0;
	uint8_t aux = 0;
};

constexpr uint32_t kRetailBreathSeconds = 20;
constexpr uint32_t kBreathSampleLimit = 4u * kRetailBreathSeconds;

// Classify the one S2C 0x1E record before Match drops the victim's carried
// objective. The original reads the transient entity+44 cause word in this
// priority order and consumes it inline. In particular, 4091/4093/4095 are
// carried flag ItemDefs (STRCND22 "killed flag carrier"), 0x800 is the
// STRCND08 headshot family, and AMMO_60MM_MORTAR selects STRCND47.
// The 0x100 branch really is a retail quirk: its CRT rand() is shifted to a
// 0..127 word and then divided by 21845, so this producer always emits 32 even
// though the client retains strings for 33/34.
// [orig: GameEvent_PlayerDeath @0x516DD0, classifier @0x5170E0..0x51724A;
// the GameEvent_BuildPayload call @0x517362 (0x51737b is the payload-length
// store that follows it)]
PlayerDeathFeed classify_player_death(
		world::World &world, const world::RoundDeath &death,
		const world::Entity *victim_entity,
		const world::Entity *killer_entity,
		uint32_t underwater_breath_samples) {
	PlayerDeathFeed out;
	const uint8_t victim_index = pool0_index_byte(death.victim_handle);
	if (killer_entity == nullptr) {
		if (underwater_breath_samples > kBreathSampleLimit)
			out.event_type = 26;
		else if ((death.event_flags & 0x200u) != 0u)
			out.event_type = 23;
		out.attacker = victim_index;
		return out;
	}
	if ((killer_entity->flags & world::kEntityFlagPlayer) == 0u) {
		out.event_type = 22;
		out.attacker = victim_index;
		return out;
	}
	if (death.killer == death.victim) {
		out.event_type = death_family_variant(world, 1);
		out.attacker = victim_index;
		return out;
	}

	const uint8_t killer_index = pool0_index_byte(death.killer_handle);
	// The see-all exemption: a same-team kill routes to the team-kill arm only
	// when NEITHER side's AI record carries the targets-any-team flag
	// (aiSlot[4] & 0x200); either flag set falls through to the enemy-kill
	// ladder with the victim/aux bytes filled and the weapon-stat accumulation.
	// [orig: GameEvent_PlayerDeath — victim gate @0x51709C..0x5170B6, killer
	//  twin @0x5170BE..0x5170DA, branch @0x5170F8..0x517113]
	auto sees_all = [&world](world::EntityHandle handle) {
		if (world.ai == nullptr) return false;
		const world::AiEntity *ai = world.ai->for_handle(handle);
		return ai != nullptr && ai->see_all;
	};
	if (victim_entity != nullptr && victim_entity->team != 0 &&
			victim_entity->team == killer_entity->team &&
			!sees_all(death.victim) && !sees_all(death.killer)) {
		out.event_type = death_family_variant(world, 7);
		out.attacker = killer_index;
		out.victim = victim_index;
		out.aux = 0xFF;
		return out;
	}

	if (victim_entity != nullptr &&
			is_retail_flag_type(world, victim_entity->mounted_child)) {
		out.event_type = 24;
	} else if ((death.event_flags & 0x100u) != 0u) {
		// The roll still consumes one CRT draw before its narrowed quotient
		// lands on 32, so the draw order of the shared stream stays
		// structural. [orig: rand @0x51718A; imul/sar @0x51719A..0x5171A9]
		(void)world.crt_rand.next();
		out.event_type = 32;
	} else if ((death.event_flags & 0x800u) != 0u) {
		out.event_type = death_family_variant(world, 10);
	} else if ((death.event_flags & 0x400u) != 0u) {
		out.event_type = death_family_variant(world, 13);
	} else {
		const world::AmmoTableEntry *ammo =
				world.ammo.by_index(death.ammo_index);
		out.event_type = ammo != nullptr &&
					world.ammo.index_of("AMMO_60MM_MORTAR") == death.ammo_index
				? 49u
				: death_family_variant(world, 4);
	}
	out.attacker = killer_index;
	out.victim = victim_index;
	out.aux = pool0_index_byte(killer_entity->primary_occupant.packed);
	return out;
}

// Retail samples breath every 32 authority ticks, not every frame. While an
// active living player's eye is strictly below the authored water plane,
// playerSlot+460 increments; sample 81 selects death_drown and runs the ordinary
// no-killer death transaction. Dry or dead players clear the counter. The
// breath global is initialized to 20 and has no other writer in the retail
// image, so keep it a local invariant rather than another configuration seam.
// [orig: Server_TickUpdate gate @0x51D8C4..0x51D8D7;
// Server_UpdateEntityIdleTimers @0x50D770;
// WacScript_FreeAll initializes dword_C6EAE0=20 @0x4F6381]
void tick_player_breath(NapiNPServerCtx &ctx, world::World &world) {
	if ((world.logic_tick & 0x1Fu) != 0u) return;
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (!is_in_match(conn) || !conn.link.owned_entity.valid()) continue;
		world::Entity *player = world.registry.get(conn.link.owned_entity);
		if (player == nullptr || !player->alive || player->health <= 0 ||
				(player->flags & world::kEntityFlagDead) != 0u) {
			conn.link.underwater_breath_samples = 0;
			continue;
		}

		// The one underwater-eye predicate (no authored water is never below).
		if (!world::entity_eye_below_water(world,
					world::to_fixed(player->position.z), player->eye_offset_z)) {
			conn.link.underwater_breath_samples = 0;
			continue;
		}

		if (++conn.link.underwater_breath_samples <= kBreathSampleLimit)
			continue;
		player->death_anim_state = world::compute_death_anim_state(
				0, 0, world::death_cause::kDrown);
		player->health = -1;
		world::RoundDeath death;
		death.victim = player->handle;
		death.victim_handle = player->handle.packed;
		world.round_sim.deaths.push_back(death);
	}
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
// [orig: Server_BuildOverlayStateForPlayer @0x517FC0 (the 14-tick cooldown, the
//  0..127 phase and the +128 stride); Entity_ClassifyForMinimap @0x50FA70 (the
//  item-type 1 / carrier-type 5 classification and the 0x10 persistent flag)]
void emit_minimap_overlay_state(NapiNPServerCtx &ctx, world::World &world) {
	if (!ctx.is_in_session) return;
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if ((conn.type != NapiNPConnection::kTypeServerSide &&
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
// Pickup/save/capture/timeout-return also precede that state mutation with the
// corresponding 8-B 0x1E event record.
// The event and 0x2F state use active-player mask 0x80 (host included); CTF's
// following 0x12 removal uses 0x90 (host excluded).
// [orig: Entity_AttachToVehicle @0x43C130 -> Server_HandleEntityDeath @0x517460
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
		world::Vec3 feed_position{};
		switch (event.kind) {
		case world::MatchGameplayEventKind::FlagPickup:
			feed_type = 0x14;
			feed_actor = pool0_index_byte(event.actor.packed);
			feed_position = event.position;
			break;
		case world::MatchGameplayEventKind::FlagSave:
			feed_type = 0x15;
			feed_actor = pool0_index_byte(event.actor.packed);
			feed_position = event.position;
			break;
		case world::MatchGameplayEventKind::FlagCapture:
			feed_type = 0x13;
			feed_actor = pool0_index_byte(event.actor.packed);
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
			put_u16le(feed, static_cast<uint16_t>(
					static_cast<uint32_t>(world::to_fixed(feed_position.x)) >> 16));
			put_u16le(feed, static_cast<uint16_t>(
					static_cast<uint32_t>(world::to_fixed(feed_position.y)) >> 16));
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
			if (!active_player_recipient(conn)) continue;
			if (!feed.empty())
				conn.link.transport->host_send(s2c::GAME_EVENT, feed);
			if (event.remove_objective) {
				if (conn.link.mode == netsim::TransportMode::Loopback) continue;
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

// Drain each transport-free RoundDeath through the one retail player-death
// transaction: 0x13 remote fan, victim 0x61 seed, victim 0x52 camera, optional
// 0x1E active-player feed, conditional 0x54 Medic state, scoring, and respawn
// holds. AI victims stop after the 0x13/scoring leg.
// [orig: Entity_CheckAndProcessDeath @0x51B550 ->
// GameEvent_PlayerDeath @0x516DD0]
void route_round_deaths(NapiNPServerCtx &ctx, world::World &world) {
	if (world.round_sim.deaths.empty()) return;
	for (const world::RoundDeath &d : world.round_sim.deaths) {
		const world::Entity *victim_entity = world.registry.get(d.victim);
		const world::Entity *killer_entity = world.registry.get(d.killer);
		const bool victim_is_player = victim_entity != nullptr &&
				(victim_entity->flags & world::kEntityFlagPlayer) != 0u;
		NapiNPConnection *victim_connection = nullptr;
		for (NapiNPConnection &c : ctx.np_protocol.connection_list) {
			if (!c.link.owned_entity.valid() ||
					c.link.owned_entity.packed != d.victim_handle)
				continue;
			victim_connection = &c;
			break;
		}
		// Match drops a carried objective, so capture the retail classifier's
		// mountedChild and player-slot inputs before handing the transaction to
		// its score ledger.
		const PlayerDeathFeed feed = victim_is_player
				? classify_player_death(
						world, d, victim_entity, killer_entity,
						victim_connection != nullptr
								? victim_connection->link.underwater_breath_samples
								: 0u)
				: PlayerDeathFeed{};
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
		if (victim_connection != nullptr) {
			// A normal other-player kill opens the exact 120-second revive
			// window. Self/environment and knife/headshot cause bits clear it.
			// [orig: GameEvent_PlayerDeath @0x516DD0: playerSlot+368]
			victim_connection->link.downed_revive_seconds =
					d.killer.valid() && d.killer != d.victim &&
					(d.event_flags & 0xC00u) == 0u
							? 120u
							: 0u;
			victim_connection->link.medic_request_active = false;
		}

		if (ctx.is_in_session) {
			std::vector<uint8_t> body13;
			put_u16le(body13, d.victim_handle);
			put_u16le(body13, d.killer_handle); // the killerSource stamp [orig: entity+704]
			std::vector<uint8_t> body1e;
			if (victim_is_player) {
				body1e.push_back(feed.event_type);
				body1e.push_back(feed.attacker);
				body1e.push_back(feed.victim);
				body1e.push_back(feed.aux);
				// Every PlayerDeath call passes literal zero/zero; positions belong
				// to other 0x1E producers, not this feed family.
				// [orig: GameEvent_PlayerDeath @0x516DD0 (the GameEvent_BuildPayload
				//  call @0x517362; 0x51737b is the payload-length store)]
				put_u16le(body1e, 0);
				put_u16le(body1e, 0);
			}
			// First broadcast the ordinary death record with mask 0x90
			// (active players, not the listen host).
			for (NapiNPConnection &c : ctx.np_protocol.connection_list) {
				if (!is_in_match(c) || c.link.transport == nullptr) continue;
				if (c.link.mode == netsim::TransportMode::Loopback) continue;
				c.link.transport->host_send(s2c::ENTITY_DEATH, body13);
			}

			// Retail DISARMS the victim immediately after 0x13 and before the
			// camera/feed tail: the death sender passes enable=0, which zeroes the
			// slot's seed/freshness stamp and ships the four-zero 0x61. The zero
			// seed freezes the client's network-role tick until the deploy release
			// re-arms it with a fresh roll. [orig: GameEvent_PlayerDeath @0x516EF4
			// -> Server_SendRandomSeedToPlayer @0x5101A0, enable==0 arm @0x510237]
			if (victim_connection != nullptr &&
					is_in_match(*victim_connection) &&
					victim_connection->link.transport != nullptr) {
				victim_connection->link.transport->host_send(
						s2c::TICK_SEED,
						Server_DisarmPlayerTickSeed(*victim_connection));
			}

			// Then target the victim with the fixed-point position used by the
			// third-person death camera: killer position when one resolves, else
			// the victim position. Mask 0x20 includes the listen host.
			// [orig: GameEvent_PlayerDeath @0x516DD0 ->
			// NetPacket_WriteThreeInt32s @0x506CB0]
			if (victim_connection != nullptr &&
					is_in_match(*victim_connection) &&
					victim_connection->link.transport != nullptr) {
				const world::Entity *camera_entity = d.killer.valid()
						? world.registry.get(d.killer) : nullptr;
				if (camera_entity == nullptr) camera_entity = victim_entity;
				DeathCameraTarget target;
				if (camera_entity != nullptr) {
					target.x = world::to_fixed(camera_entity->position.x);
					target.y = world::to_fixed(camera_entity->position.y);
					target.z = world::to_fixed(camera_entity->position.z);
				}
				victim_connection->link.transport->host_send(
						s2c::DEATH_CAMERA_TARGET,
						encode_death_camera_target(target));
			}

			// The kill feed uses mask 0x80 (every active player, including the
			// listen host), unlike 0x13's 0x90 host exclusion. `deathmes=0`
			// suppresses this record only; seed/camera/medic state still flow.
			// [orig: GameEvent_PlayerDeath @0x51725F/@0x51734E]
			if (!body1e.empty() && ctx.config.death_messages != 0u) {
				for (NapiNPConnection &c : ctx.np_protocol.connection_list) {
					if (!is_in_match(c) || c.link.transport == nullptr)
						continue;
					c.link.transport->host_send(s2c::GAME_EVENT, body1e);
				}
			}

			// Finally publish the revive window to active, alive same-team
			// Medics. Manual Auto-Medic preference first clears their marker and
			// sends the live window to the victim alone; automatic mode sends the
			// window directly to the Medic group. Mask 0x580 does include the host.
			// [orig: GameEvent_PlayerDeath @0x516DD0;
			// NapiNPServer_SendFiltered @0x4C87E0]
			if (victim_connection != nullptr && victim_entity != nullptr &&
					victim_connection->link.downed_revive_seconds != 0u) {
				auto send_downed = [&](NapiNPConnection &recipient,
						uint8_t seconds) {
					PlayerDownedState state;
					state.entity_handle = d.victim_handle;
					state.revive_seconds = seconds;
					recipient.link.transport->host_send(
							s2c::PLAYER_DOWNED_STATE,
							encode_player_downed_state(state));
				};
				for (NapiNPConnection &candidate : ctx.np_protocol.connection_list) {
					if (!is_medic_recipient(candidate, world, victim_entity->team))
						continue;
					send_downed(candidate,
							victim_connection->link.auto_medic_enabled
									? static_cast<uint8_t>(
											victim_connection->link.downed_revive_seconds)
									: uint8_t{0});
				}
				if (!victim_connection->link.auto_medic_enabled &&
						is_in_match(*victim_connection) &&
						victim_connection->link.transport != nullptr) {
					send_downed(*victim_connection,
							static_cast<uint8_t>(
									victim_connection->link.downed_revive_seconds));
				}
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

		if (victim_connection != nullptr) {
			// Every player slot gets the same two whole-second counters. A
			// configured timeout below three is floored; a spawn less than 620
			// authority ticks ago forces exactly three. +364 retains the greater
			// value only when a spawn-target registry exists.
			// [orig: GameEvent_PlayerDeath @0x516ec4..0x516eeb]
			netsim::Connection &link = victim_connection->link;
			uint32_t hold = std::max(ctx.config.respawn_timeout, 3u);
			if (link.last_deploy_tick_valid &&
					static_cast<uint32_t>(world.logic_tick - link.last_deploy_tick) < 620u)
				hold = 3;
			link.respawn_delay_seconds = hold;
			link.spawn_target_hold_seconds = world::world_has_spawn_zone(world)
					? std::max(link.spawn_target_hold_seconds, hold)
					: 0u;
			link.respawn_hold_armed = true;
		}
	}
	world.round_sim.deaths.clear();
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
	// The board stream is frozen ONCE here, before the per-slot push; the C2S
	// 0x2B service only cuts chunks from it.
	// [orig: Server_ProcessRoundEnd @0x5164F0 (the
	// Server_BuildEndOfRoundScoreboard(1, winTeam) call @0x516590)]
	ctx.round_end_board_stream =
			encode_end_round_stats(build_end_round_stats(result));
	// The header form is session state: the in-session non-team (DM/KOTH
	// family) header carries the top three frozen-board names/scores instead
	// of the winner/team-score words. world.mp_session is our SP-as-listen-
	// server stand-in for the retail is_in_session (ctx.is_in_session is
	// always 1 here — the in-process loopback IS a session).
	// [orig: EndRoundScoreboard_SerializeHeader form pick @0x5052a6]
	const bool non_team_header =
			world.mp_session && (result.game_type & 0x10000u) == 0;
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (!is_in_match(conn) || conn.link.transport == nullptr) continue;
		// The zero 0x61 precedes each recipient-specific 0x1D header, then the
		// player enters game-state 11. Match's sole outcome gate supplies the
		// original slot-state-7 replication stop without duplicating lifecycle
		// state. The client pulls 0x56 independently, so no board chunk is pushed.
		// [orig: Server_ProcessRoundEnd @0x516790..0x51685E]
		conn.link.transport->host_send(
				s2c::TICK_SEED, Server_DisarmPlayerTickSeed(conn));
		conn.link.transport->host_send(
				s2c::END_ROUND_HEADER,
				encode_end_round_header(
						build_end_round_header(result, conn.reply.player_slot,
								non_team_header),
						non_team_header));
		conn.burst.game_state = 11;
	}
	ctx.round_end_announced = true;
	ctx.round_end_linger_ticks = 2790;
	return true;
}

// The listen host has no socket-side death picker in the current presentation,
// so expiry supplies the Default Spawn command locally. It still enters the ONE
// deployment transaction used by C2S 0x0E and spawn-wave releases; there is no
// second entity-reset implementation. Remote players remain dead until a pick.
void release_expired_local_respawns(NapiNPServerCtx &ctx, world::World &world) {
	if (world.match.outcome().ended) return;
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (conn.link.mode != netsim::TransportMode::Loopback ||
				!conn.link.respawn_hold_armed ||
				conn.link.respawn_delay_seconds != 0 ||
				!conn.link.owned_entity.valid())
			continue;
		const world::Entity *player = world.registry.get(conn.link.owned_entity);
		if (player == nullptr || (player->flags & 2u) == 0) continue;
		std::vector<ProtocolMessage> deployment =
				Server_ReleasePlayerDeployment(
						ctx.config, conn, world, world::EntityHandle{});
		if (conn.link.transport == nullptr) continue;
		for (ProtocolMessage &message : deployment)
			conn.link.transport->host_send(
					message.tag, std::move(message.payload), message.reliable,
					message.flags.raw, message.capacity_exempt);
	}
}

// The original stores seconds, not tick deadlines. The per-slot decrements sit
// inside the same g_periodic_second_timer block as the win check and the
// capture transaction, so they ride Match's countdown, not a second phase.
// [orig: Server_TickUpdate @0x51DFB0..0x51E00B (slot +0x170/+0x168/+0x16C)]
void tick_respawn_holds(NapiNPServerCtx &ctx, const world::World &world) {
	if (!world.match.periodic_second()) return;
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (conn.link.respawn_delay_seconds != 0)
			--conn.link.respawn_delay_seconds;
		if (conn.link.spawn_target_hold_seconds != 0)
			--conn.link.spawn_target_hold_seconds;
		if (conn.link.downed_revive_seconds != 0)
			--conn.link.downed_revive_seconds;
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
		if (network_quality_boundary && active_player_recipient(conn)) {
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
		// The dead-age arm owns an independent consecutive-tick counter. It does
		// not derive elapsed time from Entity::death_tick: retail increments the
		// player-slot dword once per state-6 tick while Flags bit 0x02 is set,
		// resets it as soon as the bit clears, and compares after the increment.
		// The remaining retail exclusions (local player, bot/spectator, explicit
		// anti-cheat bypass) have no remote-player representation in this runtime;
		// a normal type-1 connection corresponds to all of them being clear.
		// [orig: counter @0x51E066..0x51E07D; punt @0x51E187..0x51E18E]
		const bool dead_state6 = ctx.is_in_session &&
				!conn.host_disconnect_sent && conn.type == NapiNPConnection::kTypeServerSide &&
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

std::vector<uint8_t> Server_RerollPlayerTickSeed(
		NapiNPConnection &connection) {
	connection.tick_seed =
			((make_random_session_u32() & 0xFEu) + 1u) << 16;
	std::vector<uint8_t> body;
	body.reserve(4);
	put_u32le(body, connection.tick_seed);
	return body;
}

// The enable==0 arm: clear the slot's seed/freshness stamp and ship the
// four-zero 0x61. [orig: Server_SendRandomSeedToPlayer @0x5101A0,
// enable==0 arm @0x510237..0x510278]
std::vector<uint8_t> Server_DisarmPlayerTickSeed(
		NapiNPConnection &connection) {
	connection.tick_seed = 0;
	return std::vector<uint8_t>(4, 0);
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

void Server_TickUpdate(NapiNPServerCtx &ctx) {
	// A joiner is a pure non-authority client (its frame is P5's Client_ProcessNetworkFrame); the
	// pre-World P2 unit-test path has no simulation to drive. Either way: no host frame. The host
	// tick runs under is_authority [orig: Game_ProcessMainFrame @0x5263f0 gates the call
	// `if (is_authority && !suspended) Server_TickUpdate(...)` @0x5266b4]. is_in_session (+0x58) is
	// NOT the gate here — it gates the per-frame replicate/broadcast at step (3) [D-NET-120]; the
	// C2S drain + sim tick run regardless (the orig recv/send pumps are not is_in_session-gated).
	if (ctx.world == nullptr || ctx.is_authority == 0) return;
	world::World &world = *ctx.world;
	// The phases lap onto the SIM_SERVER_* / SIM_MATCH / SIM_REPLICATION_*
	// rows of the world's profile (ADR 0043 d5); the logic tick attributes
	// its own SIM_WORLD_* rows inside run_logic_tick.
	devtools::ProfileLap lap(world.profile);
	const bool round_was_announced = ctx.round_end_announced;
	// Snapshot the phase at frame entry. Retail decrements the timer later on
	// the shared second boundary, after the entity-update gate has already been
	// tested, so the 1 -> 0 transition frame remains frozen.
	// [orig: entity gate @0x51D8BD; decrement @0x51DC20..0x51DC33]
	const bool preround_active = world.preround_delay_seconds != 0;

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

	// The sampled breath state belongs to the authoritative player slot and
	// runs at the same pre-entity-update point as retail. Its caller is skipped
	// during pre-round just like the rest of Server_UpdateEntityIdleTimers.
	// [orig: Server_TickUpdate @0x51D8C4..0x51D8D7;
	// Server_UpdateEntityIdleTimers gate @0x50D773]
	if (!preround_active) tick_player_breath(ctx, world);
	lap.mark(devtools::Slot::SIM_SERVER_INPUT);

	// (2) one logic tick (the host is always authority here). WAC/BMS/AI advance the world.
	// [D-NET-123] Server_TickUpdate OWNS this logic tick — the inverse of the legacy seam, where the
	// C2S drain ran INSIDE run_logic_tick (a net ISystem, retired P8). A binding driving the runtime
	// through Server_TickUpdate must NOT keep its own run_logic_tick() or a parallel connection-table
	// driver, or the sim advances twice per frame (and the C2S queue drains twice — header guardrail).
	world.run_logic_tick(
			/*is_authority=*/true,
			preround_active ? world::TickPhase::PreRound
			                : world::TickPhase::Gameplay);
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
			!world.match.outcome().ended)
		world::zone_capture_contact_tick(world);

	// (2b) Death routing + respawn release — the deaths the round sim raised inside the
	// tick get their broadcasts staged before this frame's 0x0A fan (§5.60; the 0x0A
	// health byte carries the same-frame damage regardless).
	route_throwable_events(ctx, world);
	route_round_deaths(ctx, world);
	route_match_gameplay_events(ctx, world);
	release_expired_local_respawns(ctx, world);
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
	// [orig: Server_UpdateCaptureZoneProximity @0x5086A0; the 0x81 sync
	// @0x508790]
	if (periodic_second) emit_requester_score_refreshes(ctx, world);

	// (2c) Win conditions at 1 Hz [orig: the g_periodic_second_timer block in
	// Server_TickUpdate @0x51D7E0 — reload 62 @0x51db93 — calls
	// Server_CheckWinConditions @0x51AD40 (the call @0x51df5a) once per second].
	if (periodic_second) check_win_conditions(ctx, world);
	tick_respawn_holds(ctx, world);
	announce_round_end(ctx, world);

	// Spawn-wave release precedes capture-zone mutation on the shared 1 Hz
	// cadence. Each release runs the same deployment transaction as an
	// immediate C2S 0x0E pick, then stages its private bundle on that player's
	// transport. [orig: SpawnWaveList_Tick @0x52A550 from Server_TickUpdate;
	// SpawnWaveList_TickEntry @0x52A330]
	if (ctx.is_in_session && !world.match.outcome().ended &&
			periodic_second) {
		for (const world::SpawnWaveRelease &release :
				world.spawn_waves.tick(world)) {
			for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
				if (!is_in_match(conn) || conn.link.transport == nullptr ||
						conn.link.owned_entity != release.player)
					continue;
				std::vector<ProtocolMessage> deployment =
						Server_ReleasePlayerDeployment(
								ctx.config, conn, world, release.zone);
				for (ProtocolMessage &message : deployment)
					conn.link.transport->host_send(
							message.tag, std::move(message.payload),
							message.reliable, message.flags.raw,
							message.capacity_exempt);
				break;
			}
		}
	}

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
	// The world side runs in zone_capture_second_tick; this block encodes its one
	// ordered event stream without regrouping messages by tag:
	//   0x6F 15 B [u16 handle][u8 team][i32 control][i32 0x10000][i16 delta][u8 f][u8 e]
	//     [orig: NetPacket_WriteZoneTimerValue @0x506E70] — CHANGE-GATED to all in-match
	//     conns + the full set at 1 Hz to deploy-pending/dead ones (the golden carries
	//     0x6F in deploy-window bursts, not a steady per-second stream; D-NET-162);
	//   0x50 6 B [u16 handle][u8 team][u16 netId][u8 animSlot] for every actual
	//     ownership mutation, including the ordered team-0/new-team instant pair;
	//     non-player identity is zero [orig: Server_ChangeEntityTeam @0x518D70;
	//     write_entity_handle_packet @0x506AD0];
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
			periodic_second &&
			(world.match.rules().game_type & 0x30000u) != 0) {
		world::ZoneCaptureEvents ev;
		world::zone_capture_second_tick(world, ev);
		for (const world::ZoneCaptureEvents::Event &event : ev.ordered) {
			if (const auto *flip =
						std::get_if<world::ZoneCaptureEvents::Flip>(&event))
				world.match.record_zone_capture(world, flip->scorers);
			else if (const auto *completion =
						std::get_if<world::ZoneCaptureEvents::TimedCompletion>(&event))
				world.match.record_zone_capture(
						world, {completion->capturer});
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
		auto send_all = [&](uint8_t tag, const std::vector<uint8_t> &body,
		                    bool reliable = true) {
			for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
				if (!is_in_match(conn) || conn.link.transport == nullptr) continue;
				conn.link.transport->host_send(tag, body, reliable);
			}
		};
		auto connection_team = [&](const NapiNPConnection &conn) -> uint8_t {
			if (!conn.link.owned_entity.valid()) return 0;
			const world::Entity *entity =
					world.registry.get(conn.link.owned_entity);
			return entity != nullptr ? entity->team : 0;
		};

		// Consume the semantic stream once, in retail callsite order. In
		// particular 0x50 neutral/new pairs must remain ahead of their 0x1E
		// announcement, and timed completion is 0x53 -> 0x50 -> 0x1E.
		for (const world::ZoneCaptureEvents::Event &event : ev.ordered) {
			if (const auto *control =
						std::get_if<world::ZoneCaptureEvents::Control>(&event)) {
				std::vector<uint8_t> body;
				put_u16le(body, control->zone.packed);
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
				put_u16le(body, static_cast<uint16_t>(control->delta));
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
				continue;
			}
			if (const auto *window =
						std::get_if<world::ZoneCaptureEvents::TimerWindow>(&event)) {
				std::vector<uint8_t> body;
				put_u16le(body, window->zone.packed);
				body.push_back(window->current_team);
				body.push_back(window->capturing_team);
				put_u16le(body, window->progress);
				put_u16le(body, window->limit);
				body.push_back(window->rate);
				send_all(s2c::ZONE_TIMER_WINDOW, body);
				continue;
			}
			if (const auto *presence =
						std::get_if<world::ZoneCaptureEvents::Presence>(&event)) {
				std::vector<uint8_t> body;
				put_u16le(body, presence->zone.packed);
				body.push_back(presence->count);
				send_all(s2c::ZONE_PRESENCE_COUNT, body);
				continue;
			}
			if (const auto *start =
						std::get_if<world::ZoneCaptureEvents::TimedStart>(&event)) {
				send_all(0x1E, event_body(start->team == 1 ? 41 : 42,
				                            pool0_index_byte(start->capturer.packed),
				                            0xFF));
				continue;
			}
			if (const auto *completion =
						std::get_if<world::ZoneCaptureEvents::TimedCompletion>(&event)) {
				if (completion->announce)
					send_all(0x1E, event_body(
							completion->new_team == 1 ? 43 : 44,
							pool0_index_byte(completion->capturer.packed), 0xFF));
				continue;
			}
			const auto *flip =
					std::get_if<world::ZoneCaptureEvents::Flip>(&event);
			if (flip == nullptr || !flip->announce || flip->suppressed) continue;
			const uint8_t zone_idx = zone_index_of(flip->zone);
			for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
				if (!is_in_match(conn) || conn.link.transport == nullptr) continue;
				const uint8_t team = connection_team(conn);
				if (team == flip->capturer_team) {
					conn.link.transport->host_send(
							0x1E, flip->frontier_changed
							              ? event_body(53, zone_idx,
							                           flip->capturer_frontier)
							              : event_body(51, zone_idx,
							                           flip->new_team));
				} else {
					conn.link.transport->host_send(
							0x1E, flip->frontier_changed
							              ? event_body(52, zone_idx,
							                           flip->loser_frontier)
							              : event_body(50, zone_idx,
							                           flip->new_team));
				}
				conn.link.transport->host_send(
						0x1E, event_body(flip->new_team == team ? 56 : 57,
						                 zone_idx, flip->new_team));
			}
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
								world, conn.link.owned_entity),
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
	lap.mark(devtools::Slot::SIM_SERVER_RULES);

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
		// The priority build runs only for recipients that take entity records:
		// the listen host's own player gets the header-only frame and never
		// walks the pools [orig: Server_SendEntityStateToPlayer @0x517c1b skips
		// Server_BuildEntityPriorityList for g_local_player_entity]. Type-1 peers
		// take one only at their open send boundary (see the fan below).
		bool any_record_recipient = false;
		for (const NapiNPConnection &conn : ctx.np_protocol.connection_list) {
			if (!is_in_match(conn)) continue;
			if (conn.type == NapiNPConnection::kTypeServerSide && !conn.s2c_send_boundary_open) continue;
			if (world.cached.local_player.valid() &&
					conn.link.owned_entity == world.cached.local_player)
				continue;
			any_record_recipient = true;
			break;
		}
		std::vector<GameEntitySnapshot> ents;
		if (any_record_recipient) {
			// Gameplay movement/destruction is complete. Replication LOS can retain
			// each target's final section matrices across every entity and recipient;
			// never inherit a view built during the earlier moving-world phases.
			{
				const devtools::ProfileScope prep_scope(
						world.profile, devtools::Slot::SIM_REPLICATION_QUERY_PREP);
				if (world.collision != nullptr)
					world.collision->prepare_cached_raycast_queries(world);
			}
			const devtools::ProfileScope snapshot_scope(
					world.profile, devtools::Slot::SIM_REPLICATION_SNAPSHOT);
			ents = netsim::snapshot_world(world);
			// Stamp owner-hidden rows: a spectator connection's own player entity
			// is admitted only to its owner's list (the JO reduction of the
			// slot+97536 hide byte — see replication_model.h owner_hidden)
			// [orig: Server_BuildEntityPriorityList @0x50e6fd].
			for (const NapiNPConnection &oc : ctx.np_protocol.connection_list) {
				if (!oc.link.spectator || !oc.link.owned_entity.valid()) continue;
				for (GameEntitySnapshot &es : ents) {
					if (es.wire_handle == oc.link.owned_entity.packed) {
						es.owner_hidden = true;
						break;
					}
				}
			}
		}
		for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
			if (!is_in_match(conn)) continue;
			// The host's type-2 loopback is an in-process seam and remains
			// full-rate (its own player takes the header-only frame inside the
			// fan). Type-1 peers receive one fresh 0x0A only when their
			// configured S2C send boundary opens; queuing all intervening
			// snapshots would burst stale frames at that boundary.
			if (conn.type == NapiNPConnection::kTypeServerSide && !conn.s2c_send_boundary_open) continue;
			const devtools::ProfileScope fan_scope(
					world.profile, devtools::Slot::SIM_REPLICATION_FAN);
			netsim::emit_connection_s2c(
					world, conn.link, ents, ctx.config.game_type,
					conn.type == NapiNPConnection::kTypeServerSide ? kMaxFrameUpdateBodyBytes : 0);
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
	lap.mark(devtools::Slot::SIM_SERVER_REPLICATION);

	// (4) flush is implicit: host_send staged each 0x0A on its transport. The loopback's local client
	// reads it via client_recv / ClientReplicaPipeline::pump; a remote peer's transport outbound_ is popped +
	// framed into a 0x83 SESSION by the owner (frame_in_match_s2c). No socket I/O in engine/.
}

} // namespace opennova::np
