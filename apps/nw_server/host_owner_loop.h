#pragma once

// The in-match HOST owner loop (P6) — the socket-bearing driver that turns the Godot-free, socket-free
// libs/npruntime runtime into a real-UDP game server. It is the faithful reimpl of the original engine's
// per-frame host pump [orig: CNapiNetwork_PumpManagerReceive @0x4c4d10 (recv, mgr flag 4, 250ms) +
// CNapiNetwork_SendUDPPacket @0x4c4d30 (CNapiNPManager_SendTo) wrapped around Server_TickUpdate], with
// the real socket I/O done here (apps/common/net_sockets) and ALL protocol/crypto/framing left in the
// libs (.agents/network.md: "UdpSessionTransport is an internal identity-frame conduit; NWU/SCRK/PN/
// ProtocolMessage framing stay in libs/novaworld"). It lives under apps/ (not libs/) because it holds a
// socket; libs/npruntime is socket-free. Shared by apps/nw_server/main.cpp AND the two-endpoint ctest so
// the wire owner loop has exactly one implementation. All functions are inline (header-only, no ODR
// conflict across the two translation units).

#include <npruntime/napi_np_connection.h>
#include <npruntime/napi_np_protocol.h>
#include <npruntime/napi_np_server_ctx.h>
#include <npruntime/server_spawn.h>
#include <npruntime/server_tick.h>

#include <netsim/connection.h>          // netsim::TransportMode
#include <netsim/session_transport.h>   // netsim::Datagram / ISessionTransport
#include <netsim/udp_session_transport.h>

#include <novaworld/connection/registry.h> // opennova::PeerAddr
#include <novaworld/ingame_decode.h>       // OrganicSpawnBatch / OrganicSpawnRecord
#include <novaworld/ingame_encode.h>       // encode_organic_spawn_batch

#include <world/entity.h>
#include <world/world.h>

#include "net_sockets.h" // opennova::net (apps/common, PUBLIC include dir)

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace opennova::nw_server {

// net::Endpoint carries the IPv4 as 4 octets (octet0 first); PeerAddr.ip is LE octet packing
// (octet0 in the low byte) — so the conversion is a straight pack/unpack, NOT a byte swap. Verified:
// 127.0.0.1 -> PeerAddr.ip 0x0100007F (== the value client_runtime_test hard-codes).
inline PeerAddr to_peer(const net::Endpoint &e) {
	return PeerAddr{static_cast<uint32_t>(e.ip[0]) | (static_cast<uint32_t>(e.ip[1]) << 8) |
	                        (static_cast<uint32_t>(e.ip[2]) << 16) |
	                        (static_cast<uint32_t>(e.ip[3]) << 24),
	                e.port};
}

inline net::Endpoint to_endpoint(const PeerAddr &p) {
	net::Endpoint e;
	e.ip[0] = static_cast<uint8_t>(p.ip & 0xFFu);
	e.ip[1] = static_cast<uint8_t>((p.ip >> 8) & 0xFFu);
	e.ip[2] = static_cast<uint8_t>((p.ip >> 16) & 0xFFu);
	e.ip[3] = static_cast<uint8_t>((p.ip >> 24) & 0xFFu);
	e.port = p.port;
	return e;
}

// Per-remote-peer owner state: the socket-free transport the owner pumps (owns its lifetime; the
// connection's link.transport is a non-owning pointer into it) + the once-latch for the joiner's named
// dcb-bearing organic-spawn announce.
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

// The in-match host: the npruntime ctx + the per-peer transports + the host's own loopback drain. The
// ctx.world / ctx.mission are wired by the owner (main.cpp / the test) before the first pump.
struct HostOwner {
	np::NapiNPServerCtx ctx;
	netsim::ISessionTransport *host_loopback = nullptr; // non-owning; the host's own dcb-2 client (Listen)
	std::map<PeerAddr, PeerLink, PeerAddrLess> peers;
	uint32_t now_tick = 0;
};

// Attach a UdpSessionTransport to `peer`'s connection (idempotent) and, ONCE the spawn pipeline has
// bound owned_entity, stream the joiner's NAMED dcb-bearing S2C 0x0C organic-spawn so the joiner
// name-matches its own player (entity_name == its ClientHello.co) and learns its wire handle H = the
// admitted entity's packed handle (D.0 / §5.23; the F3 dcb-timing contract). Mirrors
// NovaSimulation::announce_joiner_organic_spawn. owned_entity is bound by the automatic spawn pipeline
// (tick_connections -> Server_ProcessPendingPlayerSpawns -> Server_BuildPlayerInfoAndAdd), so this
// no-ops until that has run (by PeerSpawned at the latest), then announces exactly once.
inline void admit_peer(HostOwner &owner, net::Socket &sock, const PeerAddr &peer,
                       const np::HostAcceptEvent &ev) {
	PeerLink &link = owner.peers[peer];

	np::NapiNPConnection *conn = nullptr;
	for (np::NapiNPConnection &c : owner.ctx.np_protocol.connection_list) {
		if (c.peer == peer) {
			conn = &c;
			break;
		}
	}
	if (conn == nullptr) return;

	if (link.transport == nullptr) {
		link.transport =
				std::make_unique<netsim::UdpSessionTransport>(netsim::UdpSessionTransport::Role::Host);
	}
	if (conn->link.transport == nullptr) {
		conn->link.transport = link.transport.get();
		conn->link.mode = netsim::TransportMode::Client;
	}

	if (link.announced || !conn->link.owned_entity.valid()) return; // wait for the spawn pipeline

	OrganicSpawnBatch batch;
	batch.entity_count = 1;
	OrganicSpawnRecord rec;
	rec.slot_id = static_cast<uint16_t>(conn->link.owned_entity.packed); // wire handle H
	rec.has_body = true;
	rec.item_type_id = 0x14B9;                  // player infantry template
	rec.entity_name = ev.peer_name;             // the name-match key (the joiner's ClientHello.co)
	rec.entity_flags = ev.self_id;              // entity+0x78: the dcb the client self-matches (0x48 ack)
	rec.minimap_flags = 0x100;                  // entity+0x36 bit 0x100: local-player/minimap register
	rec.pos_x = ev.pose.pos_x;
	rec.pos_y = ev.pose.pos_y;
	rec.pos_z = ev.pose.pos_z;
	rec.orientation = static_cast<int32_t>(ev.pose.heading) << 16; // i16 wire heading -> 32-bit BAM
	rec.team = ev.pose.team;
	if (owner.ctx.world != nullptr) {
		if (const world::Entity *e = owner.ctx.world->registry.get(conn->link.owned_entity)) {
			rec.net_id = e->net_id;
		}
	}
	batch.records.push_back(std::move(rec));

	std::vector<uint8_t> dg;
	if (np::frame_in_match_s2c(owner.ctx, peer, 0x0C, encode_organic_spawn_batch(batch), dg)) {
		const net::Endpoint to = to_endpoint(peer);
		net::udp_send_to(sock, to, dg.data(), dg.size());
		link.announced = true; // latch only on a successful frame+send (retry otherwise)
	}
}

// React to one HostAcceptEvent surfaced by handle_server_datagram / tick_connections.
inline void dispatch_event(HostOwner &owner, net::Socket &sock, const PeerAddr &peer,
                           const np::HostAcceptEvent &ev) {
	switch (ev.kind) {
	case np::HostAcceptEvent::Kind::PeerEnteredWorldStreaming:
	case np::HostAcceptEvent::Kind::PeerSpawned:
		admit_peer(owner, sock, peer, ev);
		break;
	case np::HostAcceptEvent::Kind::PeerC2SInMatch:
		// STAGE the joiner's in-match 0x0C onto its transport; Server_TickUpdate is the single drain
		// (D-NET-125 — never apply inline).
		np::apply_in_match_c2s(owner.ctx, ev);
		break;
	case np::HostAcceptEvent::Kind::PeerGoodbye:
		owner.peers.erase(peer);            // release the owner's transport (the node is already erased)
		np::drop_connection(owner.ctx, peer); // idempotent if the goodbye already erased it
		break;
	case np::HostAcceptEvent::Kind::PeerHandshakeAdvanced:
	default:
		break;
	}
}

// One full owner iteration over `sock` (the §5.44 recv-before-send order):
//   (1) recv-drain: udp_recv_from -> handle_server_datagram -> ship replies + react to events
//   (2) tick_connections: pre-spawn §5.2a bursts (framed 0x83 for remote peers) + surface F3/spawned
//   (3) Server_TickUpdate: the single C2S drain + one logic tick + per-connection 0x0A fan
//   (4) S2C flush: pop each remote transport's identity [tag][body] -> frame_in_match_s2c (0x83) -> send
//   (5) drain the host's own loopback (no socket consumer for a headless Listen host)
// `recv_timeout_ms` is the per-poll recv wait: 0 (default) is non-blocking — right for main.cpp's busy
// 62 Hz loop, which paces with sleep_until; a small value (e.g. 30) lets a single-threaded poll-pump
// driver (the two-endpoint test) block briefly for the peer's datagram instead of spinning. Advances
// owner.now_tick by 1.
inline void host_owner_pump(HostOwner &owner, net::Socket &sock, int recv_timeout_ms = 0) {
	const uint32_t now = owner.now_tick;

	// (1) recv-drain — drain everything pending this frame.
	uint8_t buf[4096];
	for (;;) {
		net::Endpoint from{};
		const int n = net::udp_recv_from(sock, buf, sizeof(buf), from, recv_timeout_ms);
		if (n <= 0) break; // 0 = nothing left/timeout, <0 = error
		const PeerAddr peer = to_peer(from);
		np::HandleResult r =
				np::handle_server_datagram(owner.ctx, peer, buf, static_cast<std::size_t>(n), now);
		for (const std::vector<uint8_t> &dg : r.outbound) {
			net::udp_send_to(sock, from, dg.data(), dg.size()); // 0x81/0x82/0x83 handshake replies
		}
		for (const np::HostAcceptEvent &ev : r.events) dispatch_event(owner, sock, peer, ev);
	}

	// (2) tick_connections — drive each not-yet-spawned peer's §5.2a burst; surface F3/PeerSpawned.
	for (np::TickOut &t : np::tick_connections(owner.ctx, /*elapsed_ms=*/16, now)) {
		const net::Endpoint to = to_endpoint(t.peer);
		for (const std::vector<uint8_t> &dg : t.outbound) {
			net::udp_send_to(sock, to, dg.data(), dg.size()); // framed 0x83 burst datagrams
		}
		for (const np::HostAcceptEvent &ev : t.events) dispatch_event(owner, sock, t.peer, ev);
	}

	// (3) the authoritative per-frame host loop (single C2S drain + logic tick + 0x0A fan).
	np::Server_TickUpdate(owner.ctx);

	// (4) S2C flush — reframe each remote (type-1) transport's identity [tag][body] as a 0x83 + send.
	// The host's own type-2 loopback is skipped (its 0x0A is consumed in-process, step 5).
	for (np::NapiNPConnection &c : owner.ctx.np_protocol.connection_list) {
		if (c.type != 1 || c.link.transport == nullptr) continue;
		// A type-1 remote peer's transport is always the UdpSessionTransport admit_peer attached, so the
		// downcast to reach pop_outbound (an owner-boundary method, not on the base ISessionTransport) is
		// safe — the host's own type-2 loopback (a LoopbackChannel) is skipped above.
		auto *udp = static_cast<netsim::UdpSessionTransport *>(c.link.transport);
		const net::Endpoint to = to_endpoint(c.peer);
		std::vector<uint8_t> raw;
		while (udp->pop_outbound(raw)) {
			if (raw.empty()) continue;
			const uint8_t tag = raw[0];
			const std::vector<uint8_t> body(raw.begin() + 1, raw.end());
			std::vector<uint8_t> dg;
			if (np::frame_in_match_s2c(owner.ctx, c.peer, tag, body, dg)) {
				net::udp_send_to(sock, to, dg.data(), dg.size());
			}
		}
	}

	// (5) drain the host's own loopback 0x0A / burst (a headless Listen host has no local view to
	// consume it — without this its FIFO grows unbounded).
	if (owner.host_loopback != nullptr) {
		netsim::Datagram discard;
		while (owner.host_loopback->client_recv(discard)) { /* discard the host's own view */ }
	}

	++owner.now_tick;
}

} // namespace opennova::nw_server
