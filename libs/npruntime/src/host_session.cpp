#include "npruntime/host_session.h"

#include "npruntime/server_session.h" // set_connection_mode / set_transport_mode / create_session / ...
#include "npruntime/server_spawn.h"   // Server_InitNewRoundState / Server_ProcessPendingPlayerSpawns
#include "npruntime/server_tick.h"    // Server_TickUpdate

#include <npwire/ingame_decode.h> // OrganicSpawnBatch / OrganicSpawnRecord
#include <npwire/ingame_encode.h> // encode_organic_spawn_batch

#include <world/entity.h>
#include <world/world.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

namespace opennova::np {

void admit_peer(HostOwner &owner, netsim::IDatagramSocket &sock, const PeerAddr &peer,
                const HostAcceptEvent &ev) {
	PeerLink &link = owner.peers[peer];

	NapiNPConnection *conn = nullptr;
	for (NapiNPConnection &c : owner.ctx.np_protocol.connection_list) {
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

	// The joiner's own pool-0 spawn record now ships IN-PHASE via build_pool0_organic_batch (the
	// initial-state world stream: 0x10 -> 0x0D -> 0x0C -> 0x20), which already carries its name, dcb
	// (entity+0x78), net_id, playerClass and per-recipient minimap_flags. A same-map retail↔retail
	// ASH_I5A capture (2026-07-01) shows the host sends each player's 0x0C EXACTLY ONCE, in that phase
	// order — NOT an early out-of-band 0x0C. The prior early send here was a duplicate that put a 0x0C on
	// the wire right after the first static batch, diverging from retail's load order (load-sequence diff
	// 2026-07-01). Latch announced so the pipeline proceeds; the in-phase stream is the single source.
	(void)sock;
	(void)ev;
	link.announced = true;
}

void dispatch_event(HostOwner &owner, netsim::IDatagramSocket &sock, const PeerAddr &peer,
                    const HostAcceptEvent &ev) {
	switch (ev.kind) {
	case HostAcceptEvent::Kind::PeerEnteredWorldStreaming:
	case HostAcceptEvent::Kind::PeerSpawned:
		admit_peer(owner, sock, peer, ev);
		break;
	case HostAcceptEvent::Kind::PeerC2SInMatch:
		// STAGE the joiner's in-match 0x0C onto its transport; Server_TickUpdate is the single drain
		// (D-NET-125 — never apply inline).
		apply_in_match_c2s(owner.ctx, ev);
		break;
	case HostAcceptEvent::Kind::PeerGoodbye:
		owner.peers.erase(peer);              // release the owner's transport (the node is already erased)
		drop_connection(owner.ctx, peer);     // idempotent if the goodbye already erased it
		break;
	case HostAcceptEvent::Kind::PeerHandshakeAdvanced:
	default:
		break;
	}
}

void host_session_pump(HostOwner &owner, netsim::IDatagramSocket &sock,
		HostBeforeServerTickFn before_server_tick, void *before_server_tick_context) {
	const uint32_t now = owner.now_tick;

	// (1) recv-drain — drain everything pending this frame. The recv timeout lives in the adapter.
	uint8_t buf[4096];
	for (;;) {
		PeerAddr peer{};
		const int n = sock.recv_from(buf, sizeof(buf), peer);
		if (n <= 0) break; // 0 = nothing left/timeout, <0 = error
		HandleResult r = handle_server_datagram(owner.ctx, peer, buf, static_cast<std::size_t>(n), now);
		for (const std::vector<uint8_t> &dg : r.outbound) {
			sock.send_to(peer, dg.data(), dg.size()); // 0x81/0x82/0x83 handshake replies
		}
		for (const HostAcceptEvent &ev : r.events) dispatch_event(owner, sock, peer, ev);
	}
	// Resolve the retail missing-sequence latch only after the receive FIFO is empty. A later
	// datagram in this same drain may have closed the gap and emptied the ordered queue.
	for (TickOut &t : flush_server_missing_requests(owner.ctx)) {
		for (const std::vector<uint8_t> &dg : t.outbound)
			sock.send_to(t.peer, dg.data(), dg.size());
	}

	// (2) tick_connections — drive each not-yet-spawned peer's §5.2a burst; surface F3/PeerSpawned.
	for (TickOut &t : tick_connections(owner.ctx, /*elapsed_ms=*/16, now)) {
		for (const std::vector<uint8_t> &dg : t.outbound) {
			sock.send_to(t.peer, dg.data(), dg.size()); // framed 0x83 burst datagrams
		}
		for (const HostAcceptEvent &ev : t.events) dispatch_event(owner, sock, t.peer, ev);
	}

	// Owner-side entity registration belongs between creation and the first body update.
	// Retail's AnimMap_RegisterEntity runs at entity creation; adapters with external
	// animation registries use this boundary to preserve the same lifetime.
	if (before_server_tick != nullptr) before_server_tick(before_server_tick_context);

	// (3) the authoritative per-frame host loop (single C2S drain + logic tick + 0x0A fan).
	Server_TickUpdate(owner.ctx);

	// (4) S2C flush — reframe each remote (type-1) transport's identity [tag][body] as a 0x83 + send.
	// The host's own type-2 loopback is skipped (its 0x0A is consumed in-process, step 5).
	for (NapiNPConnection &c : owner.ctx.np_protocol.connection_list) {
		if (c.type != 1 || c.link.transport == nullptr) continue;
		// A type-1 remote peer's transport is always the UdpSessionTransport admit_peer attached, so the
		// downcast to reach pop_outbound (an owner-boundary method, not on the base ISessionTransport) is
		// safe — the host's own type-2 loopback (a LoopbackChannel) is skipped above.
		auto *udp = static_cast<netsim::UdpSessionTransport *>(c.link.transport);
		std::vector<uint8_t> raw;
		while (udp->pop_outbound(raw)) {
			if (raw.empty()) continue;
			const uint8_t tag = raw[0];
			const std::vector<uint8_t> body(raw.begin() + 1, raw.end());
			std::vector<uint8_t> dg;
			if (frame_in_match_s2c(owner.ctx, c.peer, tag, body, dg)) {
				sock.send_to(c.peer, dg.data(), dg.size());
			}
		}
	}

	// (5) drain the host's own loopback 0x0A / burst (a headless Listen host has no local view to
	// consume it — without this its FIFO grows unbounded). A serve-and-play owner instead FOLDS the
	// loopback into its ClientState (to render the host's own view) AFTER this pump, so it must keep
	// the data — skip the discard for it.
	if (owner.host_loopback != nullptr && !owner.serve_and_play) {
		netsim::Datagram discard;
		while (owner.host_loopback->client_recv(discard)) { /* discard the host's own view */ }
	}

	++owner.now_tick;
}

void start_host_session(HostOwner &owner, const HostConfig &cfg) {
	owner.serve_and_play = cfg.serve_and_play; // the pump's step-5 loopback handling reads this
	// The witnessed §5.0 listen-host bring-up [orig: SinglePlayer_StartMission @0x561af0]. mode 3 =
	// host + client; the host's own dcb-2 loopback (owner.host_loopback) registers as the local client.
	set_connection_mode(owner.ctx, ConnectionMode::HostClient);
	set_transport_mode(owner.ctx, cfg.socket_mode);
	SessionStartup startup;
	startup.host_key = cfg.host_key;
	create_session(owner.ctx, cfg.config, startup, owner.host_loopback); // also runs Server_InitNewRoundState (§5.2a step 1)
	if (owner.ctx.world != nullptr) {
		// [orig: dword_24D1E34 & 0x8000, "TeamTriggerClaymore" admin set @ 0x405f16]
		owner.ctx.world->throwables.team_trigger_claymore =
				(owner.ctx.config.mp_attributes & 0x8000u) != 0;
	}
	configure_session_runtime(owner.ctx);

	if (cfg.serve_and_play && owner.ctx.world != nullptr) {
		// Serve-and-play: spawn the host's own player and queue its load-time stream before it gets the
		// per-frame 0x0A its local view renders from. A dedicated/headless host skips this — its player
		// spawns lazily via tick_connections in the pump, and its loopback is discarded (pump step 5).
		Server_ProcessPendingPlayerSpawns(owner.ctx, *owner.ctx.world);
		// Run the existing type-2 initial-state path instead of latching spawned directly. The
		// loopback's effectively-unbounded burst budget queues every load-time pool batch now,
		// including the 0x0D carrier pose required by mounted children whose ewep parent has no
		// per-frame compact callback.
		(void)tick_connections(owner.ctx, /*elapsed_ms=*/0, owner.now_tick);
	}
}

} // namespace opennova::np
