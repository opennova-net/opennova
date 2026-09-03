#include <runtime/session/host_session.h>
#include <runtime/devtools/tick_profile.h>
#include <base/io/tick_rate.h>
#include <base/io/perf_clock.h>

#include <runtime/session/server_session.h> // set_connection_mode / set_transport_mode / create_session / ...
#include <runtime/session/server_spawn.h>   // Server_InitNewRoundState / Server_ProcessPendingPlayerSpawns
#include <runtime/session/server_tick.h>    // Server_TickUpdate

#include <runtime/replication/connection_fan.h> // set_entity_send_budget (the BANDWIDTH cap)
#include <net/npwire/ingame_decode.h> // OrganicSpawnBatch / OrganicSpawnRecord
#include <net/npwire/ingame_encode.h> // encode_organic_spawn_batch
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_keys.h>

#include <runtime/world/entity.h>
#include <runtime/world/world.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <limits>
#include <utility>
#include <vector>

namespace opennova::np {

namespace {

// Retail's per-connection EMPTY send interval, host side (D-NET-173): with
// NOTHING queued and NOTHING retained, the send pump still mints a packet
// once this elapses, so a quiet peer (parked at the deploy pick, dead, or
// idle) never reaches its 120 s connection reap. The ACTIVE retained-records
// probe (10000 ms) is ported separately in tick_connections; the joiner legs
// are the same template values in joiner_connection.cpp.
// [orig: CNapiNPConnection_PumpSendIntervals @0x628FD0 — empty_interval leg
//  @0x629041..0x629067; idle_send_interval_ms = 30000 stored by
//  CNapiNetwork_Init @0x4ca4a0 (@0x4caab5/@0x4cab88); the reaping
//  timeout_ms = 120000 stored @0x4caa81/@0x4cab54]
constexpr uint64_t kHostSessionIdleSendIntervalMilliseconds = 30000;

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

// Split one semantic message into the retail protocol's FIRST/MID/FINAL
// fragment records when its encoded form cannot fit the installed session
// packet ceiling. The receiver's reassemble_protocol_payload joins these back
// into one dispatch, so producer semantics and ordering stay unchanged.
std::vector<ProtocolMessage> envelope_protocol_message(
		const ProtocolMessage &message) {
	std::vector<uint8_t> encoded;
	if (append_protocol_message(encoded, message) &&
			PROTOCOL_PACKET_HEADER_SIZE + encoded.size() <=
					kGameSessionMaxPacketBytes)
		return {message};

	const std::size_t skip_bytes = message.flags.skip1
			? 1u
			: (message.flags.skip2 ? 2u : 0u);
	const std::size_t max_payload =
			kGameSessionMaxPacketBytes - PROTOCOL_PACKET_HEADER_SIZE -
			kProtocolMessageLen16Bytes - skip_bytes;
	// Preserve dispatch-table and skip metadata; length/fragment state belongs
	// to each newly emitted physical record.
	constexpr uint8_t kSemanticFlagMask =
			PROTOCOL_MSG_FLAG_SETTINGS_UPDATE | PROTOCOL_MSG_FLAG_SKIP1 |
			PROTOCOL_MSG_FLAG_SKIP2 | 0x01u;
	const uint8_t base_flags = message.flags.raw & kSemanticFlagMask;

	std::vector<ProtocolMessage> out;
	for (std::size_t offset = 0; offset < message.payload.size();) {
		const std::size_t count = std::min(
				max_payload, message.payload.size() - offset);
		const bool first = offset == 0;
		const bool final = offset + count == message.payload.size();
		uint8_t fragment_flags = 0;
		if (!final)
			fragment_flags = first
					? PROTOCOL_MSG_FLAG_FRAG_CONT
					: static_cast<uint8_t>(PROTOCOL_MSG_FLAG_FRAG_CONT |
							PROTOCOL_MSG_FLAG_FRAG_END);
		else if (!first)
			fragment_flags = PROTOCOL_MSG_FLAG_FRAG_END;
		ProtocolMessage fragment = make_protocol_message(
				message.tag,
				std::vector<uint8_t>(message.payload.begin() + offset,
						message.payload.begin() + offset + count),
				static_cast<uint8_t>(base_flags | PROTOCOL_MSG_FLAG_LEN16 |
						fragment_flags));
		fragment.skip_bytes = message.skip_bytes;
		fragment.reliable = message.reliable;
		fragment.capacity_exempt = message.capacity_exempt;
		fragment.retention_flushes = message.retention_flushes;
		fragment.retention_deadline_flush =
				message.retention_deadline_flush;
		out.push_back(std::move(fragment));
		offset += count;
	}
	return out;
}

std::vector<ProtocolMessage> send_session_batches(
		HostOwner &owner, netsim::IDatagramSocket &sock,
		NapiNPConnection &connection, std::vector<ProtocolMessage> messages) {
	std::vector<ProtocolMessage> batch;
	std::vector<uint8_t> encoded_messages;

	auto flush = [&] {
		if (batch.empty()) return true;
		std::vector<uint8_t> datagram;
		if (!frame_in_match_s2c_batch(
					owner.ctx, connection.peer, batch, datagram))
			return false;
		sock.send_to(connection.peer, datagram.data(), datagram.size());
		connection.last_session_send_tick = owner.now_tick;
		batch.clear();
		encoded_messages.clear();
		return true;
	};

	std::vector<ProtocolMessage> enveloped;
	std::vector<ProtocolMessage> capacity_retry;
	std::size_t available_nodes = session_outbound_message_prefix_count(
			connection.seq, std::numeric_limits<std::size_t>::max());
	bool ordinary_tail_rejected = false;
	for (std::size_t semantic_index = 0;
	     semantic_index < messages.size(); ++semantic_index) {
		// A pre-enveloped FIRST/MID/FINAL run re-queued by an earlier failed
		// flush arrives as separate queue entries (possibly headless when its
		// leading pieces already shipped). Its consecutive fragment-flagged
		// pieces are ONE capacity unit through the closing FINAL, or a
		// partially admitted group strands the receiver's reassembly buffer.
		std::size_t unit_end = semantic_index;
		const bool fragment_unit =
				messages[semantic_index].flags.frag_cont ||
				messages[semantic_index].flags.frag_end;
		if (fragment_unit) {
			while (unit_end + 1 < messages.size()) {
				const ProtocolMessageFlags &piece_flags =
						messages[unit_end].flags;
				if (piece_flags.frag_end && !piece_flags.frag_cont)
					break; // the closing FINAL
				const ProtocolMessageFlags &next_flags =
						messages[unit_end + 1].flags;
				if (!next_flags.frag_cont && !next_flags.frag_end)
					break; // run ends unclosed
				++unit_end;
			}
		}
		std::vector<ProtocolMessage> pieces;
		for (std::size_t piece_index = semantic_index;
				piece_index <= unit_end; ++piece_index) {
			std::vector<ProtocolMessage> sub =
					envelope_protocol_message(messages[piece_index]);
			pieces.insert(pieces.end(),
					std::make_move_iterator(sub.begin()),
					std::make_move_iterator(sub.end()));
		}
		const std::size_t charged_nodes = static_cast<std::size_t>(std::count_if(
				pieces.begin(), pieces.end(),
				[](const ProtocolMessage &piece) {
					return !piece.capacity_exempt;
				}));
		// FIRST/MID/FINAL is one semantic unit.  Admitting a capacity prefix of
		// its physical records can strand the receiver's reassembly buffer, so
		// retry that unit as a whole — including a re-queued orphan run whose
		// earlier pieces already shipped (retail's queue never holds a
		// half-shipped group, so its one-record drop rule cannot apply there).
		// An ordinary one-record message still follows retail
		// QueueMessage/Create prefix rejection: it and the later tail are
		// dropped when no node remains. Capacity-exempt records remain
		// admissible after that rejected tail, matching retail's internal
		// flag-0x10 bypass.
		if (charged_nodes != 0 &&
				(ordinary_tail_rejected || charged_nodes > available_nodes)) {
			if (!ordinary_tail_rejected &&
					(pieces.size() > 1 || fragment_unit)) {
				for (std::size_t piece_index = semantic_index;
						piece_index <= unit_end; ++piece_index)
					capacity_retry.push_back(std::move(messages[piece_index]));
			}
			ordinary_tail_rejected = true;
			semantic_index = unit_end;
			continue;
		}
		available_nodes -= charged_nodes;
		enveloped.insert(enveloped.end(),
				std::make_move_iterator(pieces.begin()),
				std::make_move_iterator(pieces.end()));
		semantic_index = unit_end;
	}
	auto append_capacity_retry = [&](std::vector<ProtocolMessage> retry) {
		retry.insert(retry.end(),
				std::make_move_iterator(capacity_retry.begin()),
				std::make_move_iterator(capacity_retry.end()));
		return retry;
	};
	for (std::size_t i = 0; i < enveloped.size(); ++i) {
		const ProtocolMessage &message = enveloped[i];
		std::vector<uint8_t> candidate = encoded_messages;
		if (!append_protocol_message(candidate, message)) {
			if (!flush()) {
				batch.insert(batch.end(), enveloped.begin() + i, enveloped.end());
				return append_capacity_retry(std::move(batch));
			}
			return append_capacity_retry(std::vector<ProtocolMessage>(
					enveloped.begin() + i, enveloped.end()));
		}
		if (PROTOCOL_PACKET_HEADER_SIZE + candidate.size() >
				kGameSessionMaxPacketBytes) {
			if (!flush()) {
				batch.insert(batch.end(), enveloped.begin() + i, enveloped.end());
				return append_capacity_retry(std::move(batch));
			}
			candidate.clear();
			if (!append_protocol_message(candidate, message)) {
				return append_capacity_retry(std::vector<ProtocolMessage>(
						enveloped.begin() + i, enveloped.end()));
			}
		}
		batch.push_back(message);
		encoded_messages = std::move(candidate);
	}
	if (!flush()) return append_capacity_retry(std::move(batch));
	return capacity_retry;
}

bool is_established_s2c_datagram(const std::vector<uint8_t> &datagram) {
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	if (!nw_decode_inbound(
				datagram.data(), datagram.size(), opcode, body))
		return false;
	return opcode == SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE ||
	       opcode == SESSION_OPCODE_SERVER_RESEND_LIST;
}

void send_or_stage_established_datagram(
		HostOwner &owner, netsim::IDatagramSocket &sock, const PeerAddr &peer,
		const std::vector<uint8_t> &datagram) {
	if (is_established_s2c_datagram(datagram)) {
		for (const NapiNPConnection &connection :
				owner.ctx.np_protocol.connection_list) {
			if (connection.type != NapiNPConnection::kTypeServerSide || !(connection.peer == peer)) continue;
			owner.pending_session_datagrams[peer].push_back(datagram);
			return;
		}
	}
	// Pre-session 0x81/0x82 legs have no established connection send block.
	sock.send_to(peer, datagram.data(), datagram.size());
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
	case HostAcceptEvent::Kind::PeerHandshakeAdvanced:
		// Attach the semantic transport as soon as 0x42 establishes the
		// remote, before the initial-state producer starts queuing records.
		admit_peer(owner, sock, peer, ev);
		break;
	case HostAcceptEvent::Kind::PeerEnteredWorldStreaming:
	case HostAcceptEvent::Kind::PeerSpawned:
		admit_peer(owner, sock, peer, ev);
		break;
	case HostAcceptEvent::Kind::PeerC2SInMatch:
		// STAGE the joiner's in-match 0x0C onto its transport; Server_TickUpdate is the single drain,
		// so receive dispatch never applies it inline.
		apply_in_match_c2s(owner.ctx, ev);
		break;
	case HostAcceptEvent::Kind::PeerGoodbye:
		// The protocol layer has already completed entity/roster/node teardown. Cleanup here is
		// deliberately owner-only: a same-address replacement may already have created its fresh
		// connection before this event is dispatched, so dropping again by endpoint would delete it.
		owner.peers.erase(peer);
		owner.pending_session_messages.erase(peer);
		owner.pending_session_datagrams.erase(peer);
		break;
	default:
		break;
	}
}

void host_session_pump(HostOwner &owner, netsim::IDatagramSocket &sock,
		HostBeforeServerTickFn before_server_tick, void *before_server_tick_context,
		HostEventObserverFn event_observer, void *event_observer_context) {
	devtools::TickProfile *profile =
			owner.ctx.world != nullptr ? owner.ctx.world->profile : nullptr;
	const devtools::ProfileScope pump_scope(profile, devtools::Slot::SIM_HOST_PUMP);
	devtools::ProfileLap lap(profile);
	const uint32_t now = owner.now_tick;
	// S2C 0x58 reports elapsed session milliseconds, while gameplay producers
	// consume a 62 Hz logical tick. Keep those clock domains explicit: retail
	// computes GetTickCount - host_start_tick; this deterministic host derives
	// equivalent elapsed time from its fixed simulation clock.
	owner.ctx.np_protocol.host_run_duration_ms = static_cast<uint32_t>(
			(static_cast<uint64_t>(now) * 1000u) / uint64_t(io::kTicksPerSecondInt));
	auto &pending_session_messages = owner.pending_session_messages;

	// (1) recv-drain — drain everything pending this frame. The recv timeout lives in the socket owner.
	uint8_t buf[4096];
	for (;;) {
		PeerAddr peer{};
		const int n = sock.recv_from(buf, sizeof(buf), peer);
		if (n <= 0) break; // 0 = nothing left/timeout, <0 = error
		HandleResult r = handle_server_datagram(
				owner.ctx, peer, buf, static_cast<std::size_t>(n), now,
				/*defer_in_match_replies=*/true);
		// Same-address replacement reports PeerGoodbye for the OLD occupant after
		// the protocol layer has already installed the fresh connection and built
		// its 0x82/0x83 auth result. Retire the old owner's queues/link before
		// classifying that fresh outbound; doing it afterward would erase the newly
		// staged sequence-1 settings packet.
		for (const HostAcceptEvent &ev : r.events) {
			if (ev.kind != HostAcceptEvent::Kind::PeerGoodbye) continue;
			pending_session_messages.erase(peer);
			owner.pending_session_datagrams.erase(peer);
			dispatch_event(owner, sock, peer, ev);
			if (event_observer != nullptr)
				event_observer(event_observer_context, ev);
		}
		for (const std::vector<uint8_t> &dg : r.outbound) {
			// 0x81/0x82 remain immediate. The retained settings 0x83 and
			// established retransmits use the connection's send boundary.
			send_or_stage_established_datagram(owner, sock, peer, dg);
		}
		if (!r.deferred_session_replies.empty()) {
			auto &pending = pending_session_messages[peer];
			pending.insert(
					pending.end(),
					std::make_move_iterator(r.deferred_session_replies.begin()),
					std::make_move_iterator(r.deferred_session_replies.end()));
		}
		for (const HostAcceptEvent &ev : r.events) {
			if (ev.kind == HostAcceptEvent::Kind::PeerGoodbye) continue;
			dispatch_event(owner, sock, peer, ev);
			if (event_observer != nullptr)
				event_observer(event_observer_context, ev);
		}
	}
	// Advance each established remote's own send clock before any transport
	// producer runs. Closed peers retain their semantic/burst state; open peers
	// may build packets during the remainder of this pump.
	for (NapiNPConnection &c : owner.ctx.np_protocol.connection_list) {
		if (c.type != NapiNPConnection::kTypeServerSide) continue;
		if (!c.s2c_send_holdoff_dictated) {
			c.s2c_send_holdoff_countdown = 0;
			c.s2c_send_boundary_open = true;
			continue;
		}
		if (c.s2c_send_holdoff_countdown > 0)
			--c.s2c_send_holdoff_countdown;
		c.s2c_send_boundary_open = c.s2c_send_holdoff_countdown == 0;
	}
	// Resolve the retail missing-sequence latch only after the receive FIFO is empty. A later
	// datagram in this same drain may have closed the gap and emptied the ordered queue.
	for (TickOut &t : flush_server_missing_requests(
			owner.ctx, /*respect_s2c_send_boundary=*/true)) {
		for (const std::vector<uint8_t> &dg : t.outbound)
			send_or_stage_established_datagram(owner, sock, t.peer, dg);
	}
	lap.mark(devtools::Slot::SIM_HOST_RECEIVE);

	// (2) tick_connections — drive each not-yet-spawned peer's §5.2a burst; surface F3/PeerSpawned.
	for (TickOut &t : tick_connections(
			owner.ctx, /*elapsed_ms=*/16, now,
			/*respect_s2c_send_boundary=*/true)) {
		for (const std::vector<uint8_t> &dg : t.outbound) {
			send_or_stage_established_datagram(owner, sock, t.peer, dg);
		}
		for (const HostAcceptEvent &ev : t.events) {
			if (ev.kind == HostAcceptEvent::Kind::PeerGoodbye) {
				pending_session_messages.erase(t.peer);
				owner.pending_session_datagrams.erase(t.peer);
			}
			dispatch_event(owner, sock, t.peer, ev);
			if (event_observer != nullptr)
				event_observer(event_observer_context, ev);
		}
	}

	// Owner-side entity registration belongs between creation and the first body update.
	// Retail's AnimMap_RegisterEntity runs at entity creation; embedders with external
	// animation registries use this boundary to preserve the same lifetime.
	lap.mark(devtools::Slot::SIM_HOST_CONNECTIONS);
	if (before_server_tick != nullptr) before_server_tick(before_server_tick_context);
	lap.mark(devtools::Slot::SIM_HOST_ADAPTER);

	// (3) Logic and C2S remain full-rate. Server_TickUpdate consults the
	// already-advanced per-connection boundary only for fresh remote 0x0A.
	Server_TickUpdate(owner.ctx);
	lap.mark(devtools::Slot::SIM_SERVER_TICK);

	// (4) S2C flush — reframe each remote (type-1) transport's identity [tag][body] as a 0x83 + send.
	// Drain each remote transport into the ordered pending queue every tick;
	// only an open boundary frames and sends it. The host's own type-2 loopback
	// is consumed in-process and skipped here.
	for (NapiNPConnection &c : owner.ctx.np_protocol.connection_list) {
		if (c.type != NapiNPConnection::kTypeServerSide) continue;
		// A type-1 remote peer's transport is always the UdpSessionTransport admit_peer attached, so the
		// downcast to reach pop_outbound (an owner-boundary method, not on the base ISessionTransport) is
		// safe — the host's own type-2 loopback (a LoopbackChannel) is skipped above.
		if (c.link.transport != nullptr) {
			auto *udp = static_cast<netsim::UdpSessionTransport *>(c.link.transport);
			netsim::Datagram staged;
			while (udp->pop_outbound(staged)) {
				ProtocolMessage message = make_protocol_message(
						staged.tag, std::move(staged.body),
						staged.protocol_flags_raw);
				message.reliable = staged.reliable;
				message.capacity_exempt = staged.capacity_exempt;
				pending_session_messages[c.peer].push_back(std::move(message));
			}
		}
		if (!c.s2c_send_boundary_open) continue;
		// These packets were framed before this boundary and therefore carry
		// lower sequence numbers than the semantic messages framed below.
		auto pending_datagrams = owner.pending_session_datagrams.find(c.peer);
		if (pending_datagrams != owner.pending_session_datagrams.end()) {
			for (const std::vector<uint8_t> &datagram : pending_datagrams->second)
				sock.send_to(c.peer, datagram.data(), datagram.size());
			owner.pending_session_datagrams.erase(pending_datagrams);
			c.last_session_send_tick = now;
		}
		std::vector<ProtocolMessage> messages;
		auto pending = pending_session_messages.find(c.peer);
		if (pending != pending_session_messages.end()) {
			messages = std::move(pending->second);
			pending_session_messages.erase(pending);
		}
		std::vector<ProtocolMessage> retry =
				send_session_batches(owner, sock, c, std::move(messages));
		if (!retry.empty())
			pending_session_messages[c.peer] = std::move(retry);
		// The EMPTY send-interval leg (D-NET-173): retail's pump reads the
		// per-connection last-send clock this boundary just updated, and with
		// NOTHING queued and NOTHING retained still mints a header-only
		// sequence once the interval elapses — the keepalive a stock client's
		// 120 s reap requires. (The ACTIVE retained-records probe is
		// tick_connections' append_active_probe.) Arm on the first open
		// boundary. [orig: CNapiNPConnection_PumpSendIntervals @0x628FD0]
		if (c.last_session_send_tick == 0) {
			c.last_session_send_tick = now;
		} else if (c.seq.retained_outbound_message_count == 0 &&
				pending_session_messages.find(c.peer) ==
						pending_session_messages.end()) {
			const uint64_t elapsed_ms = static_cast<uint64_t>(
					now - c.last_session_send_tick) * 1000u / uint64_t(io::kTicksPerSecondInt);
			if (elapsed_ms > kHostSessionIdleSendIntervalMilliseconds) {
				std::vector<uint8_t> keepalive;
				if (frame_in_match_s2c_batch(owner.ctx, c.peer, {}, keepalive)) {
					sock.send_to(c.peer, keepalive.data(), keepalive.size());
					c.last_session_send_tick = now;
				}
			}
		}
		// One OPEN host send boundary can contain preframed settings/resends,
		// several MTU-split semantic packets, or no payload at all. Retail prunes
		// finite message nodes after all of them, then increments +0x64C once.
		complete_session_send_flush(c.seq);
		if (c.s2c_send_holdoff_dictated) {
			c.s2c_send_holdoff_countdown = c.s2c_send_holdoff_ticks;
			c.s2c_send_boundary_open = c.s2c_send_holdoff_ticks == 0;
		} else {
			c.s2c_send_holdoff_countdown = 0;
			c.s2c_send_boundary_open = true;
		}
	}

	// (5) A dedicated host registers no type-2 local client. If its embedder nevertheless supplied a
	// loopback channel, defensively drain it so that unused input cannot accumulate. A serve-and-play
	// owner instead folds the registered loopback into ClientState after this pump, so preserve it.
	if (owner.host_loopback != nullptr && !owner.serve_and_play) {
		netsim::Datagram discard;
		while (owner.host_loopback->client_recv(discard)) { /* discard the host's own view */ }
	}

	++owner.now_tick;
	lap.mark(devtools::Slot::SIM_HOST_SEND);
}

void start_host_session(HostOwner &owner, const HostConfig &cfg) {
	owner.now_tick = 0;
	owner.serve_and_play = cfg.serve_and_play; // the pump's step-5 loopback handling reads this
	owner.pending_session_messages.clear();
	owner.pending_session_datagrams.clear();
	// Apply the configured 0x0A byte cap to the netsim global (the retail
	// BANDWIDTH command's target [orig: g_entity_send_budget @0xC8FC50]).
	netsim::set_entity_send_budget(
			static_cast<int>(cfg.config.entity_send_budget));
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
	// The type-2 loopback is the host's own client: it never uploads ClientAuth CU
	// vars, so its per-side character selection is installed here — before
	// Server_ProcessPendingPlayerSpawns stamps the local player from it.
	for (NapiNPConnection &connection : owner.ctx.np_protocol.connection_list) {
		if (connection.type == NapiNPConnection::kTypeClientSide) {
			connection.char_vars = cfg.local_character_vars;
		}
	}
	if (owner.ctx.world != nullptr) {
		world::MatchRules match_rules;
		match_rules.game_type = owner.ctx.config.game_type;
		// SET GameTime feeds g_respawn_time in retail. The existing host model
		// calls that field respawn_time; KOTHLimit/time_limit_minutes is unrelated.
		// [orig: Game_StartMission seed @0x524F66]
		match_rules.game_time_minutes = owner.ctx.config.respawn_time;
		match_rules.score_limit = owner.ctx.config.score_limit;
		match_rules.hill_limit_minutes = owner.ctx.config.time_limit_minutes;
		match_rules.hill_delta = owner.ctx.config.koth_delta;
		match_rules.max_score = owner.ctx.config.max_score;
		match_rules.flag_return_ticks = owner.ctx.config.flag_return_ticks;
		match_rules.capture_duration_seconds =
				owner.ctx.config.capture_duration_seconds;
		match_rules.capture_speed_setting =
				owner.ctx.config.capture_speed_setting;
		match_rules.team_count = owner.ctx.config.num_teams;
		match_rules.score_values = owner.ctx.config.session_status_stat_values;
		match_rules.score_fields.reserve(owner.ctx.config.scoreboard_fields.size());
		for (const auto &[field, enabled] : owner.ctx.config.scoreboard_fields)
			match_rules.score_fields.push_back({field, enabled});
		owner.ctx.world->match.configure(match_rules);
		owner.ctx.world->spawn_waves.build_from_mission(
				*owner.ctx.world, owner.ctx.config.spawn_wave_time_base,
				owner.ctx.config.spawn_wave_time_zone);
		owner.ctx.world->mp_session =
				cfg.socket_mode != SocketMode::Socketless;
		owner.ctx.world->destroy_buildings =
				owner.ctx.config.destroy_buildings != 0;
		// [orig: dword_24D1E34 & 0x8000, "TeamTriggerClaymore" admin set @ 0x405f16]
		owner.ctx.world->throwables.team_trigger_claymore =
				(owner.ctx.config.mp_attributes & GameConfig::kMpAttribClaymorePref) != 0;
		// The MP NoTracers rule: bit 0 of the same rules word kills the tracer
		// visual at round spawn unless the ammo is FORCETRACER; the lobby
		// publishes its inverse as the "Tracers" key [orig: g_rules_flags
		// @ 0x24D1E34 & 1 at RoundData_SpawnRound @ 0x4ec41f; admin set
		// @ 0x405c80; Lobby_UpdateServerInfo "Tracers" @ 0x4fee4f]
		owner.ctx.world->round_sim.no_tracers_rule =
				(owner.ctx.config.mp_attributes & GameConfig::kMpAttribNoTracers) != 0;
	}
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
