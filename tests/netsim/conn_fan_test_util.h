#pragma once

// Test-only scaffolding for the netsim per-connection drain/fan primitives (connection_fan.h).
// Production owns its connection table elsewhere (npruntime: NapiNPProtocol.connection_list, driven
// by Server_TickUpdate); these helpers give the netsim unit tests a plain std::vector<Connection>
// table to drive the SAME drain_connection_c2s / emit_connection_s2c functions over. This is NOT a
// parallel runtime model — it is the harness the retired NetSystem test convenience used to provide.

#include <cstddef>
#include <vector>

#include <netsim/connection.h>
#include <netsim/connection_fan.h>     // drain_connection_c2s / emit_connection_s2c
#include <netsim/entity_wire_bridge.h> // snapshot_world
#include <world/player_spawn.h>        // spawn_remote_player
#include <world/world.h>

namespace opennova::netsim::test {

// The authority host's top-of-tick C2S drain over a connection table (a joiner, is_authority == 0,
// never drains — the original gates on g_napi_np_ctx.is_authority).
inline void drain_all(world::World &world, const std::vector<Connection> &conns, bool is_authority) {
	if (!is_authority) return;
	for (const Connection &c : conns) drain_connection_c2s(world, c);
}

// Build the world snapshot ONCE, then fan a per-connection-anchored 0x0A to every connection
// [orig: NapiNPServer_SendFiltered @0x4C87E0 builds once, SendToConn @0x4c4f20 per node]. `conns` is
// non-const: each emit advances that connection's 0x0A sub-block phase counter.
inline void emit_all(const world::World &w, std::vector<Connection> &conns,
                     const PlayerReplicationState &fallback_anchor,
                     uint32_t game_type = 0) {
	const std::vector<GameEntitySnapshot> ents = snapshot_world(w);
	for (Connection &c : conns)
		emit_connection_s2c(w, c, ents, fallback_anchor, game_type);
}

// Spawn a joiner's owned pool-0 player (a REMOTE peer — spawn_remote_player leaves it non-local) and
// bind it to conns[idx] [orig: Server_PlayerAdd @0x51cbc0 registers a joined player without assigning
// g_local_player_entity]. Returns the spawned handle (invalid if idx is out of range or pool 0 full).
inline world::EntityHandle admit_peer(world::World &w, std::vector<Connection> &conns,
                                      std::size_t idx, const world::PlayerSpawn &spawn) {
	if (idx >= conns.size()) return world::EntityHandle{};
	const world::EntityHandle h = world::spawn_remote_player(w, spawn);
	if (h.valid()) conns[idx].owned_entity = h;
	return h;
}

} // namespace opennova::netsim::test
