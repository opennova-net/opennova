#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <novaworld/replication_min.h> // PlayerReplicationState, GameEntitySnapshot
#include <world/player_spawn.h>         // PlayerSpawn (admit_peer)
#include <world/world.h>

#include "netsim/connection.h"
#include "netsim/entity_wire_bridge.h"
#include "netsim/session_transport.h"

namespace opennova::netsim {

// S2C in-match message tags carried on the loopback (the inner-message tag, not the
// session opcode). Phase 1 emits only the per-frame world reference.
inline constexpr uint8_t kTag0aFrameUpdate = 0x0A;

// The in-match net seam as a World ISystem (ADR 0009 Decision 1 / ADR 0011),
// registered AHEAD of WAC so it sits where the original's net step does. Faithful
// frame order [orig: Game_ProcessMainFrame @ 0x5263f0]:
//
//   input -> NetSystem::tick (drain C2S) -> World::run_logic_tick (WAC/BMS/AI)
//         -> NetSystem::emit_s2c -> present
//
// tick() runs INSIDE the authoritative system loop (it only fires under is_authority,
// which the SP host always is). emit_s2c() is called by the host AFTER
// run_logic_tick — deliberately NOT an ISystem hook — so the outbound serialize
// happens post-logic, mirroring the original's net-before-logic / serialize-after
// order without tripping the run_logic_tick authority guard.
//
// NetSystem holds the host's CONNECTION TABLE (the reimpl of the original's per-connection
// fan): emit_s2c builds the world snapshot ONCE then sends a per-connection-anchored 0x0A to
// each connection [orig: NapiNPServer_SendFiltered @0x4C87E0 walks connection_list -> one
// SendToConn @0x4c4f20 per node]; tick drains every connection's C2S queue [orig:
// NapiNPConnection_ParseMessages @0x625BC0 / PumpRecvQueues]. The host's own client is a
// transport-mode-1 LoopbackChannel connection; a remote LAN peer is a UdpSessionTransport
// connection of the same shape. The single-arg ctor registers exactly one loopback connection
// so every SP call site stays byte-identical (one connection, default anchor = the local
// player via the passed anchor).
class NetSystem : public world::ISystem {
public:
	NetSystem() = default;
	// SP / single-connection: register one mode-1 loopback connection (no owned entity — it
	// rides the passed anchor, which compute_net_anchor builds from the local player).
	explicit NetSystem(ISessionTransport &channel) {
		connections_.push_back(Connection{&channel, TransportMode::Loopback, {}, 0});
	}

	const char *name() const override { return "net"; }
	void tick(world::World &world, const world::TickContext &ctx) override;

	// Serialize the live world into one S2C 0x0A frame PER connection. `fallback_anchor` is the
	// subject the frame is built around for any connection with no owned entity yet (the host's
	// own loopback connection, or a joiner still in the handshake); a connection WITH an owned
	// entity is anchored to that entity's live position so its compact deltas stay small around
	// its own player — the original's per-player anchoring.
	void emit_s2c(const world::World &w, const PlayerReplicationState &fallback_anchor);

	// --- the connection table (the host's HandleNewConnection / ParseMessages surface) ---
	std::size_t add_connection(const Connection &c);
	void clear_connections();
	std::size_t connection_count() const { return connections_.size(); }
	Connection &connection(std::size_t i) { return connections_[i]; }
	const Connection &connection(std::size_t i) const { return connections_[i]; }

	// Spawn a joiner's owned pool-0 player entity (a REMOTE peer — NOT the host's own player)
	// and bind it to connection `conn_index`. The reimpl of the host accepting a join and
	// registering its entity [orig: Server_BuildPlayerInfoAndAdd @0x51d560 -> Server_PlayerAdd
	// @0x51cbc0; §5.2a]. The real handshake (Increment C) calls this on reaching the Spawned
	// phase. Returns the spawned handle (invalid if the index is out of range or pool 0 is full).
	world::EntityHandle admit_peer(world::World &w, std::size_t conn_index,
	                               const world::PlayerSpawn &spawn);

private:
	std::vector<Connection> connections_;
};

// --- per-connection primitives, shared by NetSystem (the legacy nova_simulation table) and
// npruntime's Server_TickUpdate (which fans over NapiNPProtocol.connection_list instead) so the
// single owner decision (ADR 0011 / npruntime ROADMAP P4) keeps ONE drain/emit implementation. ---

// Drain + read-apply the queued C2S 0x0C player uplinks on one connection's transport (the SNAP)
// [orig: dispatch_entity_packet_callback @0x4D6A80 -> NetPacket_SerializePlayerState]. Only
// sub_op 0x0A (extended) this increment; 0x0B compact is deferred.
void drain_connection_c2s(world::World &world, ISessionTransport &transport);

// Serialize the live world into ONE S2C 0x0A frame for `conn`, anchored to its owned entity (or
// `fallback_anchor` when it has none), and host_send it onto that connection's transport. `ents`
// is the world snapshot built ONCE by the caller [orig: NapiNPServer_SendToConn @0x4c4f20 per node].
void emit_connection_s2c(const world::World &w, const Connection &conn,
                         const std::vector<GameEntitySnapshot> &ents,
                         const PlayerReplicationState &fallback_anchor);

} // namespace opennova::netsim
