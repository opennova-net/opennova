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

// The batch a transaction's record names, onto the transports of the
// connections still there: a connection dropped since the entity update takes nothing, as
// retail's slot teardown discards its queue.
void release_staged_sends(NapiNPServerCtx &ctx, uint32_t seq) {
	auto batch = std::find_if(ctx.entity_update_sends.begin(), ctx.entity_update_sends.end(),
			[seq](const StagedEntityUpdateBatch &b) { return b.seq == seq; });
	if (batch == ctx.entity_update_sends.end()) return;
	for (StagedEntityUpdateSend &send : batch->sends) {
		for (NapiNPConnection &c : ctx.np_protocol.connection_list) {
			if (c.connection_id != send.connection_id || c.link.transport != send.transport ||
					send.transport == nullptr)
				continue;
			send.transport->host_send(send.tag, std::move(send.body));
			break;
		}
	}
	ctx.entity_update_sends.erase(batch);
}

} // namespace

// See header.
void send_entity_update_message(NapiNPServerCtx &ctx, NapiNPConnection &c, uint8_t tag,
		std::vector<uint8_t> body) {
	if (ctx.entity_update_staging != nullptr) {
		StagedEntityUpdateSend send;
		send.connection_id = c.connection_id;
		send.transport = c.link.transport;
		send.tag = tag;
		send.body = std::move(body);
		ctx.entity_update_staging->push_back(std::move(send));
		return;
	}
	c.link.transport->host_send(tag, std::move(body));
}

// See header.
EntityUpdateStaging::EntityUpdateStaging(NapiNPServerCtx &ctx, world::World &world,
		world::EntityHandle victim)
	: ctx_(ctx), world_(world), victim_(victim) {
	ctx_.entity_update_staging = &sends_;
}

EntityUpdateStaging::~EntityUpdateStaging() {
	ctx_.entity_update_staging = nullptr;
	if (++ctx_.entity_update_batch_seq == 0) ++ctx_.entity_update_batch_seq;
	StagedEntityUpdateBatch batch;
	batch.seq = ctx_.entity_update_batch_seq;
	batch.sends = std::move(sends_);
	ctx_.entity_update_sends.push_back(std::move(batch));
	world::RoundDeath marker;
	marker.victim = victim_;
	marker.victim_handle = victim_.packed;
	marker.staged_sends = ctx_.entity_update_batch_seq;
	world_.round_sim.deaths.push_back(marker);
}

// Drain the queued deaths in order. A transaction's record (a player body's
// edge, the explosion queue's medic arm) releases the sends the transaction
// staged in the entity update; a damage-time record for
// a body whose edge raises its transaction only tallies (an org2 body's here,
// an org1 body's in Server_ProcessPlayerDeath); every other record runs the
// transaction. The head's drain then drops any batch whose record is gone.
// [orig: Game_ProcessMainFrame -- the Server_TickUpdate call @0x5266B6 (its
//  send pump @0x51E487), then Entity_UpdateAllEntities @0x52674B, whose
//  player-body edge calls Entity_CheckAndProcessDeath
//  (Entity_UpdateInfantryPlayerBody @0x4B4CEA); Score_ProcessKillEvent
//  @0x4FD400 from the damage paths only]
void route_round_deaths(NapiNPServerCtx &ctx, world::World &world,
		EdgeDrain edges) {
	std::vector<world::RoundDeath> queued = std::move(world.round_sim.deaths);
	world.round_sim.deaths.clear();
	for (const world::RoundDeath &d : queued) {
		if (d.staged_sends != 0) {
			release_staged_sends(ctx, d.staged_sends);
			continue;
		}
		if (!d.motor_edge && world::org2_owns_death_transaction(world, d.victim)) {
			world.match.process_kill_event(world, d);
			continue;
		}
		Server_ProcessPlayerDeath(ctx, world, d);
	}
	if (edges != EdgeDrain::HoldAtHead) return;
	ctx.entity_update_sends.clear();
}

// See header. [orig: Server_CheckPlayerViolations @0x51ABD0 - the Health
//  store @0x51ac94]
void kill_player_body_at_frame_tail(world::World &, world::Entity *player) {
	// Health = -1 is the store alone: the player body's death edge picks it up
	// in this frame's entity update, after the tick's send pump and ahead of the
	// next receive pump; an already-dead body only keeps the store.
	player->health = -1;
}

namespace {

// GameEvent_PlayerDeath (and Entity_CheckAndProcessDeath's non-player leg) for
// one record. `direct` is a caller that runs it on a slot's body outside any
// death edge (Server_RunPlayerDeath): no tally, no Flags store, the live +0x2C0.
// [orig: GameEvent_PlayerDeath @0x516DD0]
void player_death_transaction(NapiNPServerCtx &ctx, world::World &world,
		const world::RoundDeath &d, bool direct) {
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
	NapiNPConnection *victim_connection = nullptr;
	for (NapiNPConnection &c : ctx.np_protocol.connection_list) {
		if (!c.link.owned_entity.valid() ||
				c.link.owned_entity.packed != d.victim_handle)
			continue;
		victim_connection = &c;
		break;
	}
	// The player transaction's head: a mounted victim leaves its seat, every
	// pool-0 AI body's link to it goes, and a slot whose spectator latch is set
	// stops there: no 0x13, holds, scoring or carried-object drop.
	// [orig: GameEvent_PlayerDeath -- the +0x16C test and
	//  Entity_DetachFromVehicleIfServer @0x516DE7..0x516DFF, the pool-0 walk
	//  @0x516E07..0x516E45, Entity_ValidatePtr @0x516E48, the latch
	//  `cmp [edi+188D7h], bl` @0x516E5A, the return @0x516E60]
	if (victim_is_player) {
		if (victim_entity->mounted) world.vehicles.detach(d.victim);
		if (victim_connection != nullptr && victim_connection->link.spectator) {
			world.match.unlink_point_share_links(world, d.victim);
			return;
		}
	}
	// An edge record's victim is a person even when its corpse leg removed the row.
	const bool organic_victim = d.motor_edge || (victim_entity != nullptr &&
			victim_entity->kind == world::EntityKind::Organic);
	// The revive-window and resend gates read the victim's live entity+44
	// cause word BEFORE the classifier clears the bit it reports.
	// [orig: GameEvent_PlayerDeath @0x516f4d precedes the ladder @0x517180]
	const uint32_t victim_cause_bits =
			victim_entity != nullptr ? victim_entity->cause_flags : 0u;
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
	// An edge record's row carries the edge's own latch (or, an org1 row's, a
	// respawned life); a direct caller's transaction stores no Flags (a caller
	// that latches does it after) [orig: GameEvent_PlayerDeath @0x516DD0 has no
	// store to +0x24; the team change's @0x519061; KillPlayer's none @0x4D29EC].
	if (organic_victim && !d.motor_edge && !direct) {
		victim_entity->flags |= 2u;
		victim_entity->alive = false;
	}
	// The dead/protection latch is the player leg's alone.
	// [orig: Entity_CheckAndProcessDeath tests Flags & 0x100
	// @0x51B555..0x51B55D before GameEvent_PlayerDeath]
	if (victim_is_player) victim_entity->damage_state = -1;
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
		// 0), as does a person's record standing in for its edge. A direct
		// caller (the team change, the admin kills) ships the slot as it
		// stands, a staged selection included, and every other kind ships
		// the slot as stored. The retail receiver stores the word
		// sign-extended into +0x2C0.
		// [orig: NetPacket_BuildDeathNotifyPayload @0x5036E0 (movzx word [esi+2C0h]
		//  @0x503733, store @0x50374A); edges @0x4B9D38 -> @0x4B9D4D (AI)
		//  and @0x4B4CD5 -> @0x4B4CEA (player body); direct senders
		//  @0x4D29EC (KillPlayer), @0x519059 (the team change), @0x404254
		//  (the admin console); receiver NapiNPClientMsg_EntityDeath
		//  @0x42EB8D/@0x42EBDF]
		const bool edge_consumes_slot = !direct && victim_entity != nullptr &&
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
			send_entity_update_message(ctx, c, s2c::ENTITY_DEATH, body13);
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
			send_entity_update_message(ctx, *victim_connection, s2c::TICK_SEED,
					Server_DisarmPlayerTickSeed(*victim_connection, world.out.rounds.frame_clock));
		}

		// Then target the victim with the fixed-point position used by the
		// third-person death camera: killer position when one resolves, else
		// the victim position. Mask 0x20 includes the listen host.
		// [orig: GameEvent_PlayerDeath @0x516DD0 ->
		// NetPacket_WriteThreeInt32s @0x506CB0]
		if (victim_connection != nullptr &&
				is_in_match(*victim_connection) &&
				victim_connection->link.transport != nullptr) {
			const world::Entity *camera_entity = killer_entity;
			if (camera_entity == nullptr) camera_entity = victim_entity;
			DeathCameraTarget target;
			if (camera_entity != nullptr) {
				target.x = camera_entity->position.x.q16;
				target.y = camera_entity->position.y.q16;
				target.z = camera_entity->position.z.q16;
			}
			send_entity_update_message(ctx, *victim_connection, s2c::DEATH_CAMERA_TARGET,
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
				send_entity_update_message(ctx, c, s2c::GAME_EVENT, body1e);
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
				send_entity_update_message(ctx, recipient, s2c::PLAYER_DOWNED_STATE,
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

	if (!d.motor_edge && !direct) world.match.process_kill_event(world, d);

	// The killer-less arm names the victim its own last attacker, which a
	// later medic test and a later edge's transaction read.
	// [orig: GameEvent_PlayerDeath -- the +0x178 test @0x516FF4..0x516FFC, the
	//  store `mov [esi+178h], esi` @0x5172E6]
	if (victim_connection != nullptr && victim_is_player && killer_entity == nullptr)
		victim_entity->last_attacker = d.victim;

	if (victim_connection != nullptr) {
		// Every player slot gets the same two whole-second counters. A
		// configured timeout below three is floored; a death less than 620
		// server frames after the slot's last seed arm (its join, deploy or
		// revive) forces exactly three: the frame clock less the arm's stamp,
		// a signed compare, with no validity flag (a fresh row's stamp is 0).
		// +364 retains the greater value only when a spawn-target registry exists.
		// [orig: GameEvent_PlayerDeath @0x516E9F..0x516EB4 (`mov eax,
		//  g_ServerFrameClock` @0x516E9F, `sub eax, [edi+178C8h]` @0x516EA4, `cmp eax,
		//  26Ch; jl` @0x516EAF..0x516EB4), the stores @0x516EBE..0x516EEB]
		replication::Connection &link = victim_connection->link;
		uint32_t hold = std::max(ctx.config.respawn_timeout, 3u);
		if (static_cast<int32_t>(world.out.rounds.frame_clock -
				victim_connection->seed_armed_clock) < 620)
			hold = 3;
		link.respawn_delay_seconds = hold;
		link.spawn_target_hold_seconds = world.zones.has_spawn_zone()
				? std::max(link.spawn_target_hold_seconds, hold)
				: 0u;
	}
}

} // namespace

// See header. [orig: GameEvent_PlayerDeath @0x516DD0]
void Server_ProcessPlayerDeath(NapiNPServerCtx &ctx, world::World &world,
		const world::RoundDeath &d) {
	player_death_transaction(ctx, world, d, false);
}

// See header.
void Server_RunPlayerDeath(NapiNPServerCtx &ctx, world::World &world,
		world::EntityHandle victim) {
	const world::Entity *entity = world.registry.get(victim);
	if (entity == nullptr) return;
	world::RoundDeath death;
	death.victim = victim;
	death.victim_handle = victim.packed;
	death.killer = entity->last_attacker;
	death.killer_handle = entity->last_attacker.valid() ? entity->last_attacker.packed : 0xFFFFu;
	death.event_flags = entity->cause_flags & 0xF00u;
	player_death_transaction(ctx, world, death, true);
}

// See napi_np_server_ctx.h. The player body's edge has latched the dead bit
// and, unless a non-lethal hit staged the death selection (+0x2C0), cleared
// +0x178, which the transaction credits as the edge leaves it. Its staging
// scope's record keeps the transaction's place among the entity update's other
// deaths for its sends.
// [orig: Entity_UpdateInfantryPlayerBody -- the +0x2C0 test and +0x178 clear
//  @0x4B4C72..0x4B4C8D, the latch @0x4B4CB5..0x4B4CDB, `cmp is_authority`
//  @0x4B4CE1, Entity_CheckAndProcessDeath @0x4B4CEA; GameEvent_PlayerDeath
//  reads +0x178 @0x516F6B, the scoring @0x516FB0]
void ServerEntityUpdateTransactions::player_death(world::World &world, world::Entity &body) {
	if (ctx == nullptr) return;
	world::RoundDeath death;
	death.victim = body.handle;
	death.victim_handle = body.handle.packed;
	death.killer = body.last_attacker;
	death.killer_handle = body.last_attacker.valid() ? body.last_attacker.packed : 0xFFFFu;
	death.event_flags = body.cause_flags & 0xF00u;
	// The classifier's special-ammo test reads the victim's +0x17C round def,
	// which ours carries on the lethal hit's damage-time record, queued earlier
	// in this entity update (the projectiles and the explosion queue run ahead
	// of the pool-0 walk) [orig: GameEvent_PlayerDeath @0x517223; the hit's
	// store Projectile_ProcessDamageOnTarget @0x4E81F9].
	for (auto it = world.round_sim.deaths.rbegin(); it != world.round_sim.deaths.rend(); ++it) {
		if (it->victim != body.handle || it->staged_sends != 0) continue;
		death.ammo_index = it->ammo_index;
		break;
	}
	death.motor_edge = true;
	EntityUpdateStaging staging(*ctx, world, body.handle);
	Server_ProcessPlayerDeath(*ctx, world, death);
}

} // namespace opennova::inmatch
