#pragma once

#include <cstdint>
#include <string>

#include <netsim/connection.h>             // netsim::Connection, netsim::TransportMode
#include <novaworld/connection/registry.h> // opennova::PeerAddr (the transport-addr key)

namespace opennova::np {

// dcb (ConnectionId) reservation on a LAN listen server. The host's loopback client consumes
// ConnectionId 0/1; the host's own PLAYER is dcb 2; remote joiners are assigned from 3 up. A
// 0x14B9 organic with entity+0x78 == 0 is the dedicated-server reservation, which makes a joining
// client keep no player entity for that slot and flood C2S 0x0F → 0xC9 disconnect — so the host
// player MUST carry a non-zero dcb. Wire-proven by the golden host_and_join_lan.pcapng (host player
// eFlags=2, first joiner 3, AI 0). [orig: NapiNP_GetLocalConnectionId @0x4c6d40; Server_PlayerAdd
// @0x51cbc0 writes entity+0x78 = conn->connection_id; net-re §5.2a/§5.2b]
inline constexpr uint32_t kHostPlayerDcb = 2;
inline constexpr uint32_t kFirstJoinerDcb = kHostPlayerDcb + 1;

// The per-connection lifecycle phase a server-side node walks from a fresh datagram to an
// in-match player (§5.0 / §5.2a). Names mirror the witnessed original flow:
//
//   New -> ValidateJoin [CNapiNetwork_ValidateJoinRequest @0x4c61b0]
//       -> HelloReceived 0x41 [NapiNPProtocol_HandleClientHello @0x6213b0] -> emit 0x81
//       -> Joined        0x42 [NapiNPProtocol_HandleClientJoin  @0x62b750] -> emit 0x82 (SCRK)
//       -> NewConnection      [NapiNPServer_HandleNewConnection  @0x4c8040]
//       -> InitRound          [Server_InitNewRoundState          @0x51c8e0]
//       -> PendingSpawn       [CNapiServer_ProcessPendingPlayerSpawns @0x4c8dc0]
//       -> PlayerAdded        [Server_BuildPlayerInfoAndAdd @0x51d560 -> Server_PlayerAdd @0x51cbc0]
//       -> SendingInitial     [Server_SendInitialGameStateToPlayer @0x51bba0]
//       -> Spawned            (NetSystem::admit_peer binds owned_entity)
//       -> InMatch            (per-frame 0x0A out / 0x0C in)
//       -> Goodbye       0x46 [Nwu_HandleClientGoodbye @0x624250]
//
// The SCRK/seq/ack state currently in HostSessionAccept::PeerState folds onto this node in P2.
enum class ConnectionPhase : uint8_t {
	New = 0,
	ValidateJoin,
	HelloReceived,
	Joined,
	NewConnection,
	InitRound,
	PendingSpawn,
	PlayerAdded,
	SendingInitial,
	Spawned,
	InMatch,
	Goodbye,
};

// The §5.2a initial-state burst cursor for one connection — Server_SendInitialGameStateToPlayer's
// per-player phase counters, folded onto the connection node like P2 folded PeerState. After P3 this
// is the SINGLE authority for the connection's spawn-gate progress (the F3 / PeerSpawned latches read
// entity_batch_count / spawned here, no longer the game_runtime GameSessionState).
// [orig: the playerSlot+0x20 sync-state + playerSlot+89878/89882/89884 phase counters; net-re §5.2a]
struct InitialStateBurst {
	uint8_t  sync_state = 0;            // [playerSlot+0x20] 0 idle / 2 player-sync / 4 world-stream / 5 done
	uint16_t player_sync_subphase = 8;  // [playerSlot+89878] 8..20 (one tag each; see server_initial_state)
	uint8_t  world_stream_phase = 0;    // [playerSlot+89882] 0 not-started, 1=0x10 .. 7=0x1A
	uint16_t phase_loop_counter = 0;    // [playerSlot+89884] per-phase record cursor (reserved; paging)
	uint8_t  game_state = 0;            // [CNetPlayer_SetGameState] 8 player-added / 9 in-game
	uint32_t entity_batch_count = 0;    // world-stream batches emitted — the F3 readiness signal
	bool     spawned = false;           // set with game_state==9 (the PeerSpawned source)
};

// One node on the host's connection_list — the reimpl of a NapiNPConnection [orig: the list
// NapiNPServer_SendFiltered @0x4C87E0 walks, one SendToConn @0x4c4f20 per node; struct reached via
// NapiNPProtocol.connection_list @+0xEBC, §6.5]. Named fields + cited offsets, idiomatic types
// (the in-memory layout is NOT byte-exact — wire bytes are; see the plan's fidelity decision).
//
// This unifies the two per-peer representations the old code split apart: HostSessionAccept's
// PeerState (handshake/SCRK/seq) and netsim::Connection (transport + owned_entity + send_mask).
struct NapiNPConnection {
	// [orig +0x18] ConnectionId / dcb — the join-order id a player record is matched by [orig:
	// NapiNP_GetLocalConnectionId @0x4c6d40; Player_FindLocalPlayerEntity @0x4e0090 numeric match].
	uint32_t connection_id = 0;

	// [orig: NapiNPConnection_Create @0x62ACB0 direction mirroring, §6.5] 1 = server-side
	// connection (the host's view of a client), 2 = client-side connection (a client's view of
	// the host, incl. the host's own local loopback client).
	uint8_t type = 1;

	// Where this node is in its lifecycle (above).
	ConnectionPhase phase = ConnectionPhase::New;

	// The byte transport + the pool-0 entity this connection drives + the per-connection send
	// filter, modeled by netsim::Connection (TransportMode, owned_entity, send_mask). The
	// transport is NON-OWNING: the binding/test owns the LoopbackChannel / UdpSessionTransport.
	netsim::Connection link{};

	// --- per-connection handshake state (P2: the old HostSessionAccept::PeerState, folded on) ---
	// SCRK / seq / ack + the session-flow latches, witnessed as fields the original keeps on the
	// NapiNPConnection node. Wire bytes stay byte-exact; this in-memory layout is not padded.

	PeerAddr peer{};               // the transport-addr key (distinct from connection_id; the
	                               // host scans connection_list by this to route a datagram).

	std::string pn;                // ClientHello.pn — drives classify_session_protocol
	std::string player_name;       // ClientHello.co — echoed into the organic-spawn 0x0C (D.0)
	std::string client_scrk;       // ClientAuth.scrk — decrypts inbound 0x43
	std::string server_scrk;       // our SCRK — encrypts outbound 0x83, echoed in ServerAuth
	uint32_t client_ck = 0;        // ClientAuth.ck -> session_id on our S2C
	uint32_t client_ci = 0;        // ClientAuth.ci — with client_ck, the retransmit-match key the join
	                               // leg compares a repeat 0x42 against [orig: session_keys.client_id
	                               // == CI && session_keys.remote_key == CK @ HandleClientJoin 0x62b750]
	uint32_t server_sk = 0;        // our ServerAuth.SK
	uint32_t next_outbound_seq = 1;
	uint32_t last_inbound_seq = 0;
	std::string session_id;        // key into the GameServerRuntime sessions_ (the peer label)

	// self_id (the joiner's own ConnectionId / dcb / unk_18, learned from its in-match 0x48
	// client-ack) UNIFIES onto connection_id above (+0x18) — the value the client's
	// Player_FindLocalPlayerEntity @0x4e0090 numeric-matches. Written only once self_id_seen.
	bool self_id_seen = false;             // true once the 0x48 client-ack has been parsed
	bool world_stream_announced = false;   // F3 edge-latch (PeerEnteredWorldStreaming fires once)
	bool spawned_announced = false;        // edge-latch (PeerSpawned fires once)

	// --- P3: the World-driven initial-state burst cursor (Server_SendInitialGameStateToPlayer) ---
	InitialStateBurst burst{};
};

// [D-NET-122] The single in-match predicate shared by Server_TickUpdate's C2S drain AND its S2C 0x0A
// emit fan (it was an inline `conn.burst.spawned` in each). In-match = the §5.2a initial-state burst
// has completed (game_state 9 -> burst.spawned). BOTH a remote joiner AND the host's OWN type-2
// loopback latch this the SAME way: tick_connections drives every connection's §5.2a burst
// (including the host loopback, D-NET-114) until completion, THEN this flips true. So the host's own
// loopback is no longer starved of its per-frame 0x0A once Server_TickUpdate is the SP/host driver.
// Its 0x0A anchors correctly (not the D-NET-121 dvxi5 fallback) because Server_BuildPlayerInfoAndAdd
// binds conn.link.owned_entity to the host player when it spawns. [orig: the per-frame replicate fan
// is is_in_session-gated inside Server_TickUpdate; the per-connection in-match selector is the burst
// completion]
inline bool is_in_match(const NapiNPConnection &conn) { return conn.burst.spawned; }

} // namespace opennova::np
