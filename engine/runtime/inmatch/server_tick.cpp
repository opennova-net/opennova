#include <runtime/inmatch/server_tick.h>
#include <runtime/devtools/tick_profile.h>
#include <runtime/inmatch/end_round_protocol.h>
#include <runtime/inmatch/server_idle_timers.h>      // the every-32 breath samples
#include <runtime/inmatch/server_message_dispatch.h> // build_player_list_message
#include <runtime/inmatch/server_net_quality.h>      // the host CNetQuality sample + the 0x46 quality resend
#include <runtime/inmatch/server_medic.h>            // the medic revive and heal transactions
#include <runtime/inmatch/server_entity_routes.h>    // the item events, crossings and guidance
#include <runtime/inmatch/server_spawn.h>            // the admitted 0x51 spectator converts

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include <base/gameprofile/game_type.h>        // the CTF / FlagBall / Flag Me carry-limit modes
#include <base/io/le.h>                        // append_u16_le / append_u32_le (put_u16le / put_u32le)
#include <base/io/strutil.h>                   // iequals (the JOINTICKET key lookup)
#include <net/npwire/ingame_decode.h>          // kPlayerSyncHasDownedState (the 0x46 resend form)
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/nw_session_framing.h> // per-player 0x61 tick-seed roll
#include <net/npwire/protocol_message.h>
#include <net/npwire/replication_model.h>      // PlayerReplicationState (the 0x46 resend input)
#include <net/npwire/session_hello.h>
#include <runtime/replication/entity_wire_bridge.h> // snapshot_world / GameEntitySnapshot
#include <runtime/replication/connection_fan.h>     // drain_connection_c2s / emit_connection_s2c
#include <runtime/world/ai.h>                  // AiSlot[1] BERSERK (the team-kill exemption)
#include <runtime/world/world.h>
#include <runtime/world/collision.h>           // stable replication LOS view epoch
#include <runtime/world/geom.h>                // to_fixed
#include <runtime/world/local_player.h>        // the listen host's live inventory (kit weight)
#include <runtime/world/powerup.h>             // the remote players' powerup grants
#include <runtime/world/weapon_inventory.h>    // the grant's pool arithmetic
#include <runtime/world/minimap_overlay.h>      // portable Entity_ClassifyForMinimap result
#include <runtime/world/spawn_select.h>         // sorted SpawnZoneList index for capture events
#include <runtime/world/vehicle_motor.h>       // VehicleTraits (the 0x40 vehicle-blip icons)
#include <runtime/world/weapon_inventory.h>    // weapon_inventory_loadout_weight_fp16
#include <runtime/world/world.h>               // World::run_logic_tick
#include <runtime/world/zone_capture.h>        // the 1 Hz AS capture pass (slice 2)

#include <algorithm>
#include <string>

namespace opennova::inmatch {

namespace {

// Pool-0 index byte for the S2C 0x1E kill-feed actor fields (§5.26: u8 pool-0 index,
// 0xFF = none).
uint8_t pool0_index_byte(uint16_t handle) {
	const world::EntityHandle h{handle};
	if (!h.valid() || h.pool() != 0) return 0xFF;
	const int slot = h.slot();
	return slot <= 0xFE ? static_cast<uint8_t>(slot) : 0xFF;
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

// Classify the one S2C 0x1E record before Match drops the victim's carried
// objective. The original reads the LIVE entity+44 cause word (Entity::
// cause_flags, latched at hit time — a second same-tick critical pellet
// counts) in this priority order and clears the one bit it reports in place;
// the RoundDeath snapshot is not consulted here. In particular, 4091/4093/4095
// are carried flag ItemDefs (STRCND22 "killed flag carrier"), 0x800 is the
// STRCND08 headshot family, and AMMO_60MM_MORTAR selects STRCND47.
// The 0x100 branch really is a retail quirk: its CRT rand() is shifted to a
// 0..127 word and then divided by 21845, so this producer always emits 32 even
// though the client retains strings for 33/34.
// [orig: GameEvent_PlayerDeath @0x516DD0, classifier @0x5170E0..0x51724A;
// the entity+44 reads @0x517180 / @0x517311; the reported-bit clears
// @0x5171ca (0x5171b4) / @0x5171e8 / @0x517206 / @0x517325;
// the GameEvent_BuildPayload call @0x517362 (0x51737b is the payload-length
// store that follows it)]
PlayerDeathFeed classify_player_death(
		world::World &world, const world::RoundDeath &death,
		world::Entity *victim_entity,
		const world::Entity *killer_entity,
		uint32_t underwater_breath_samples) {
	PlayerDeathFeed out;
	const uint8_t victim_index = pool0_index_byte(death.victim_handle);
	const uint32_t cause =
			victim_entity != nullptr ? victim_entity->cause_flags : 0u;
	auto clear_cause = [victim_entity](uint32_t bit) {
		if (victim_entity != nullptr) victim_entity->cause_flags &= ~bit;
	};
	if (killer_entity == nullptr) {
		// [orig: `cmp [edi+1CCh],ecx; jle` @0x517302]
		if (static_cast<int32_t>(underwater_breath_samples) > breath_sample_limit(world)) {
			out.event_type = 26;
		} else if ((cause & 0x200u) != 0u) {
			out.event_type = 23;
			clear_cause(0x200u); // [orig: @0x517325]
		}
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
		const world::AiEntity *ai = world.ai.for_handle(handle);
		return ai != nullptr && (ai->slot.f[world::AiSlot::kBehaviorFlags] & 0x200) != 0;
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
	} else if ((cause & 0x100u) != 0u) {
		// The roll still consumes one CRT draw before its narrowed quotient
		// lands on 32, so the draw order of the shared stream stays
		// structural. [orig: rand @0x51718A; imul/sar @0x51719A..0x5171A9]
		(void)world.crt_rand.next();
		out.event_type = 32;
		clear_cause(0x100u); // [orig: @0x5171ca / @0x5171b4]
	} else if ((cause & 0x800u) != 0u) {
		out.event_type = death_family_variant(world, 10);
		clear_cause(0x800u); // [orig: @0x5171e8]
	} else if ((cause & 0x400u) != 0u) {
		out.event_type = death_family_variant(world, 13);
		clear_cause(0x400u); // [orig: @0x517206]
	} else {
		const world::AmmoTableEntry *ammo =
				world.tables.ammo.by_index(death.ammo_index);
		out.event_type = ammo != nullptr &&
					world.tables.ammo.index_of("AMMO_60MM_MORTAR") == death.ammo_index
				? 49u
				: death_family_variant(world, 4);
	}
	out.attacker = killer_index;
	out.victim = victim_index;
	out.aux = pool0_index_byte(killer_entity->primary_occupant.packed);
	return out;
}

void put_u16le(std::vector<uint8_t> &v, uint16_t x) {
	opennova::io::append_u16_le(v, x);
}

void put_u32le(std::vector<uint8_t> &v, uint32_t x) {
	opennova::io::append_u32_le(v, x);
}

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
				conn.link.mode != replication::TransportMode::Loopback) ||
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
					world::classify_minimap_overlay(e, &world);
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

// Drain each transport-free RoundDeath through the one retail player-death
// transaction: 0x13 remote fan, victim 0x61 seed, victim 0x52 camera, optional
// 0x1E active-player feed, conditional 0x54 Medic state, scoring, and respawn
// holds. AI victims stop after the 0x13/scoring leg. That transaction is the
// organic body's alone: its only callers are the player body, the infantry AI
// and the console kill. Vehicles and items keep only the score ledger and the SP
// tally here; their class death paths own Flags and the S2C 0x26 kill record.
// [orig: Entity_CheckAndProcessDeath @0x51B550, called only from
// Entity_UpdateInfantryPlayerBody @0x4B4CEA, Entity_UpdateInfantryAI @0x4B9D4D
// and the console kill @0x4D29EC -> GameEvent_PlayerDeath @0x516DD0; the only
// 0x13 sends are GameEvent_PlayerDeath @0x516E8E and
// Entity_CheckAndProcessDeath @0x51B58F]
void route_round_deaths(NapiNPServerCtx &ctx, world::World &world) {
	if (world.round_sim.deaths.empty()) return;
	auto tally_kill = [&](const world::RoundDeath &d) {
		// The kill accounting of a damage-pass lethal edge: scorer event 12 in
		// every session, then the SP tallies behind world.rules.mp_session (our
		// SP listen server always runs ctx.is_in_session = 1).
		// [orig: Score_ProcessKillEvent @0x4fd400]
		world.match.process_kill_event(world, d);
	};
	for (const world::RoundDeath &d : world.round_sim.deaths) {
		// An org1 (NPC) body's transaction is its motor edge's motor_edge record; a
		// damage-time record for it only tallies and leaves the dead bit to the edge.
		// [orig: Entity_UpdateInfantryAI @0x4B9D4D -> Entity_CheckAndProcessDeath
		//  @0x51B550; Score_ProcessKillEvent @0x4FD400 is called from damage paths only]
		if (!d.motor_edge && world::org1_owns_death_transaction(world, d.victim)) {
			tally_kill(d);
			if (world::Entity *victim_entity = world.registry.get(d.victim))
				victim_entity->alive = false; // health is already <= 0
			continue;
		}
		world::Entity *victim_entity = world.registry.get(d.victim);
		const world::Entity *killer_entity = world.registry.get(d.killer);
		const bool victim_is_player = victim_entity != nullptr &&
				(victim_entity->flags & world::kEntityFlagPlayer) != 0u;
		// An edge record's victim is a person even when its corpse leg removed the row.
		const bool organic_victim = d.motor_edge || (victim_entity != nullptr &&
				victim_entity->kind == world::EntityKind::Organic);
		// The revive-window and resend gates read the victim's live entity+44
		// cause word BEFORE the classifier clears the bit it reports.
		// [orig: GameEvent_PlayerDeath @0x516f4d precedes the ladder @0x517180]
		const uint32_t victim_cause_bits =
				victim_entity != nullptr ? victim_entity->cause_flags : 0u;
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
		world.match.record_death(world, d.victim, d.killer, d.event_flags);
		// Mark the victim DEAD on the entity: Flags bit1 is the wire-dead signal — the
		// victim's OWN client learns of its death from its record byte13 bit 0x02
		// (the LOCAL apply's dead path stores the anim + zeroes Health -> the death
		// screen + the redeploy flow), and everyone else's dead-state masks read it too.
		// Cleared by entity_reset_to_spawn_state at the deploy — the wire 1->0 edge IS
		// the client spawn hook (pose snap + reset). Without this bit the victim never
		// knows it died (v33). [orig: the death path sets entity+36 bit1; §5.10 off-13
		// "bit 0x02 = DEAD/UNDEPLOYED", apply @0x4c1005-0x4c1027, edge @0x4c1109]
		// An edge record's row carries the edge's own latch (or a respawned life).
		if (organic_victim && !d.motor_edge) {
			victim_entity->flags |= 2u;
			victim_entity->alive = false;
			// The dead/protection latch is the player leg's alone.
			// [orig: Entity_CheckAndProcessDeath tests Flags & 0x100
			// @0x51B555..0x51B55D before GameEvent_PlayerDeath]
			if (victim_is_player) victim_entity->damage_state = -1;
		}
		if (victim_connection != nullptr) {
			// A normal other-player kill opens the exact 120-second revive
			// window. Self/environment and knife/headshot cause bits clear it.
			// [orig: GameEvent_PlayerDeath @0x516DD0: playerSlot+368]
			victim_connection->link.downed_revive_seconds =
					d.killer.valid() && d.killer != d.victim &&
					(victim_cause_bits & 0xC00u) == 0u
							? 120u
							: 0u;
			victim_connection->link.medic_request_active = false;
			// The team-mode 1 Hz 0x46 downed resend reads only the cause bits,
			// never the killer: self/environment deaths ARE resent (with a zero or
			// request-only downed byte). [orig: Server_TickUpdate @0x51e333 tests
			//  `(entity+44 & 0xC00) == 0`; the 0x800 writer is Weapon_CalcImpactDamage
			//  @0x4ec9c6/@0x4ec994]
			victim_connection->link.death_cause_revivable =
					(victim_cause_bits & 0xC00u) == 0u;
		}

		if (ctx.is_in_session && organic_victim) {
			std::vector<uint8_t> body13;
			put_u16le(body13, d.victim_handle);
			// word1 = the victim's entity+0x2C0 death-anim slot AS THE SENDER
			// SEES IT — never the killer. Both infantry death edges consume the
			// slot into the anim state and ZERO it before the authority-gated
			// Entity_CheckAndProcessDeath call, so an edge-driven infantry
			// death always ships 0 (every 0x13 in the retail capture carries
			// 0); only the direct third sender ships a live slot. An org1
			// body's record comes from its own edge, after the consume; a
			// player body's death routing runs in the damage tick, before the
			// next body update's edge consumes the staged selection, so a person
			// victim reports the post-edge zero here rather than the selection
			// its edge still owns; every other kind ships the slot as stored.
			// The retail receiver stores the word sign-extended into +0x2C0.
			// [orig: NetPacket_BuildDeathNotifyPayload @0x5036E0 (movzx word [esi+2C0h]
			//  @0x503733, store @0x50374A); edges @0x4B9D38 -> @0x4B9D4D (AI)
			//  and @0x4B4CD5 -> @0x4B4CEA (player body); direct sender
			//  @0x4D29EC; receiver NapiNPClientMsg_EntityDeath @0x42EB8D/@0x42EBDF]
			const bool edge_consumes_slot = victim_entity != nullptr &&
					(victim_entity->item_type == 3 ||
					 (victim_entity->item_type == 0 &&
					  victim_entity->kind == world::EntityKind::Organic));
			put_u16le(body13, victim_entity != nullptr && !edge_consumes_slot
					? static_cast<uint16_t>(victim_entity->death_anim_state)
					: uint16_t{0});
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
				if (c.link.mode == replication::TransportMode::Loopback) continue;
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
						Server_DisarmPlayerTickSeed(*victim_connection, world.logic_tick));
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

			// Finally publish the revive window to in-match same-team Medics
			// (mask 0x580 tests slot state 6/7, the team byte and the Medic
			// class — never the medic's own health or dead bit, so a dead
			// medic is a recipient too). Manual Auto-Medic preference first
			// clears their marker and sends the live window to the victim
			// alone; automatic mode sends the window directly to the Medic
			// group. Mask 0x580 does include the host.
			// [orig: GameEvent_PlayerDeath @0x516DD0 (mask @0x51739C, team
			//  filter @0x5173AF); NapiNPServer_SendFiltered @0x4C87E0]
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

		if (!d.motor_edge) tally_kill(d);

		if (victim_connection != nullptr) {
			// Every player slot gets the same two whole-second counters. A
			// configured timeout below three is floored; a spawn less than 620
			// authority ticks ago forces exactly three. +364 retains the greater
			// value only when a spawn-target registry exists.
			// [orig: GameEvent_PlayerDeath @0x516ec4..0x516eeb]
			replication::Connection &link = victim_connection->link;
			uint32_t hold = std::max(ctx.config.respawn_timeout, 3u);
			if (link.last_deploy_tick_valid &&
					static_cast<uint32_t>(world.logic_tick - link.last_deploy_tick) < 620u)
				hold = 3;
			link.respawn_delay_seconds = hold;
			link.spawn_target_hold_seconds = world.zones.has_spawn_zone()
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
	const bool dead = !local->alive || (local->flags & 2u) != 0;
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
	ctx.round_end_announced = true;
	ctx.round_end_linger_ticks = 2790;
	// A ServerCommand Cycle / EndMission / GameOver overrides the stored
	// linger right after the round end [orig: CNapiGameSession_HandleServerCommand @0x4D31CA].
	if (ctx.round_end_linger_override_ticks != 0) {
		ctx.round_end_linger_ticks = ctx.round_end_linger_override_ticks;
		ctx.round_end_linger_override_ticks = 0;
	}
	return true;
}

// The listen host has no socket-side death picker in the current presentation,
// so expiry supplies the Default Spawn command locally. It still enters the ONE
// deployment transaction used by C2S 0x0E and spawn-wave releases; there is no
// second entity-reset implementation. Remote players remain dead until a pick.
void release_expired_local_respawns(NapiNPServerCtx &ctx, world::World &world) {
	if (world.match.outcome().ended) return;
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (conn.link.mode != replication::TransportMode::Loopback ||
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
					message.flags.raw, message.capacity_exempt, message.retention_flushes);
	}
}

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
		// The +376 emote cooldown [orig: @0x51e028..0x51e03f].
		if (conn.link.emote_cooldown_seconds > 0)
			--conn.link.emote_cooldown_seconds;
		else if (conn.link.emote_cooldown_seconds < 0)
			conn.link.emote_cooldown_seconds = 0;
	}
}

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
void tick_spawn_protection(NapiNPServerCtx &ctx, world::World &world) {
	if (world.match.outcome().ended || world.preround_delay_seconds != 0) return;
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (!is_in_match(conn) || !conn.link.owned_entity.valid()) continue;
		world::Entity *player = world.registry.get(conn.link.owned_entity);
		if (player == nullptr) continue;
		if (world.rules.mp_session) {
			if (conn.link.spectator)
				player->damage_state = -1;
			else if (player->damage_state > 0)
				--player->damage_state;
		} else {
			player->damage_state = 0;
		}
	}
}

// The frontier-hint arm of the same walk: a state-6 slot whose bit 0x04 is set
// and which has played 1240 ticks gets S2C 0x1E event 58 [its team's frontier
// zone][the enemy's, 0 when the same][0xFF] (mask 0xA0, itself) unless its own
// frontier is 0; either way the bit clears.
// [orig: Server_UpdateAllActivePlayerSlots @0x518820 — @0x51890C..0x5189A6]
void emit_frontier_hints(NapiNPServerCtx &ctx, world::World &world) {
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (!is_in_match(conn) || !conn.reply.frontier_hint_pending) continue;
		const world::Entity *player = world.registry.get(conn.link.owned_entity);
		const world::MatchPlayer *slot = world.match.player(conn.link.owned_entity);
		if (player == nullptr || slot == nullptr || slot->play_ticks < 1240u) continue;
		const uint8_t own = world.zones.frontier_zone(player->team);
		uint8_t enemy = world.zones.frontier_zone(player->team == 1 ? 2 : 1);
		if (enemy == own) enemy = 0;
		if (own != 0 && conn.link.transport != nullptr)
			conn.link.transport->host_send(s2c::GAME_EVENT,
					{0x3A, own, enemy, 0xFF, 0, 0, 0, 0});
		conn.reply.frontier_hint_pending = false;
	}
}

// A refused touch arms its slot's nag: not while one is held, and only when the
// slot's +100360 stamp is set and over ten seconds old; it sets bits
// 0x04|0x08 and restamps the word.
// [orig: Server_OnPlayerTouchCaptureZone @0x500BA0 — the slot
//  @0x500BE4..0x500BF1, bit 0x08 @0x500C10, the stamp @0x500C19..0x500C33, the
//  set @0x500C35..0x500C3C]
void arm_refused_capture_nags(NapiNPServerCtx &ctx, world::World &world) {
	const uint32_t now_ms = host_milliseconds_for_logic_tick(world.logic_tick);
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
// (`suicides > 9` -> punt t6), else the team-kill arm punts t6. The host-local
// punt log line ("#S>9") and the global punts-off switch dword_B4C698 are not
// wire state and are not modeled; stage_host_punt already carries the
// first-event latch (slot dword +89896).
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
				// Health = -1; a body that is still alive enters the same death
				// transaction the breath kill uses (the retail entity death path
				// picks the store up on its next update; an already-dead body only
				// keeps the store).
				const bool was_alive = player->alive && player->health > 0 &&
						((player->flags | player->engine_flags) &
								world::kEntityFlagDead) == 0u;
				player->health = -1;
				if (was_alive) {
					world::RoundDeath death;
					death.victim = player->handle;
					death.victim_handle = player->handle.packed;
					death.event_flags = player->cause_flags & 0xF00u;
					world.round_sim.deaths.push_back(death);
				}
			}
		}
		if (conn.link.mode == replication::TransportMode::Loopback) continue;
		const world::MatchPlayer *stats = world.match.player(player->handle);
		if (stats == nullptr) continue;
		const int32_t limit = ctx.config.max_friendly_kills;
		if (limit < 0 || stats->stats[world::MatchStats::kTeamKills] <= limit) {
			if (stats->stats[world::MatchStats::kSuicides] > 9)
				stage_host_punt(conn, 6);
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
// [orig: sub_517B20 @0x517B20 — authority @0x517b27, def @0x517b55, ids
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
		state.pos_x = world::to_fixed(e.position.x);
		state.pos_y = world::to_fixed(e.position.y);
		state.pos_z = world::to_fixed(e.position.z);
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

// The team-mode downed-state resend: in session, when the game type carries the
// team bit, a 62-tick countdown (fires when zero or when a positive value
// decrements to zero, then reloads 62) walks every active slot with an entity
// and, for each one whose entity is dead (Health <= 0 and Flags 0x2) of a
// revivable cause, sends the 0x46 field-0x0008 form — [u8 slot][u16 0x0008]
// [u8 pool-0 index][u8 downedState] — to every state-6/7 slot on the victim's
// team, the listen host included. downedState is the dead-and-(auto-medic or
// requested) packing of the revive window and the request bit, else 0.
// Retail's SendFiltered(0x46, msgClass 1, userParam 1) maps to this port's
// transient send, as the 0x57 pong and the 0x16 list do.
// [orig: Server_TickUpdate @0x51E2D0..0x51E378 — team test @0x51e2d0, timer
//  @0x51e2db..0x51e307, predicate @0x51e333, form 8 @0x51e352, mask 0x180
//  @0x51e357, team filter @0x51e36d, send @0x51e373;
//  NetPacket_SerializePlayerSync0x46 @0x505E80 downedState @0x5060e6..0x506131;
//  NapiNPServer_SendFiltered @0x4C87E0 masks 0x80/0x100 @0x4c8948/@0x4c8964]
void emit_team_downed_resend(NapiNPServerCtx &ctx, world::World &world) {
	if (!ctx.is_in_session ||
			(world.match.rules().game_type & game_type::kTeamBit) == 0u)
		return;
	bool fire = world.team_downed_resend_countdown == 0;
	if (world.team_downed_resend_countdown > 0)
		fire = --world.team_downed_resend_countdown == 0;
	if (!fire) return;
	world.team_downed_resend_countdown = 62;
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (conn.phase < ConnectionPhase::PlayerAdded ||
				conn.phase >= ConnectionPhase::Goodbye ||
				!conn.link.owned_entity.valid())
			continue;
		const world::Entity *victim = world.registry.get(conn.link.owned_entity);
		if (victim == nullptr || victim->health > 0 ||
				((victim->flags | victim->engine_flags) & world::kEntityFlagDead) == 0u ||
				!conn.link.death_cause_revivable)
			continue;
		PlayerReplicationState rep;
		rep.player_slot = conn.reply.player_slot;
		rep.entity_handle = conn.link.owned_entity.packed;
		rep.team = victim->team;
		if (conn.link.auto_medic_enabled || conn.link.medic_request_active)
			rep.downed_state = static_cast<uint8_t>(
					(conn.link.downed_revive_seconds & 0x7Fu) |
					(conn.link.medic_request_active ? 0x80u : 0u));
		const std::vector<uint8_t> body =
				encode_player_sync(rep, kPlayerSyncHasDownedState);
		for (NapiNPConnection &recipient : ctx.np_protocol.connection_list) {
			if (!active_player_recipient(recipient)) continue;
			const world::Entity *member =
					world.registry.get(recipient.link.owned_entity);
			if (member == nullptr || member->team != victim->team) continue;
			recipient.link.transport->host_send(
					s2c::PLAYER_SYNC, body, /*reliable=*/false);
		}
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

// The "<localaddr>JOINTICKET" lookup in the joiner's CD identity pairs
// [orig: CNapiGameSession_SendPlayEnterRequest @0x4D0312..0x4D0362 —
//  String_ConcatTwoSafe(localAddress, "JOINTICKET") then
//  KeyValueBuffer_FindValue @0x4C2A30; a failed lookup rides an empty ticket].
std::string join_ticket_for(const NapiNPServerCtx &ctx, const NapiNPConnection &conn) {
	if (ctx.host_local_address.empty()) return {};
	const std::string key = ctx.host_local_address + "JOINTICKET";
	for (const auto &pair : conn.join_identity_pairs) {
		if (strutil::iequals(pair.first, key)) return pair.second;
	}
	return {};
}

} // namespace

// The join-phase validation watchdog, once per periodic second, over every
// accepted 0x42 that has not completed its admission (NetPlayer game states
// 3 and 4); the receive-silence reap never fires for a peer that keeps
// sending, so these are absolute deadlines from the validation stamp.
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
				conn.phase < ConnectionPhase::Joined ||
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

namespace {

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
		put_u32le(body, host_milliseconds_for_logic_tick(world.logic_tick));
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
		// The dead-age arm owns an independent consecutive-SECOND counter. It
		// does not derive elapsed time from Entity::death_tick: the counter and
		// its compare sit inside the g_PeriodicSecondTimer block (reload 62),
		// so retail increments the player-slot dword once per periodic second
		// while a state-6 entity has Flags bit 0x02 set, resets it as soon as
		// the bit clears, and compares after the increment — punt type 7 lands
		// after 361 seconds dead, not 361 ticks (5.8 s). The remaining retail
		// exclusions (local player, bot/spectator, explicit anti-cheat bypass)
		// have no remote-player representation in this runtime; a normal type-1
		// connection corresponds to all of them being clear.
		// [orig: Server_TickUpdate @0x51D7E0 — periodic gate reload 62
		//  @0x51DB93; counter @0x51E066..0x51E07D; punt @0x51E187..0x51E18E]
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
					!ctx.config.permanent_death) {
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
	put_u32le(body, connection.tick_seed);
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
// body every tick); a remote player's rows are the granted combos with their
// live clips over the authority pool table, stamped straight onto the body
// the host already runs player_body_select for. Called once per periodic
// second and after every accepted loadout. The sub-variant rows of a remote
// kit are not modeled host-side (their pools never reach the 0x0F image), so
// a remote weight omits the differing-class sub term; that is the D-NET-152
// shared-pool tail, not a second mechanism.
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
			local->loadout.weight_fp16 =
					world::weapon_inventory_loadout_weight_fp16(table, local->inventory);
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
		body->inf.loadout_weight_fp16 =
				world::weapon_inventory_loadout_weight_fp16(table, inventory);
	}
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
// A remote player's powerup pickup: the per-class adds and the `allammo`
// re-seed land on the owning connection's pools and slot rows, the way retail's
// authority arms write the validated entity's per-connection tables (the listen
// host's own player writes its live inventory inside world/powerup.cpp). The
// joiner ran the same pickup on its own pools, so no message follows. Only the
// slots the host has seeded take the re-seed (D-PWR-4, docs/world/powerup-re.md).
// [orig: WeaponSlot_AddAmmo @0x540A20 -- Entity_ValidatePtr @0x540AC5, the pool
//  add and cap clamp @0x540AD8..0x540AF1; Entity_UpdateWeaponOverlayFrameState
//  @0x4DC340 -- WeaponSlots_SeedAmmoPoolsFromDefs @0x4DC373 then
//  WeaponSlots_RecalculateAmmoFromCapacity @0x4DC38F over the slot tables]
void Server_ApplyPowerupGrants(std::vector<NapiNPConnection> &roster, world::World &world) {
	if (world.out.powerup_grants.empty()) return;
	const world::WeaponTable &table = world.tables.weapons;
	for (const world::PowerupGrant &grant : world.out.powerup_grants) {
		if (table.empty()) break;
		for (NapiNPConnection &conn : roster) {
			if (conn.phase < ConnectionPhase::PlayerAdded ||
					conn.phase >= ConnectionPhase::Goodbye ||
					conn.link.owned_entity != grant.picker)
				continue;
			const world::Entity *body = world.registry.get(grant.picker);
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
			for (size_t i = 0; i < pool_count; ++i) {
				inventory.pools[i] = conn.reply.ammo_pools[i];
				inventory.shared_clips[i] = conn.reply.shared_clips[i];
			}
			if (grant.allammo) {
				world::weapon_inventory_seed_pools(
						table, inventory, body != nullptr ? body->player_class : 0);
				world::weapon_inventory_recalc_clips(table, inventory);
			}
			for (const auto &[class_id, amount] : grant.ammo_adds)
				world::weapon_pool_add(table, inventory, class_id, amount);
			for (size_t i = 0; i < pool_count; ++i) {
				conn.reply.ammo_pools[i] = inventory.pools[i];
				conn.reply.shared_clips[i] = inventory.shared_clips[i];
			}
			for (auto &[combo, row] : conn.weapon_slots)
				if (const world::WeaponInventorySlot *slot = inventory.slot(combo))
					row.clip = static_cast<int16_t>(slot->clip);
			break;
		}
	}
	world.out.powerup_grants.clear();
}

static void route_entity_pass_records(NapiNPServerCtx &ctx, world::World &world) {
	Server_FanEntityEvents(ctx, world);
	route_throwable_events(ctx, world);
	route_round_deaths(ctx, world);
	Server_RouteMedicInteractions(ctx, world);
	Server_ApplyPowerupGrants(ctx.np_protocol.connection_list, world);
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
	// Then the spawn-protection arm of the per-player maintenance walk
	// [orig: Server_UpdateAllActivePlayerSlots @0x518820, called @0x51d88b].
	tick_spawn_protection(ctx, world);
	emit_frontier_hints(ctx, world);
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

	// (1b) The WAC 'humans' count, rebuilt each server tick just before the script
	// pass. The original also uses it as the empty-server world-run gate
	// (entities/WAC advance while humans > 0 || ticks == 0).
	// [orig: Server_BuildEntitySlotLists @0x4f97a0, called from Server_TickUpdate
	//  @0x51d89a before the WAC pre-pass]
	world.cached.humans = world.registry.count_humans();

	// The C2S 0x51 spectator converts the dispatcher admitted this frame run
	// inline in retail's receive dispatch, ahead of the state fan.
	Server_ProcessSpectatorRespawnRequests(ctx, world);
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
			stage_host_disconnect(conn, event);
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
	//  @0x51df64 sub_517B20, then @0x51df6e SpawnWaveList_Tick]
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
				std::vector<ProtocolMessage> deployment =
						Server_ReleasePlayerDeployment(
								ctx.config, conn, world, release.zone);
				for (ProtocolMessage &message : deployment)
					conn.link.transport->host_send(
							message.tag, std::move(message.payload),
							message.reliable, message.flags.raw,
							message.capacity_exempt, message.retention_flushes);
				break;
			}
		}
	}

	// Queue per-peer retail maintenance on this tick so the requests share
	// HostSession's next open S2C boundary with the 0x0A fan above.
	if (!world.match.outcome().ended) {
		emit_periodic_session_maintenance(ctx, world, periodic_second);
		emit_minimap_overlay_state(ctx, world);
	}

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
		auto connection_team = [&](const NapiNPConnection &conn) -> uint8_t {
			if (!conn.link.owned_entity.valid()) return 0;
			const world::Entity *entity =
					world.registry.get(conn.link.owned_entity);
			return entity != nullptr ? entity->team : 0;
		};
		// Mask 0x180: in-match slots whose team is the filter team.
		// [orig: NapiNPServer_SendFiltered bits 0x80/0x100]
		auto send_team = [&](uint8_t team, const std::vector<uint8_t> &body) {
			for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
				if (!is_in_match(conn) || conn.link.transport == nullptr ||
						connection_team(conn) != team)
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
			if (flip == nullptr || !flip->announce) continue;
			const uint8_t capturer_idx = pool0_index_byte(flip->capturer.packed);
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
				const uint8_t own_team = flip->capturer_team;
				const uint8_t enemy_team = own_team == 1 ? 2 : 1;
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
								host_milliseconds_for_logic_tick(world.logic_tick);
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
								world, conn.link.owned_entity),
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
	emit_team_downed_resend(ctx, world);

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
		// The priority build runs only for recipients that take entity records:
		// the listen host's own player gets the header-only frame and never
		// walks the pools [orig: Server_SendEntityStateToPlayer @0x517c1b skips
		// Server_BuildEntityPriorityList for g_LocalPlayerEntity]. Type-1 peers
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
			// The previous tick's movement/destruction is settled. Replication LOS
			// can retain each target's final section matrices across every entity
			// and recipient; never inherit a view built while the motor is moving.
			{
				const devtools::ProfileScope prep_scope(
						world.profile, devtools::Slot::SIM_REPLICATION_QUERY_PREP);
				if (world.collision != nullptr)
					world.collision->prepare_cached_raycast_queries(world);
			}
			const devtools::ProfileScope snapshot_scope(
					world.profile, devtools::Slot::SIM_REPLICATION_SNAPSHOT);
			ents = replication::snapshot_world(world);
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
			conn.link.receive_silence_ms = conn.receive_inactive_ms;
			// Built here and queued just below: the frame's CONTENT is the world
			// after the script pass and the maintenance, before the motor step.
			// The server-fps byte is this tick's copy of the main loop's FR
			// counter [orig: Server_TickUpdate @0x51D7E0..0x51D7E5].
			conn.frame_update_staged = replication::build_connection_s2c(
					world, conn.link, ents, conn.staged_frame_update, ctx.config.game_type,
					conn.type == NapiNPConnection::kTypeServerSide ? kMaxFrameUpdateBodyBytes : 0,
					ctx.stats_avg_fps);
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
	// count; automatic multiplayer announcements start draining next tick. At expiry,
	// the session replication gate closes while the frozen result stays readable.
	// [orig: store @0x5166C4; phase/drain @0x51DA04; exit reason 3 @0x51DA91]
	if ((round_was_announced || round_ended_at_retail_linger_phase) &&
			ctx.round_end_linger_ticks > 0) {
		--ctx.round_end_linger_ticks;
		if (ctx.round_end_linger_ticks == 0) ctx.is_in_session = 0;
	}
	Server_RouteGuidance(ctx, world);

	emit_periodic_rtt(ctx, world);
	lap.mark(devtools::Slot::SIM_SERVER_REPLICATION);

	// (4) The entity pass: Game_ProcessMainFrame runs the gated entity update
	// once Server_TickUpdate has sent this frame's 0x0A, then the weapon pump
	// and the frame's tail. Its inline sends reach the wire in the next frame,
	// routed at the head of the next call.
	// [orig: Game_ProcessMainFrame — the Server_TickUpdate call @0x5266B6, the
	//  entity gate @0x526703..0x526742, the Entity_UpdateAllEntities call
	//  @0x52674B]
	world.run_entity_pass(tick);

	// (5) flush is implicit: host_send staged each 0x0A on its transport. The loopback's local client
	// reads it via client_recv / ClientReplicaPipeline::pump; a remote peer's transport outbound_ is popped +
	// framed into a 0x83 SESSION by the owner (frame_in_match_s2c). No socket I/O in engine/.
}

} // namespace opennova::inmatch
