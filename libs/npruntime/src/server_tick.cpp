#include "npruntime/server_tick.h"

#include <cmath>
#include <cstdint>
#include <vector>

#include <npwire/ingame_message_id.h>
#include <netsim/entity_wire_bridge.h> // snapshot_world / GameEntitySnapshot
#include <netsim/connection_fan.h>     // drain_connection_c2s / emit_connection_s2c
#include <world/ai.h>                  // AiEntity / AiSystem::for_handle (the motor store)
#include <world/angle.h>               // bam_heading_from_mission_yaw_deg
#include <world/entity_spawn.h>        // entity_reset_to_spawn_state (respawn release)
#include <world/geom.h>                // to_fixed
#include <world/infantry.h>            // infantry_respawn_snap (the motor half of a respawn)
#include <world/vehicle_motor.h>       // VehicleTraits (the 0x40 vehicle-blip icons)
#include <world/world.h>               // World::run_logic_tick
#include <world/zone_capture.h>        // the 1 Hz AS capture pass (slice 2)

#include <algorithm>

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

// Route the deaths the damage pass raised this tick [orig: health<=0 detection in the
// entity update -> Entity_CheckAndProcessDeath @0x51b550 -> GameEvent_PlayerDeath
// @0x516dd0 (players, Flags & 0x100) / the 0x13-only AI leg @0x51b573]. Emits, per
// death: the S2C 0x13 death notify `[u16 victim][u16 killerSource]`
// [orig: BuildDeathNotifyPayload @0x5036e0, mask 0x90 = alive+not-host] and — for
// player victims — the S2C 0x1E kill-feed event (§5.26 8-B body; standard-kill
// event_type 4: the original picks rand(0-2)+4 @0x517237, a presentation-only variant;
// zone-flag types (headshot 32 / vehicle 10 / knife 13) wait on the bone-hit port).
// Deferred (§5.60): 0x52 kill stats, 0x54 death/wounded markers, 0x32 name broadcast,
// scoring. A dead HOST player (the loopback's own entity) is queued for the respawn
// release; a joiner's respawn rides its own deploy request instead.
void route_round_deaths(NapiNPServerCtx &ctx, world::World &world) {
	if (world.round_sim.deaths.empty()) return;
	for (const world::RoundDeath &d : world.round_sim.deaths) {
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
// actions [orig: @0x51ad6f]. The MP legs (zone-ownership sweep, per-game-type
// score/time/kill limits over the team stat blocks) are unported — net track.
void check_win_conditions(NapiNPServerCtx &ctx, world::World &world) {
	(void)ctx;
	if (world.round_end.ended) return;
	// MP legs unported; mp_session (not ctx.is_in_session — always 1 on our
	// listen server) is the retail SP discriminator.
	if (world.mp_session) return;
	const world::Entity *local = world.registry.get(world.cached.local_player);
	if (local == nullptr) return;
	const bool dead = !local->alive || (local->flags & 2u) != 0;
	if (dead && (world.mission_attrib_flags & world::World::kMissionAttribSinglePlayerRespawn) == 0)
		world.process_round_end(2);
}

// Release due respawns: back to the spawn point at full health [orig:
// Server_ProcessPlayerDeath -> Entity_ResetToSpawnState @0x4B9610; the D-NET-66
// death/respawn teleport — a snap, never motion].
void release_due_respawns(NapiNPServerCtx &ctx, world::World &world) {
	// Respawns are gate-blocked once the round has ended — the queue simply holds
	// [orig: the respawn request path checks g_spawn_success_gate,
	// Server_ProcessClientRequestRespawn @0x519af6].
	if (world.round_end.ended) return;
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

} // namespace

void Server_TickUpdate(NapiNPServerCtx &ctx, const PlayerReplicationState &fallback_anchor) {
	// A joiner is a pure non-authority client (its frame is P5's Client_ProcessNetworkFrame); the
	// pre-World P2 unit-test path has no simulation to drive. Either way: no host frame. The host
	// tick runs under is_authority [orig: Game_ProcessMainFrame @0x5263f0 gates the call
	// `if (is_authority && !suspended) Server_TickUpdate(...)` @0x5266b4]. is_in_session (+0x58) is
	// NOT the gate here — it gates the per-frame replicate/broadcast at step (3) [D-NET-120]; the
	// C2S drain + sim tick run regardless (the orig recv/send pumps are not is_in_session-gated).
	if (ctx.world == nullptr || ctx.is_authority == 0) return;
	world::World &world = *ctx.world;

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

	// (2b) Death routing + respawn release — the deaths the round sim raised inside the
	// tick get their broadcasts staged before this frame's 0x0A fan (§5.60; the 0x0A
	// health byte carries the same-frame damage regardless).
	route_round_deaths(ctx, world);
	release_due_respawns(ctx, world);

	// (2c) Win conditions at 1 Hz [orig: the g_periodic_second_timer block in
	// Server_TickUpdate — reload 62 @0x51db93 — calls Server_CheckWinConditions
	// @0x51df5a once per second].
	if (world.logic_tick % 62u == 0) check_win_conditions(ctx, world);

	// (2d) The AS capture loop at 1 Hz — slice 2 of the §5.61 witness [orig: the
	// Server_TickUpdate g_periodic_second_timer block @0x51DF50..0x51DF8C: proximity ->
	// Server_UpdateCaptureZoneEntities (0x6F + 0x1E 0x3B/0x3C) -> Server_EnforceZoneEntityTeams
	// -> Server_UpdateCaptureZones (instant numbered flips + 0x53 + GameEvent_FlagCapture)].
	// The world side runs in zone_capture_tick; this block encodes its events:
	//   0x6F 15 B [u16 handle][u8 team][i32 control][i32 0x10000][i16 delta][u8 f][u8 e]
	//     [orig: NetPacket_WriteZoneTimerValue @0x506E70] — CHANGE-GATED to all in-match
	//     conns + the full set at 1 Hz to deploy-pending/dead ones (the golden carries
	//     0x6F in deploy-window bursts, not a steady per-second stream; D-NET-162);
	//   0x1E 8 B zone events [orig: GameEvent_BuildPayload @0x5054E0]: 0x3B/0x3C secure
	//     edges (attacker = zone-list index — ours is the chain index, a tracked
	//     divergence; victim = zone team); flips 50/51 (frontier held) or 52/53 (victim =
	//     the recipient side's NEW frontier), team-filtered; then the 56/57 banner to all
	//     [orig: GameEvent_FlagCapture @0x50F6F0];
	//   0x53 9 B on flips [u16 handle][u8 curTeam][u8 capTeam][u16 progress=0][u16 limit=0]
	//     [u8 rate=0] [orig: NetPacket_WriteZoneTimerWindow @0x506D00, the drain legs
	//     @0x53BA36/0x53BA68];
	//   0x40 minimap-overlay state per conn at 1 Hz [orig: Server_BuildOverlayStateForPlayer
	//     @0x517FC0 -> Entity_ClassifyForMinimap @0x50FA70 -> the 16-entry flush
	//     @0x50FE20]: persistent zone entries (icon 0, flags 0x10) + transient vehicle
	//     blips (icon by unit_type, flags 0x00); player/emplacement/CTF entries deferred
	//     (D-NET-162).
	if (ctx.is_in_session && world.logic_tick % 62u == 0 && !world.zone_chain.empty()) {
		static world::ZoneCaptureEvents ev; // scratch (single-threaded host tick)
		world::zone_capture_tick(world, world.zone_chain, ev);

		auto zone_team_color = [](uint8_t team) -> uint8_t {
			// [orig: Entity_ClassifyForMinimap @0x50FA70 — team 1 -> 0x0a (blue),
			// team 2 -> 0x09 (red), else 0x0c (neutral/green); §5.19 color table]
			if (team == 1) return 0x0a;
			return team == 2 ? 0x09 : 0x0c;
		};

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

		// Zone-list index approximation for the 0x1E attacker byte: the chain vector index
		// (retail uses SpawnZoneList_IndexOf @0x43B990 over the client-sorted registry —
		// tracked divergence, D-NET-162).
		auto chain_index_of = [&](world::EntityHandle h) -> uint8_t {
			for (size_t i = 0; i < world.zone_chain.zones.size(); ++i)
				if (world.zone_chain.zones[i] == h) return static_cast<uint8_t>(i);
			return 0xFF;
		};
		auto event_body = [](uint8_t ev_type, uint8_t attacker, uint8_t victim) {
			return std::vector<uint8_t>{ev_type, attacker, victim, 0xFF, 0, 0, 0, 0};
		};

		// 0x53 flip windows.
		std::vector<std::vector<uint8_t>> flip_53;
		for (const auto &f : ev.flips) {
			std::vector<uint8_t> b;
			put_u16le(b, f.zone.packed);
			b.push_back(f.new_team);
			b.push_back(f.capturer_team);
			put_u16le(b, 0); // progress [orig: the drain restart writes 0]
			put_u16le(b, 0); // limit
			b.push_back(0);  // rate
			flip_53.push_back(std::move(b));
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
				if (changed || deploy_screen) conn.link.transport->host_send(s2c::ZONE_TIMER_VALUE, zb.second);
			}

			// 0x1E secure edges (to all in-match) [orig: @0x519839/@0x51988E].
			for (const auto &se : ev.secure_edges) {
				conn.link.transport->host_send(
						0x1E, event_body(se.secured ? 0x3B : 0x3C,
				                         chain_index_of(se.zone), se.zone_team));
			}

			// Flip events + 0x53 windows.
			for (size_t fi = 0; fi < ev.flips.size(); ++fi) {
				const auto &f = ev.flips[fi];
				conn.link.transport->host_send(s2c::ZONE_TIMER_WINDOW, flip_53[fi]);
				if (f.suppressed) continue; // match decided [orig: @0x4A2920 gate]
				const uint8_t zone_idx = chain_index_of(f.zone);
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

			// 0x40 minimap overlay: persistent zone entries + transient vehicle blips,
			// chunked 16 per datagram [orig: the 16-slot staging flush @0x50FE20].
			std::vector<uint8_t> entries;
			int count = 0;
			auto flush_40 = [&]() {
				if (count == 0) return;
				std::vector<uint8_t> body;
				body.push_back(static_cast<uint8_t>(count));
				body.insert(body.end(), entries.begin(), entries.end());
				conn.link.transport->host_send(s2c::CAPTURE_ZONE_STATE, body);
				entries.clear();
				count = 0;
			};
			auto push_40 = [&](uint16_t handle, uint8_t icon, uint8_t color, uint8_t flags) {
				put_u16le(entries, handle);
				entries.push_back(icon);
				entries.push_back(color);
				entries.push_back(flags);
				entries.push_back(0); // source byte [orig: entity weaponByte]
				if (++count == 16) flush_40();
			};
			for (const world::EntityHandle zh : world.zone_chain.zones) {
				if (const world::Entity *z = world.registry.get(zh))
					push_40(zh.packed, 0, zone_team_color(z->team), 0x10);
			}
			world.registry.for_each([&](const world::Entity &e) {
				if (e.handle.pool() != 1 || !e.alive || e.health <= 0) return;
				const world::VehicleTraits *vt = world.vehicle_traits.get(e.item_id);
				if (vt == nullptr) return; // vehicle-class blips only (D-NET-162)
				uint8_t icon = 10; // ground [orig: @0x50FA70 unitType switch]
				if (vt->unit_type >= 5 && vt->unit_type <= 8) icon = 15;
				else if (vt->unit_type == 3 || vt->unit_type == 4) icon = 11;
				else if (vt->unit_type == 12) icon = 25;
				push_40(e.handle.packed, icon, zone_team_color(e.team), 0x00);
			});
			flush_40();
		}
	}

	// (2c) Spawn-wave status: S2C 0x6E at 1 Hz to every PENDING or DEAD in-match player —
	// the deploy/death screen's team-roster + wave panel feed. With no host wave options
	// configured the body is the empty-group form (a single 0x00 group-count byte); wave
	// groups land with g_spawn_wave_list (§5.61 deferral). [orig: Server_TickUpdate
	// @0x51e089 emits NetPacket_WriteSpawnWaveStatus @0x507490 at 1 Hz; the recipient mask
	// includes the respawn-pending bit4 (slot+89912 & 0x10 @0x5074c2) and dead players;
	// golden ASH_I5A deploy window carries 0x6E ×11 at ~1 Hz]
	if (ctx.is_in_session && world.logic_tick % 62u == 0) {
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
				conn.link.transport->host_send(s2c::ROSTER_SYNC, kEmptyWaveStatus);
		}
	}

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
	// Server_BuildPlayerInfoAndAdd), NOT the D-NET-121 dvxi5 fallback_anchor.
	if (ctx.is_in_session) {
		const std::vector<GameEntitySnapshot> ents = netsim::snapshot_world(world);
		for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
			if (!is_in_match(conn)) continue;
			netsim::emit_connection_s2c(world, conn.link, ents, fallback_anchor,
			                            ctx.config.game_type);
		}
	}

	// (4) flush is implicit: host_send staged each 0x0A on its transport. The loopback's local client
	// reads it via client_recv / NetClientView::pump; a remote peer's transport outbound_ is popped +
	// framed into a 0x83 SESSION by the owner (frame_in_match_s2c). No socket I/O in libs/.
}

} // namespace opennova::np
