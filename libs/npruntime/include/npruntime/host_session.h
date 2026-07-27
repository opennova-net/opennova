#pragma once

// The in-match HOST owner loop, promoted into libs/npruntime so there is exactly ONE
// implementation. It was copy-pasted between apps/nw_server/host_owner_loop.h and
// godot/engine/simulation/nova_simulation.cpp (host_pump/dispatch_event/admit_peer) and had begun
// to drift; both now delegate here. The faithful reimpl of the original engine's per-frame host
// pump [orig: CNapiNetwork_PumpManagerReceive @0x4c4d10 (recv, mgr flag 4, 250ms) +
// CNapiNetwork_SendUDPPacket @0x4c4d30 (CNapiNPManager_SendTo) wrapped around Server_TickUpdate].
//
// Socket-free: the loop holds NO socket — the owner supplies a netsim::IDatagramSocket and this
// pumps it (apps/nw_server wraps net::Socket via apps/common/net_datagram_socket.h; godot/engine
// wraps NovaUdpPump). ALL protocol/crypto/framing stay in the libs (.agents/network.md). The loop
// speaks PeerAddr; the recv timeout (0 = non-blocking busy loop; ~30 ms for a single-threaded
// poll-pump test) is a property of the adapter, not this loop.

#include <cstdint>
#include <map>
#include <memory>

#include <netsim/connection.h>            // netsim::TransportMode
#include <netsim/idatagram_socket.h>      // netsim::IDatagramSocket
#include <netsim/session_transport.h>     // netsim::ISessionTransport / Datagram
#include <netsim/udp_session_transport.h> // netsim::UdpSessionTransport

#include <npwire/peer_addr.h> // opennova::PeerAddr

#include "npruntime/napi_np_connection.h"
#include "npruntime/napi_np_protocol.h"  // HostAcceptEvent + the server protocol entry points
#include "npruntime/napi_np_server_ctx.h"

namespace opennova::np {

// Per-remote-peer owner state: the socket-free transport the owner pumps (owns its lifetime; the
// connection's link.transport is a non-owning pointer into it) + the once-latch for the joiner's
// named dcb-bearing organic-spawn announce.
struct PeerLink {
	std::unique_ptr<netsim::UdpSessionTransport> transport;
	bool announced = false;
};

// std::map ordering for PeerAddr (which has operator== but no operator<). Lexicographic (ip, port).
struct PeerAddrLess {
	bool operator()(const PeerAddr &a, const PeerAddr &b) const {
		return a.ip != b.ip ? a.ip < b.ip : a.port < b.port;
	}
};

// The in-match host: the npruntime ctx + the per-peer transports + the host's own loopback drain.
// The ctx.world / ctx.mission are wired by the owner (main.cpp / the Godot binding / the test)
// before the first pump.
struct HostOwner {
	NapiNPServerCtx ctx;
	netsim::ISessionTransport *host_loopback = nullptr; // non-owning; the host's own dcb-2 client (Listen)
	std::map<PeerAddr, PeerLink, PeerAddrLess> peers;
	uint32_t now_tick = 0;
	// false (dedicated/headless): HostOnly registers no local-player connection; an adapter-supplied
	// unused channel may be defensively drained. true (serve-and-play): HostClient registers the
	// loopback and the owner folds it into ClientState, so the pump must preserve it.
	bool serve_and_play = false;
};

// Attach a UdpSessionTransport to `peer`'s connection (idempotent) and, ONCE the spawn pipeline has
// bound owned_entity, stream the joiner's NAMED dcb-bearing S2C 0x0C organic-spawn so the joiner
// name-matches its own player (entity_name == its game ClientAuth.NA) and learns its wire handle H (the
// admitted entity's packed handle; D.0 / §5.23; the F3 dcb-timing contract). No-ops until the
// automatic spawn pipeline binds owned_entity, then announces exactly once.
void admit_peer(HostOwner &owner, netsim::IDatagramSocket &sock, const PeerAddr &peer,
                const HostAcceptEvent &ev);

// React to one HostAcceptEvent surfaced by handle_server_datagram / tick_connections.
void dispatch_event(HostOwner &owner, netsim::IDatagramSocket &sock, const PeerAddr &peer,
                    const HostAcceptEvent &ev);

// One full owner iteration over `sock` (the §5.44 recv-before-send order):
//   (1) recv-drain: recv_from -> handle_server_datagram -> ship replies + react to events
//   (2) tick_connections: pre-spawn §5.2a bursts (framed 0x83 for remote peers) + surface F3/spawned
//   (3) Server_TickUpdate: the single C2S drain + one logic tick + per-connection 0x0A fan
//   (4) S2C flush: pop each remote transport's identity [tag][body] -> frame_in_match_s2c (0x83) -> send
//   (5) drain an unused adapter loopback for HostOnly; preserve the HostClient local view
// `before_server_tick`, when supplied, runs after tick_connections has completed any
// player spawns and before their first authoritative logic update / 0x0A fan. The opaque
// context keeps this shared owner loop independent of adapter-specific registration state.
// Advances owner.now_tick by 1.
using HostBeforeServerTickFn = void (*)(void *context);
using HostEventObserverFn = void (*)(void *context, const HostAcceptEvent &event);
void host_session_pump(HostOwner &owner, netsim::IDatagramSocket &sock,
		HostBeforeServerTickFn before_server_tick = nullptr,
		void *before_server_tick_context = nullptr,
		HostEventObserverFn event_observer = nullptr,
		void *event_observer_context = nullptr);

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
};

// Stand `owner` up through the shared in-match host bring-up used by apps/nw_server and the Godot
// host. `serve_and_play` selects HostClient and registers the type-2 loopback; otherwise it selects
// HostOnly and creates no local player. When serve-and-play also has a World, spawn the host player
// and queue its initial-state burst before the first per-frame 0x0A.
void start_host_session(HostOwner &owner, const HostConfig &cfg);

} // namespace opennova::np
