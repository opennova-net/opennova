#pragma once

// The in-match HOST owner loop: exactly ONE implementation, which apps/nw_server and
// godot/src/simulation both delegate to. The faithful reimpl of the original engine's per-frame host
// pump [orig: CNapiNetwork_PumpManagerReceive @0x4c4d10 (recv, mgr flag 4, 250ms) +
// CNapiNetwork_SendUDPPacket @0x4c4d30 (CNapiNPManager_SendTo) wrapped around Server_TickUpdate].
//
// Socket-free: the loop holds NO socket — the owner supplies a opennova::IDatagramSocket and this
// pumps it (apps/nw_server wraps net::Socket via apps/common/net_datagram_socket.h; godot/src
// wraps UdpPump). ALL protocol/crypto/framing stay in the libs (.agents/network.md). The loop
// speaks PeerAddr; the recv timeout (0 = non-blocking busy loop; ~30 ms for a single-threaded
// poll-pump test) is a property of the embedder, not this loop.

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <runtime/replication/connection.h>            // replication::TransportMode
#include <net/npwire/idatagram_socket.h>      // opennova::IDatagramSocket
#include <runtime/inmatch/session_transport.h>     // replication::ISessionTransport / Datagram
#include <runtime/inmatch/udp_session_transport.h> // replication::UdpSessionTransport

#include <net/npwire/peer_addr.h> // opennova::PeerAddr
#include <net/npwire/protocol_message.h>

#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_protocol.h>  // HostAcceptEvent + the server protocol entry points
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_tick.h> // Server_TickUpdate

namespace opennova::inmatch {

// Per-remote-peer owner state: the socket-free transport the owner pumps (owns its lifetime; the
// connection's link.transport is a non-owning pointer into it) + the once-latch for the joiner's
// named dcb-bearing organic-spawn announce.
struct PeerLink {
	std::unique_ptr<replication::UdpSessionTransport> transport;
	bool announced = false;
};

// std::map ordering for PeerAddr (which has operator== but no operator<). Lexicographic (ip, port).
struct PeerAddrLess {
	bool operator()(const PeerAddr &a, const PeerAddr &b) const {
		return a.ip != b.ip ? a.ip < b.ip : a.port < b.port;
	}
};

// The in-match host: the inmatch ctx + the per-peer transports + the host's own loopback drain.
// The ctx.world / ctx.mission are wired by the owner (main.cpp / the Godot binding / the test)
// before the first pump.
struct HostOwner {
	NapiNPServerCtx ctx;
	replication::ISessionTransport *host_loopback = nullptr; // non-owning; the host's own dcb-2 client (Listen)
	std::map<PeerAddr, PeerLink, PeerAddrLess> peers;
	// In-match semantic replies collected while the S2C send block is closed.
	// They retain message order and are framed with transport output on the next
	// configured boundary.
	std::map<PeerAddr, std::vector<ProtocolMessage>, PeerAddrLess>
			pending_session_messages;
	// Already-framed established-session packets (0x83 initial stream,
	// retransmits, and 0x84 missing-sequence requests) also wait on the peer's
	// send boundary. Their sequence numbers and ciphertext are already fixed.
	std::map<PeerAddr, std::vector<std::vector<uint8_t>>, PeerAddrLess>
			pending_session_datagrams;
	uint32_t now_tick = 0;
	// false (dedicated/headless): HostOnly registers no local-player connection; an embedder-supplied
	// unused channel may be defensively drained. true (serve-and-play): HostClient registers the
	// loopback and the owner folds it into ClientState, so the pump must preserve it.
	bool serve_and_play = false;
};

// Attach a UdpSessionTransport to `peer`'s connection (idempotent) and, ONCE the spawn pipeline has
// bound owned_entity, latch the peer announced exactly once. The joiner's NAMED dcb-bearing S2C 0x0C
// organic-spawn (ownerConnectionId == ServerAuth.MI, its wire handle H; D.0 / §5.23; the F3
// dcb-timing contract) ships IN-PHASE with the initial-state world stream, not from here.
void admit_peer(HostOwner &owner, const PeerAddr &peer);

// React to one HostAcceptEvent surfaced by handle_server_datagram / tick_connections.
void dispatch_event(HostOwner &owner, const PeerAddr &peer, const HostAcceptEvent &ev);

// One full owner iteration over `sock` (the §5.44 recv-before-send order):
//   (1) recv-drain: recv_from -> handle_server_datagram -> ship replies + react to events
//   (2) tick_connections: pre-spawn §5.2a bursts (framed 0x83 for remote peers) + surface F3/spawned
//   (3) Server_TickUpdate: the single C2S drain + one logic tick + per-connection 0x0A fan
//   (4) S2C flush: on each peer's send boundary, ship retained 0x83/0x84 packets,
//       then pop its transport's identity [tag][body] -> frame/batch as 0x83 -> send
//   (5) drain an unused embedder loopback for HostOnly; preserve the HostClient local view
// `before_server_tick`, when supplied, runs after tick_connections has completed any
// player spawns and before their first authoritative logic update / 0x0A fan. The opaque
// context keeps this shared owner loop independent of embedder-specific registration state.
// Advances owner.now_tick by 1.
using HostBeforeServerTickFn = void (*)(void *context);
using HostEventObserverFn = void (*)(void *context, const HostAcceptEvent &event);

// The phases lap onto the SIM_HOST_* / SIM_SERVER_TICK rows of the world's
// profile (ADR 0043 d5); an inactive profile reads no clock.
void host_session_pump(HostOwner &owner, opennova::IDatagramSocket &sock,
		HostBeforeServerTickFn before_server_tick = nullptr,
		void *before_server_tick_context = nullptr,
		HostEventObserverFn event_observer = nullptr,
		void *event_observer_context = nullptr);

// The pump's step (4) alone, with every boundary treated as open: the
// teardown's final flush, so the round-reset 0x25 and the "NP.C:SH:STOP"
// description staged by the host's close reach every remote before the
// sockets go away [orig: the StopServer walk NapiNPProtocol_StopServer
// @0x62A820 stamps and destroys each connection in turn].
void host_session_flush_s2c(HostOwner &owner, opennova::IDatagramSocket &sock);

// Host bring-up config. The owner sets owner.ctx.world / owner.ctx.mission and owner.host_loopback (its
// dcb-2 LoopbackChannel) BEFORE start_host_session; this fills the rest.
struct HostConfig {
	// The ONE consolidated GameConfig (ADR 0013): §6.4 identity (server name / max players / passwords /
	// game type) + the §6.9 rule globals + the §5.1 reactive-reply config (mission / player / spawn).
	GameConfig config;
	SocketMode socket_mode = SocketMode::Lan; // Lan (real socket) / Socketless (SP in-process loopback)
	// Nonzero values are deterministic overrides for golden/tests. Zero asks the production helper
	// to mint the volatile retail startup value.
	uint32_t host_key = 0;
	uint32_t host_start_tick = 0;
	uint32_t session_seed_id = 0;
	bool serve_and_play = false; // true: HostClient + local player; false: HostOnly, no local player
	// The host's game directory — where CNapiNetwork_Init looks for the loose
	// `_NSTMOUT.TXT` reap/pool override (session_timeout_config.h). Empty = no
	// override lookup, the 120000 ms / 1200-record template stands.
	std::string game_root;
	// The listen host's OWN per-side character selection (weapon.sav / PLAYER_INFO),
	// installed on its type-2 loopback connection before the local player add — the
	// same fields a joiner uploads in ClientAuth. Retail fills its local player
	// record from the profile the same way [orig: PlayerSession_InitFromProfile
	// @0x50ca80 validates + stores both side ids; Player_InitPlayer @0x4e15f0 <-
	// g_AvatarTeam1/2, g_CharClassTeam1/2 <- Game_ApplySessionSettingsToGlobals
	// @0x551500]. Defaults to the stock fresh-profile seed; the shell replaces it
	// from the mounted Avatars.def + weapon.sav.
	CharacterJoinVars local_character_vars = retail_fresh_profile_character_vars();
};

// Stand `owner` up through the shared in-match host bring-up used by apps/nw_server and the Godot
// host. `serve_and_play` selects HostClient and registers the type-2 loopback; otherwise it selects
// HostOnly and creates no local player. When serve-and-play also has a World, spawn the host player
// and queue its initial-state burst before the first per-frame 0x0A.
void start_host_session(HostOwner &owner, const HostConfig &cfg);

} // namespace opennova::inmatch
