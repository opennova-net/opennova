#include "npruntime/server_tick.h"

#include <cmath>
#include <cstdint>
#include <vector>

#include <netsim/entity_wire_bridge.h> // snapshot_world / GameEntitySnapshot
#include <netsim/connection_fan.h>     // drain_connection_c2s / emit_connection_s2c
#include <world/entity_spawn.h>        // entity_reset_to_spawn_state (respawn release)
#include <world/world.h>               // World::run_logic_tick

namespace opennova::np {

namespace {

// Pool-0 index byte for the S2C 0x1E kill-feed actor fields (§5.26: u8 pool-0 index,
// 0xFF = none).
uint8_t pool0_index_byte(uint16_t handle) {
	if (handle == 0xFFFF || (handle & 0xF000u) != 0) return 0xFF;
	const uint16_t slot = handle & 0xFFFu;
	return slot <= 0xFEu ? static_cast<uint8_t>(slot) : 0xFF;
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
				c.link.transport->host_send(0x13, body13);
				if (!body1e.empty()) c.link.transport->host_send(0x1E, body1e);
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

// Release due respawns: back to the spawn point at full health [orig:
// Server_ProcessPlayerDeath -> Entity_ResetToSpawnState @0x4B9610; the D-NET-66
// death/respawn teleport — a snap, never motion].
void release_due_respawns(NapiNPServerCtx &ctx, world::World &world) {
	for (auto it = ctx.respawn_queue.begin(); it != ctx.respawn_queue.end();) {
		if (world.logic_tick < it->due_tick) {
			++it;
			continue;
		}
		world::Entity *e = world.registry.get(it->victim);
		if (e != nullptr) {
			// Respawn placement = the recorded spawn point (the D-NET-66 death/respawn
			// TELEPORT — a snap, never motion); entity_reset_to_spawn_state then re-backs
			// it up and clears the movement gate [orig: the deploy flow places the
			// entity, then Entity_ResetToSpawnState @0x4B9610 records Position].
			e->position = e->spawn_position;
			world::entity_reset_to_spawn_state(*e);
			// Spawn health = the item template's healthMax [orig: Entity_InitFromItemDef
			// @0x49e550; the player_item_hp mirror, D-NET-144].
			if (world.player_item_hp > 0)
				e->health = world.player_item_hp;
			else if (e->health_max > 0)
				e->health = e->health_max;
			else
				e->health = 100;
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
				conn.link.transport->host_send(0x6E, kEmptyWaveStatus);
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
			netsim::emit_connection_s2c(world, conn.link, ents, fallback_anchor);
		}
	}

	// (4) flush is implicit: host_send staged each 0x0A on its transport. The loopback's local client
	// reads it via client_recv / NetClientView::pump; a remote peer's transport outbound_ is popped +
	// framed into a 0x83 SESSION by the owner (frame_in_match_s2c). No socket I/O in libs/.
}

} // namespace opennova::np
