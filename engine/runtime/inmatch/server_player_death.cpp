#include <runtime/inmatch/server_player_death.h>
#include <runtime/inmatch/server_tick.h>             // Server_DisarmPlayerTickSeed
#include <runtime/inmatch/server_idle_timers.h>      // breath_sample_limit
#include <runtime/inmatch/server_message_dispatch.h> // is_medic_recipient

#include <algorithm>
#include <cstdint>
#include <vector>

#include <base/io/le.h>                        // append_u16_le
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <runtime/world/ai.h>                  // AiSlot[1] BERSERK (the team-kill exemption)
#include <runtime/world/world.h>
#include <runtime/world/geom.h>                // to_fixed

namespace opennova::inmatch {

namespace {

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
	const uint8_t victim_index = world::pool0_index_byte(world::EntityHandle{death.victim_handle});
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

	const uint8_t killer_index = world::pool0_index_byte(world::EntityHandle{death.killer_handle});
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
	out.aux = world::pool0_index_byte(killer_entity->primary_occupant);
	return out;
}

// A player body's death edge as it runs: an unhidden body, still alive in
// ours, at Health <= 0 with its dead bit clear. [orig:
// Entity_UpdateInfantryPlayerBody -- the hidden bit @0x4B411E..0x4B4127,
// Health <= 0 @0x4B4BF1, the dead bit @0x4B4BFE]
bool player_edge_open(const world::World &world, world::EntityHandle victim) {
	const world::Entity *e = world.registry.get(victim);
	if (e == nullptr) return false;
	const uint32_t flags = e->flags | e->engine_flags;
	return e->alive && e->health <= 0 && (flags & world::kEntityFlagCarried) == 0u &&
			(flags & world::kEntityFlagDead) == 0u;
}

} // namespace

// Drain the queued deaths in order, each through Server_ProcessPlayerDeath,
// treating a player-body edge's record as `edges` says. The head tests the
// whole edge before the previous frame's medic heals route, which retail's
// entity update stored ahead of this frame's pump. After the converts it
// re-reads the flag bits alone: a convert hides the body, and a death the
// head routed for a record queued behind a held one (or for an earlier record
// on the same body) latched the dead bit; Health is not re-read, since ours
// routes the previous frame's heals after the pump. [orig:
// Entity_UpdateAllEntities -- Projectile_ProcessExplosionQueue @0x4C223F ->
// GameEvent_HandleMedicInteraction, the GameEvent_HealPlayer call @0x4E6805,
// Health = max @0x50DE7F]
void route_round_deaths(NapiNPServerCtx &ctx, world::World &world,
		EdgeDrain edges) {
	std::vector<world::RoundDeath> held;
	for (const world::RoundDeath &d : world.round_sim.deaths) {
		if (d.frame_tail_edge) { // the frame tail's (Server_RouteFrameTailEdges)
			held.push_back(d);
			continue;
		}
		if (d.player_edge && edges == EdgeDrain::AfterConverts) {
			// The edge's hidden bit and dead bit [orig: @0x4B411E..0x4B4127,
			// @0x4B4BFE].
			const world::Entity *e = world.registry.get(d.victim);
			if (e == nullptr || ((e->flags | e->engine_flags) &
					(world::kEntityFlagCarried | world::kEntityFlagDead)) != 0u)
				continue;
		} else if (d.player_edge) {
			if (!player_edge_open(world, d.victim)) continue;
			if (edges == EdgeDrain::HoldAtHead) {
				held.push_back(d);
				continue;
			}
		}
		Server_ProcessPlayerDeath(ctx, world, d);
	}
	world.round_sim.deaths = std::move(held);
}

// See header. [orig: Server_CheckPlayerViolations @0x51ABD0 - the Health
//  store @0x51ac94]
void kill_player_body_at_frame_tail(world::World &world, world::Entity *player) {
	// Health = -1 is the store alone: the player body's death edge picks
	// it up in this frame's entity update, after the tick's send pump and
	// ahead of the next receive pump; an already-dead body only keeps the
	// store. Ours raises a player body's transaction from the death queue
	// (D-AI-9), so a body still alive queues the edge's death for the
	// host's frame tail: the edge's test runs ahead of the frame's entity
	// pass (arm_frame_tail_edges), and the tail raises what it kept.
	const bool was_alive = player->alive && player->health > 0 &&
			((player->flags | player->engine_flags) &
					world::kEntityFlagDead) == 0u;
	player->health = -1;
	if (was_alive) {
		world::RoundDeath death;
		death.victim = player->handle;
		death.victim_handle = player->handle.packed;
		death.event_flags = player->cause_flags & 0xF00u;
		death.frame_tail_edge = true;
		world.round_sim.deaths.push_back(death);
	}
}

// See header. [orig: GameEvent_PlayerDeath @0x516DD0]
void Server_ProcessPlayerDeath(NapiNPServerCtx &ctx, world::World &world,
		const world::RoundDeath &d) {
	// The kill accounting of a damage-pass lethal edge (process_kill_event):
	// scorer event 12 in every session, then the SP tallies behind
	// world.rules.mp_session (our SP listen server always runs
	// ctx.is_in_session = 1). [orig: Score_ProcessKillEvent @0x4fd400]
	// An org1 (NPC) body's transaction is its motor edge's motor_edge record; a
	// damage-time record for it only tallies and leaves the dead bit to the edge.
	// [orig: Entity_UpdateInfantryAI @0x4B9D4D -> Entity_CheckAndProcessDeath
	//  @0x51B550; Score_ProcessKillEvent @0x4FD400 is called from damage paths only]
	if (!d.motor_edge && world::org1_owns_death_transaction(world, d.victim)) {
		world.match.process_kill_event(world, d);
		if (world::Entity *victim_entity = world.registry.get(d.victim))
			victim_entity->alive = false; // health is already <= 0
		return;
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
		// dfx2.exe stores 0 on both arms (DFX2-20): no window, so no S2C 0x54
		// below, and the 0x0A slot-state byte and the 0x46 downed byte stay 0.
		// [orig: GameEvent_PlayerDeath @0x516DD0: playerSlot+368 (@0x516f4f)]
		// [orig: GameEvent_PlayerDeath `mov [edi+170h], ebx` @0x51419b, the 0x54
		//  gate @0x5145cb (dfx2.exe)]
		const bool revive_window = world.rules.title != gameprofile::GAME_DFX2;
		victim_connection->link.downed_revive_seconds =
				revive_window && d.killer.valid() && d.killer != d.victim &&
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
		io::append_u16_le(body13, d.victim_handle);
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
		io::append_u16_le(body13, victim_entity != nullptr && !edge_consumes_slot
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
			io::append_u16_le(body1e, 0);
			io::append_u16_le(body1e, 0);
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
		//  filter @0x5173AF from the victim entity's byte, movsx @0x5173A8);
		//  NapiNPServer_SendFiltered @0x4C87E0]
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
				if (!is_medic_recipient(candidate, world,
						static_cast<int8_t>(victim_entity->team)))
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

	if (!d.motor_edge) world.match.process_kill_event(world, d);

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
	}
}

// The frame tail's player-body edges as this frame's entity update meets them:
// an unhidden body at Health <= 0 whose dead bit is clear. Tested ahead of the
// pass, whose local motor leg latches the dead bit as the edge does while the
// remote leg (a body that uplinked) latches nothing, and a record whose edge
// is shut is dropped: a body another transaction killed while the record
// waited for an entity update (a 0x4D in a pre-round frame) dies once.
// [orig: Entity_UpdateInfantryPlayerBody -- the hidden bit @0x4B411E..0x4B4127,
//  Health <= 0 @0x4B4BF1, the dead bit @0x4B4BFE]
void arm_frame_tail_edges(world::World &world) {
	std::vector<world::RoundDeath> &deaths = world.round_sim.deaths;
	deaths.erase(std::remove_if(deaths.begin(), deaths.end(),
			[&world](const world::RoundDeath &d) {
				if (!d.frame_tail_edge) return false;
				const world::Entity *body = world.registry.get(d.victim);
				return body == nullptr || body->health > 0 ||
						((body->flags | body->engine_flags) &
								(world::kEntityFlagCarried | world::kEntityFlagDead)) != 0u;
			}),
			deaths.end());
}

// See header. Retail's player-body edge latches the dead bit and raises the
// transaction in one step; in ours the local motor leg latches the bit in the
// pass (infantry_death.cpp) and the remote leg latches nothing, so the edge's
// test runs ahead of the pass (arm_frame_tail_edges, which drops a record whose
// edge is shut) and this tail raises the transaction for every record left;
// with no entity update this frame each waits.
// [orig: Game_ProcessMainFrame -- Server_TickUpdate's send pump @0x51E487,
//  then the gated Entity_UpdateAllEntities call @0x52674B; the edge
//  Entity_UpdateInfantryPlayerBody -- the hidden bit @0x4B411E..0x4B4127,
//  Health <= 0 @0x4B4BF1, the dead bit @0x4B4BFE, its latch @0x4B4CB5..0x4B4CDB,
//  Entity_CheckAndProcessDeath @0x4B4CEA]
void Server_RouteFrameTailEdges(NapiNPServerCtx &ctx) {
	if (ctx.world == nullptr || ctx.is_authority == 0 || !ctx.entity_update_ran) return;
	world::World &world = *ctx.world;
	if (std::none_of(world.round_sim.deaths.begin(), world.round_sim.deaths.end(),
				[](const world::RoundDeath &d) { return d.frame_tail_edge; }))
		return;
	std::vector<world::RoundDeath> queued = std::move(world.round_sim.deaths);
	world.round_sim.deaths.clear();
	for (const world::RoundDeath &d : queued) {
		if (!d.frame_tail_edge) {
			world.round_sim.deaths.push_back(d);
			continue;
		}
		const world::Entity *body = world.registry.get(d.victim);
		if (body == nullptr) continue;
		world::RoundDeath death = d;
		death.frame_tail_edge = false;
		// The transaction credits the body's +0x178 as the edge leaves it: the
		// pass's edge cleared it unless a non-lethal hit had staged the death
		// selection (+0x2C0) [orig: Entity_UpdateInfantryPlayerBody @0x4B4C72
		//  .. @0x4B4C8D; GameEvent_PlayerDeath reads +0x178 @0x516F47, the
		//  scoring @0x516FB0].
		death.killer = body->last_attacker;
		death.killer_handle = body->last_attacker.valid() ? body->last_attacker.packed : 0xFFFFu;
		Server_ProcessPlayerDeath(ctx, world, death);
	}
}

} // namespace opennova::inmatch
