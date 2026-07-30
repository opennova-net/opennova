#include "npruntime/host_session.h"

#include "npruntime/server_session.h" // set_connection_mode / set_transport_mode / create_session / ...
#include "npruntime/server_spawn.h"   // Server_InitNewRoundState / Server_ProcessPendingPlayerSpawns
#include "npruntime/server_tick.h"    // Server_TickUpdate

#include <npwire/ingame_decode.h> // OrganicSpawnBatch / OrganicSpawnRecord
#include <npwire/ingame_encode.h> // encode_organic_spawn_batch
#include <npwire/nw_session_framing.h>
#include <npwire/protocol_message.h>
#include <npwire/session_keys.h>

#include <world/entity.h>
#include <world/world.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <utility>
#include <vector>

namespace opennova::np {

namespace {

constexpr std::size_t kGameSessionMaxPacketBytes = 1300;

uint32_t mint_nonzero_session_value() {
	uint32_t value = 0;
	while (value == 0) value = make_random_session_u32();
	return value;
}

uint32_t monotonic_milliseconds() {
	const uint64_t value = static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now().time_since_epoch()).count());
	const uint32_t low = static_cast<uint32_t>(value);
	return low != 0 ? low : 1u;
}

SessionStartup make_session_startup(const HostConfig &cfg) {
	SessionStartup startup;
	startup.host_key = cfg.host_key != 0 ? cfg.host_key : mint_nonzero_session_value();
	startup.host_start_tick =
			cfg.host_start_tick != 0 ? cfg.host_start_tick : monotonic_milliseconds();
	if (cfg.session_seed_id != 0) {
		startup.session_seed_id = cfg.session_seed_id;
	} else {
		const uint64_t mixed =
				static_cast<uint64_t>(startup.host_start_tick) +
				mint_nonzero_session_value();
		startup.session_seed_id = static_cast<uint32_t>(mixed % 900000u) + 100000u;
	}
	return startup;
}

void send_session_batches(HostOwner &owner, netsim::IDatagramSocket &sock,
		NapiNPConnection &connection, std::vector<ProtocolMessage> messages) {
	std::vector<ProtocolMessage> batch;
	std::vector<uint8_t> encoded_messages;

	auto flush = [&] {
		if (batch.empty()) return;
		std::vector<uint8_t> datagram;
		if (frame_in_match_s2c_batch(owner.ctx, connection.peer, batch, datagram))
			sock.send_to(connection.peer, datagram.data(), datagram.size());
		batch.clear();
		encoded_messages.clear();
	};

	for (ProtocolMessage &message : messages) {
		std::vector<uint8_t> candidate = encoded_messages;
		if (!append_protocol_message(candidate, message)) {
			flush();
			continue;
		}
		if (!batch.empty() &&
		    PROTOCOL_PACKET_HEADER_SIZE + candidate.size() > kGameSessionMaxPacketBytes) {
			flush();
			candidate.clear();
			if (!append_protocol_message(candidate, message)) continue;
		}
		batch.push_back(std::move(message));
		encoded_messages = std::move(candidate);
	}
	flush();
}

} // namespace

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
		// The protocol layer has already completed entity/roster/node teardown. Cleanup here is
		// deliberately owner-only: a same-address replacement may already have created its fresh
		// connection before this event is dispatched, so dropping again by endpoint would delete it.
		owner.peers.erase(peer);
		break;
	case HostAcceptEvent::Kind::PeerHandshakeAdvanced:
	default:
		break;
	}
}

void host_session_pump(HostOwner &owner, netsim::IDatagramSocket &sock,
		HostBeforeServerTickFn before_server_tick, void *before_server_tick_context,
		HostEventObserverFn event_observer, void *event_observer_context) {
	const uint32_t now = owner.now_tick;
	std::map<PeerAddr, std::vector<ProtocolMessage>, PeerAddrLess> deferred_session_replies;

	// (1) recv-drain — drain everything pending this frame. The recv timeout lives in the adapter.
	uint8_t buf[4096];
	for (;;) {
		PeerAddr peer{};
		const int n = sock.recv_from(buf, sizeof(buf), peer);
		if (n <= 0) break; // 0 = nothing left/timeout, <0 = error
		HandleResult r = handle_server_datagram(
				owner.ctx, peer, buf, static_cast<std::size_t>(n), now,
				/*defer_in_match_replies=*/true);
		for (const std::vector<uint8_t> &dg : r.outbound) {
			sock.send_to(peer, dg.data(), dg.size()); // 0x81/0x82/0x83 handshake replies
		}
		if (!r.deferred_session_replies.empty()) {
			auto &pending = deferred_session_replies[peer];
			pending.insert(
					pending.end(),
					std::make_move_iterator(r.deferred_session_replies.begin()),
					std::make_move_iterator(r.deferred_session_replies.end()));
		}
		for (const HostAcceptEvent &ev : r.events) {
			if (ev.kind == HostAcceptEvent::Kind::PeerGoodbye)
				deferred_session_replies.erase(peer);
			dispatch_event(owner, sock, peer, ev);
			if (event_observer != nullptr)
				event_observer(event_observer_context, ev);
		}
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
		for (const HostAcceptEvent &ev : t.events) {
			if (ev.kind == HostAcceptEvent::Kind::PeerGoodbye)
				deferred_session_replies.erase(t.peer);
			dispatch_event(owner, sock, t.peer, ev);
			if (event_observer != nullptr)
				event_observer(event_observer_context, ev);
		}
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
		std::vector<ProtocolMessage> messages;
		auto deferred = deferred_session_replies.find(c.peer);
		if (deferred != deferred_session_replies.end()) {
			messages = std::move(deferred->second);
			deferred_session_replies.erase(deferred);
		}
		std::vector<uint8_t> raw;
		while (udp->pop_outbound(raw)) {
			if (raw.empty()) continue;
			const uint8_t tag = raw[0];
			const std::vector<uint8_t> body(raw.begin() + 1, raw.end());
			messages.push_back(make_protocol_message(tag, body));
		}
		send_session_batches(owner, sock, c, std::move(messages));
	}

	// (5) A dedicated host registers no type-2 local client. If its adapter nevertheless supplied a
	// loopback channel, defensively drain it so that unused input cannot accumulate. A serve-and-play
	// owner instead folds the registered loopback into ClientState after this pump, so preserve it.
	if (owner.host_loopback != nullptr && !owner.serve_and_play) {
		netsim::Datagram discard;
		while (owner.host_loopback->client_recv(discard)) { /* discard the host's own view */ }
	}

	++owner.now_tick;
}

void start_host_session(HostOwner &owner, const HostConfig &cfg) {
	owner.serve_and_play = cfg.serve_and_play; // the pump's step-5 loopback handling reads this
	// Select the witnessed §5.0 table row: serve-and-play is mode 3 (host + local client);
	// dedicated/headless is mode 1 (host only, no dcb-2 loopback player).
	set_connection_mode(
			owner.ctx,
			cfg.serve_and_play ? ConnectionMode::HostClient : ConnectionMode::HostOnly);
	set_transport_mode(owner.ctx, cfg.socket_mode);
	const SessionStartup startup = make_session_startup(cfg);
	create_session(
			owner.ctx, cfg.config, startup,
			cfg.serve_and_play ? owner.host_loopback : nullptr); // also runs Server_InitNewRoundState
	if (owner.ctx.world != nullptr) {
		// [orig: dword_24D1E34 & 0x8000, "TeamTriggerClaymore" admin set @ 0x405f16]
		owner.ctx.world->throwables.team_trigger_claymore =
				(owner.ctx.config.mp_attributes & GameConfig::kMpAttribClaymorePref) != 0;
	}
	configure_session_runtime(owner.ctx);

	if (cfg.serve_and_play && owner.ctx.world != nullptr) {
		// Serve-and-play: spawn the host's own player and queue its load-time stream before it gets the
		// per-frame 0x0A its local view renders from. A dedicated/headless HostOnly session has no
		// local-player connection, so it intentionally performs neither operation.
		Server_ProcessPendingPlayerSpawns(owner.ctx, *owner.ctx.world);
		// Run the existing type-2 initial-state path instead of latching spawned directly. The
		// loopback's effectively-unbounded burst budget queues every load-time pool batch now,
		// including the 0x0D carrier pose required by mounted children whose ewep parent has no
		// per-frame compact callback.
		(void)tick_connections(owner.ctx, /*elapsed_ms=*/0, owner.now_tick);
	}
}

} // namespace opennova::np
