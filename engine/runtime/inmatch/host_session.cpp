#include <runtime/inmatch/host_session.h>
#include <runtime/devtools/tick_profile.h>
#include <base/io/tick_rate.h>

#include <runtime/inmatch/server_session.h> // set_connection_mode / set_transport_mode / create_session / ...
#include <runtime/inmatch/server_spawn.h>   // Server_InitNewRoundState / Server_ProcessPendingPlayerSpawns
#include <runtime/inmatch/server_tick.h>    // Server_TickUpdate
#include <runtime/inmatch/session_timeout_config.h> // load_session_timeout_config (_NSTMOUT.TXT)

#include <runtime/replication/connection_fan.h> // set_entity_send_budget (the BANDWIDTH cap)
#include <net/npwire/ingame_decode.h> // OrganicSpawnBatch / OrganicSpawnRecord
#include <net/npwire/ingame_encode.h> // encode_organic_spawn_batch
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/outgoing_packets.h>
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_hello.h> // parse_disconnect_event (the staged H:0x03 record)
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

namespace opennova::inmatch {

namespace {

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

// Frame and send `messages` in as few packets as fit under the connection's
// ceiling, at most `max_packets` of them (what remains of this build's cs_dir0
// field-14 budget). Returns the nodes left queued — a failed frame's tail, or
// what the budget left (a split message's remaining pieces first) — for the
// head of the connection's queue.
std::vector<ProtocolMessage> send_session_batches(
		HostOwner &owner, opennova::IDatagramSocket &sock,
		NapiNPConnection &connection, std::vector<ProtocolMessage> messages,
		std::size_t max_packets) {
	if (max_packets == 0) return messages;
	// The shared BuildOutgoingPackets planner admits and packs the queue under the connection's
	// ceiling: cs_dir0 field 13, the host's mpmaxpacketsize as the new-connection callback
	// negotiated it against the joiner's MPS (D-NET-234), floored at 26. A server-side
	// connection's refused node is DROPPED: the MSGCRE record {1, 4, count, max, "", 0,
	// "NP.C:MSGCRE"} latches (first cause wins) and pending_disconnect marks the node for the
	// next protocol pump, which sends the 0x86 burst and destroys it; every later non-exempt
	// node in this boundary fails the same way, exempt ones still ship. Retail never defers a
	// fragment group for capacity: a FIRST that fit is built and sent while its over-cap MID
	// fails and tears the connection down.
	// [orig: BuildOutgoingPackets @0x628436, floor @0x628446; HandleNewConnection @0x4c81ca;
	//  NapiNPMessage_Create @0x627FC0 — RequestDisconnect @0x628112 (state 1 ->
	//  pending_disconnect @0x61e107); the destroy is NapiNPProtocol_Pump @0x62a6fd after the
	//  per-connection send pump @0x62a6f8]
	OutgoingPacketPlan plan = plan_outgoing_packets(connection.seq, messages,
			packet_ceiling_bytes(connection.timeouts.max_packet_bytes), max_packets,
			OutgoingOverflow::DropRefused);
	// The latches, in queue order. A staged H:0x03 connection description is retail's
	// SendChatMessage: the record is stored on the connection (only while none is latched) and
	// THEN queued through Create with the 0x10 exemption, so in queue order it sits after every
	// earlier node's overflow. This port defers Create to the send boundary, so the latch rides
	// the record's place in the queue and keeps that first-cause-wins order; the 0x46 that
	// answers the punt then makes the 0x86 burst echo this record. [orig:
	//  CNapiNPConnection_SendChatMessage @0x4C7EF0 — the record @0x4c7f8e..0x4c7fe4,
	//  latch-if-!valid @0x4c7ff2..0x4c8004, TrySendSessionInit @0x4c800c ->
	//  NapiNPDataTransfer_SendDescription @0x628C80 serializes conn->disconnect_event and Creates
	//  it with flag 0x10 @0x628e42; NapiNPMessage_Create's latch @0x6280f9..0x62810a]
	for (std::size_t m = 0; m < messages.size(); ++m) {
		const ProtocolMessage &message = messages[m];
		if (message.capacity_exempt &&
		    message.full_tag == PROTOCOL_TAG_CONNECTION_DESCRIPTION) {
			DisconnectEvent description;
			if (parse_disconnect_event(message.payload.data(), message.payload.size(),
			                           description))
				latch_disconnect_event(connection, description);
		}
		if (plan.overflow && m == plan.overflow_message) {
			latch_disconnect_event(connection, make_disconnect_event(
					1, 4, plan.overflow_count,
					static_cast<uint32_t>(connection.seq.outbound_message_limit),
					"", 0, "NP.C:MSGCRE"));
			connection.pending_disconnect = true;
		}
	}
	for (std::size_t p = 0; p < plan.packets.size(); ++p) {
		std::vector<uint8_t> datagram;
		if (!frame_in_match_s2c_batch(owner.ctx, connection.peer, plan.packets[p], datagram)) {
			// A packet that fails to frame keeps its records, and everything after them, queued.
			std::vector<ProtocolMessage> rest;
			for (std::size_t q = p; q < plan.packets.size(); ++q)
				for (ProtocolMessage &record : plan.packets[q]) rest.push_back(std::move(record));
			for (ProtocolMessage &record : plan.unbuilt) rest.push_back(std::move(record));
			return rest;
		}
		sock.send_to(connection.peer, datagram.data(), datagram.size());
		connection.last_session_send_tick = owner.now_tick;
	}
	// The build stops once its packet budget went out; the rest waits for the next build.
	// [orig: BuildOutgoingPackets @0x62860b..0x628619]
	return std::move(plan.unbuilt);
}

bool is_established_s2c_datagram(const std::vector<uint8_t> &datagram) {
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	if (!nw_decode_inbound(
				datagram.data(), datagram.size(), opcode, body))
		return false;
	return opcode == SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE;
}

void send_or_stage_established_datagram(
		HostOwner &owner, opennova::IDatagramSocket &sock, const PeerAddr &peer,
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

void admit_peer(HostOwner &owner, const PeerAddr &peer) {
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
				std::make_unique<replication::UdpSessionTransport>(replication::UdpSessionTransport::Role::Host);
	}
	if (conn->link.transport == nullptr) {
		conn->link.transport = link.transport.get();
		conn->link.mode = replication::TransportMode::Client;
	}

	if (link.announced || !conn->link.owned_entity.valid()) return; // wait for the spawn pipeline

	// The joiner's own pool-0 spawn record now ships IN-PHASE via build_pool0_organic_batch (the
	// initial-state world stream: 0x10 -> 0x0D -> 0x0C -> 0x20), which already carries its name, dcb
	// (entity+0x78), net_id, playerClass and per-recipient minimap_flags. A same-map retail↔retail
	// ASH_I5A capture (2026-07-01) shows the host sends each player's 0x0C EXACTLY ONCE, in that phase
	// order — NOT an early out-of-band 0x0C. The prior early send here was a duplicate that put a 0x0C on
	// the wire right after the first static batch, diverging from retail's load order (load-sequence diff
	// 2026-07-01). Latch announced so the pipeline proceeds; the in-phase stream is the single source.
	link.announced = true;
}

void dispatch_event(HostOwner &owner, const PeerAddr &peer, const HostAcceptEvent &ev) {
	switch (ev.kind) {
	case HostAcceptEvent::Kind::PeerHandshakeAdvanced:
		// Attach the semantic transport as soon as 0x42 establishes the
		// remote, before the initial-state producer starts queuing records.
		admit_peer(owner, peer);
		break;
	case HostAcceptEvent::Kind::PeerEnteredWorldStreaming:
	case HostAcceptEvent::Kind::PeerSpawned:
		admit_peer(owner, peer);
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

static void flush_s2c_boundaries(HostOwner &owner, opennova::IDatagramSocket &sock, uint32_t now,
		bool force_open);

// The pump's step (1): the receive drain with its handlers, the 0x84 tail,
// and each established remote's send-boundary clock.
static void receive_and_advance_boundaries(HostOwner &owner, opennova::IDatagramSocket &sock,
		uint32_t now, HostEventObserverFn event_observer, void *event_observer_context) {
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
			dispatch_event(owner, peer, ev);
			if (event_observer != nullptr)
				event_observer(event_observer_context, ev);
		}
		// A 0x44's rebuilt packets leave from inside the receive pump, at this datagram's
		// position, whatever the connection's S2C send boundary says (D-NET-229).
		// [orig: NapiNP_HandleResendList -> SendSessionPacket @0x6239b6 -> SendTo @0x61f039]
		for (const std::vector<uint8_t> &dg : r.immediate_outbound)
			sock.send_to(peer, dg.data(), dg.size());
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
			dispatch_event(owner, peer, ev);
			if (event_observer != nullptr)
				event_observer(event_observer_context, ev);
		}
	}
	// The receive pump's per-connection tail: resolve the missing-sequence latch only after the
	// receive FIFO is empty (a later datagram in this same drain may have closed the gap), and
	// send the 0x84 at once — the server receive pump runs every Server_TickUpdate, outside the
	// S2C send boundary, ahead of PumpFlags' 0x10 holdoff decrement below (D-NET-229).
	// [orig: Server_TickUpdate -> CNapiNetwork_PumpServerProtocolRecv @0x51d895 (flags 0x19) ->
	//  NapiNPProtocol_Pump @0x62a6ac -> PumpRecvQueues tail @0x6269bb..0x6269d6 ->
	//  SendMissingSeqList(conn, 0) @0x6269ce; then CNapiNPConnection_PumpFlags @0x62a6f8, the
	//  0x10 decrement @0x62979a..0x6297ac]
	for (TickOut &t : flush_server_missing_requests(owner.ctx)) {
		for (const std::vector<uint8_t> &dg : t.outbound)
			sock.send_to(t.peer, dg.data(), dg.size());
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
}

void host_session_pump(HostOwner &owner, opennova::IDatagramSocket &sock,
		HostBeforeServerTickFn before_server_tick, void *before_server_tick_context,
		HostEventObserverFn event_observer, void *event_observer_context) {
	devtools::TickProfile *profile =
			owner.ctx.world != nullptr ? owner.ctx.world->profile : nullptr;
	const devtools::ProfileScope pump_scope(profile, devtools::Slot::SIM_HOST_PUMP);
	devtools::ProfileLap lap(profile);
	const uint32_t now = owner.now_tick;
	// S2C 0x58 reports elapsed session milliseconds, while gameplay producers
	// consume a 62.5 Hz logical tick. Keep those clock domains explicit: retail
	// computes GetTickCount - host_start_tick; this deterministic host derives
	// equivalent elapsed time from its fixed simulation clock. The tick is the
	// 16 ms drain quantum, so the clock advances exactly 1000 ms per real second
	// (dividing by the integer 62 ran it 0.8 % fast and fired the 360 s
	// deploy-idle punt at ~357 s). [orig: Game_MainLoop @0x52B630 16 ms quantum;
	// Server_TickUpdate @0x51E109 GetTickCount delta vs 0x57E40]
	owner.ctx.np_protocol.host_run_duration_ms = static_cast<uint32_t>(
			static_cast<uint64_t>(now) * uint64_t(io::kTickMs));
	auto &pending_session_messages = owner.pending_session_messages;

	// (1) recv-drain, the 0x84 tail and the send-boundary clocks.
	receive_and_advance_boundaries(owner, sock, now, event_observer, event_observer_context);
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
			dispatch_event(owner, t.peer, ev);
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
	flush_s2c_boundaries(owner, sock, now, /*force_open=*/false);

	// (5) A dedicated host registers no type-2 local client. If its embedder nevertheless supplied a
	// loopback channel, defensively drain it so that unused input cannot accumulate. A serve-and-play
	// owner instead folds the registered loopback into ClientState after this pump, so preserve it.
	if (owner.host_loopback != nullptr && !owner.serve_and_play) {
		replication::Datagram discard;
		while (owner.host_loopback->client_recv(discard)) { /* discard the host's own view */ }
	}

	++owner.now_tick;
	lap.mark(devtools::Slot::SIM_HOST_SEND);
}

void host_session_flush_s2c(HostOwner &owner, opennova::IDatagramSocket &sock) {
	flush_s2c_boundaries(owner, sock, owner.now_tick, /*force_open=*/true);
}

void host_session_load_pump(HostOwner &owner, opennova::IDatagramSocket &sock) {
	if (owner.ctx.is_authority == 0 || owner.ctx.is_in_session == 0) return;
	receive_and_advance_boundaries(owner, sock, owner.now_tick, nullptr, nullptr);
	flush_s2c_boundaries(owner, sock, owner.now_tick, /*force_open=*/false);
}

// BuildOutgoingPackets over this connection's queue: the packets framed
// before this build (they own the lower sequence numbers) leave first, then
// every queued semantic record, split at the connection's ceiling. Returns
// whether a staged packet left; the caller compares sequences for the rest.
// One build sends at most cs_dir0 field 14's packets (the template's -1 is
// unbounded); the pre-framed packets count first, and whatever the budget
// leaves stays queued, in order, for the next build.
// [orig: BuildOutgoingPackets @0x62844e, @0x62860b..0x628619]
static bool build_s2c_queue(HostOwner &owner, opennova::IDatagramSocket &sock,
		NapiNPConnection &c, uint32_t now) {
	auto &pending_session_messages = owner.pending_session_messages;
	std::size_t budget = max_packets_per_build(c.timeouts.max_packets_per_tick);
	bool released = false;
	auto pending_datagrams = owner.pending_session_datagrams.find(c.peer);
	if (pending_datagrams != owner.pending_session_datagrams.end()) {
		std::vector<std::vector<uint8_t>> &staged = pending_datagrams->second;
		std::size_t sent = 0;
		while (sent < staged.size() && sent < budget) {
			sock.send_to(c.peer, staged[sent].data(), staged[sent].size());
			++sent;
		}
		staged.erase(staged.begin(), staged.begin() + static_cast<std::ptrdiff_t>(sent));
		if (staged.empty()) owner.pending_session_datagrams.erase(pending_datagrams);
		budget -= sent;
		c.last_session_send_tick = now;
		released = true;
	}
	std::vector<ProtocolMessage> messages;
	auto pending = pending_session_messages.find(c.peer);
	if (pending != pending_session_messages.end()) {
		messages = std::move(pending->second);
		pending_session_messages.erase(pending);
	}
	std::vector<ProtocolMessage> retry =
			send_session_batches(owner, sock, c, std::move(messages), budget);
	if (!retry.empty())
		pending_session_messages[c.peer] = std::move(retry);
	return released;
}

// One header-only packet carrying the current ACK (BuildOutgoingPackets with
// has_pending_out set and no record queued @0x62847e).
static void send_header_only(HostOwner &owner, opennova::IDatagramSocket &sock,
		NapiNPConnection &c, uint32_t now) {
	std::vector<uint8_t> packet;
	if (frame_in_match_s2c_batch(owner.ctx, c.peer, {}, packet)) {
		sock.send_to(c.peer, packet.data(), packet.size());
		c.last_session_send_tick = now;
	}
}

// PumpSendIntervals, the connected server node's arm of PumpStateMachine,
// which the server send pump runs on EVERY Server_TickUpdate (flags 0x2E1, no
// holdoff test), open boundary or not (D-NET-256). Both legs measure the time
// since this connection last built a packet (+0x640, which BuildOutgoingPackets
// and the legs stamp) against its cs_dir0 fields, a negative one disabling:
//   ACTIVE (field 5): records are retained and MORE than the interval passed;
//   EMPTY (field 4): nothing is retained, no out-of-order C2S packet is held,
//   and MORE than the interval passed — the keepalive a stock client's 120 s
//   reap requires (D-NET-173, D-NET-236).
// A due leg forces has_pending_out and builds: every queued record leaves (a
// closed window's queue included), or one header-only packet; the countdown is
// not touched. The leg clock arms on the first tick it is read (0 = unarmed).
// The JO template's intervals are 10000 / 30000 ms (CNapiNetwork_Init
// @0x4caac5/@0x4cab98 and @0x4caab5/@0x4cab88); 1000 ms is the NOVAWORLDUDP
// service template's active value @0x4d3e60, not the game session's.
// [orig: CNapiNPConnection_PumpSendIntervals @0x628FD0 — the active leg
//  @0x628ff1..0x629017 (`sub ebp,[esi+640h]; cmp; jbe` @0x62900c..0x629015), the
//  empty leg @0x629041..0x629067 (`cmp [esi+7A8h], 0` @0x629053), the build
//  has_pending_out @0x62906f -> BuildOutgoingPackets @0x62907b -> PrunePacketQueue
//  @0x629082, the stamp @0x629089; BuildOutgoingPackets' own stamp @0x628605;
//  PumpStateMachine case 1 @0x62933f; PumpFlags 0x40 @0x6297c8;
//  CNapiNetwork_PumpServerProtocolSend @0x4c4f11 called @0x51e487]
static bool pump_send_interval_legs(HostOwner &owner, opennova::IDatagramSocket &sock,
		NapiNPConnection &c, uint32_t now) {
	if (c.server_scrk.empty()) return false;
	if (c.last_session_send_tick == 0) {
		c.last_session_send_tick = now;
		return false;
	}
	const uint64_t since_ms =
			static_cast<uint64_t>(now - c.last_session_send_tick) * uint64_t(io::kTickMs);
	const int32_t active_ms = c.timeouts.active_send_interval_ms;
	const int32_t empty_ms = c.timeouts.idle_send_interval_ms;
	const bool retained = c.seq.retained_outbound_message_count > 0;
	const bool active_due =
			active_ms >= 0 && retained && since_ms > static_cast<uint64_t>(active_ms);
	const bool empty_due = !active_due && empty_ms >= 0 && !retained &&
			c.seq.queued_inbound.empty() && since_ms > static_cast<uint64_t>(empty_ms);
	if (!active_due && !empty_due) return false;
	const uint32_t sequence_before = c.seq.next_outbound_seq;
	const bool released = build_s2c_queue(owner, sock, c, now);
	if (!released && c.seq.next_outbound_seq == sequence_before)
		send_header_only(owner, sock, c, now);
	c.last_session_send_tick = now;
	return true;
}

// The pump's step (4). Drain each remote transport into the ordered pending
// queue every tick; only an open boundary frames and sends it (force_open
// ships regardless — the teardown's last flush), then the send-interval legs
// run whatever the boundary says. The host's own type-2 loopback is consumed
// in-process and skipped here.
static void flush_s2c_boundaries(HostOwner &owner, opennova::IDatagramSocket &sock, uint32_t now,
		bool force_open) {
	auto &pending_session_messages = owner.pending_session_messages;
	for (NapiNPConnection &c : owner.ctx.np_protocol.connection_list) {
		if (c.type != NapiNPConnection::kTypeServerSide) continue;
		// A type-1 remote peer's transport is always the UdpSessionTransport admit_peer attached, so the
		// downcast to reach pop_outbound (an owner-boundary method, not on the base ISessionTransport) is
		// safe — the host's own type-2 loopback (a LoopbackChannel) is skipped above.
		if (c.link.transport != nullptr) {
			auto *udp = static_cast<replication::UdpSessionTransport *>(c.link.transport);
			replication::Datagram staged;
			while (udp->pop_outbound(staged)) {
				ProtocolMessage message = make_protocol_message(
						staged.tag, std::move(staged.body),
						staged.protocol_flags_raw);
				message.reliable = staged.reliable;
				message.capacity_exempt = staged.capacity_exempt;
				message.retention_flushes = staged.retention_flushes;
				pending_session_messages[c.peer].push_back(std::move(message));
			}
		}
		// Whether this tick built anything: retail prunes only after a build
		// (PumpEnumeratorAndSend builds and prunes when messages are pending or an
		// ACK is owed @0x6292a9..0x6292bb; the send-interval legs build and prune
		// themselves @0x62907b..0x629082).
		const uint32_t sequence_before = c.seq.next_outbound_seq;
		bool built = false;
		const bool open = c.s2c_send_boundary_open || force_open;
		if (open) {
			built = build_s2c_queue(owner, sock, c, now);
			// The owed ACK (D-NET-233): a C2S packet with records arrived since the last
			// build, and nothing above built a packet, so the boundary builds a header-only
			// one carrying the current ACK.
			// [orig: PumpEnumeratorAndSend `queued > 0 || has_pending_out` @0x6292a9 ->
			//  BuildOutgoingPackets @0x6292b4 (one packet even with no records,
			//  @0x62847e); the clear once the queue drained @0x628629]
			if (c.session_ack_owed && !built && c.seq.next_outbound_seq == sequence_before)
				send_header_only(owner, sock, c, now);
		}
		// PumpFlags runs 0x40 (PumpStateMachine -> the legs) after the 0x20 build.
		if (pump_send_interval_legs(owner, sock, c, now)) built = true;
		// One build can contain preframed settings/resends, several MTU-split
		// semantic packets, or no payload at all. Retail prunes the finite message
		// nodes once, after all of them, at the counter they were built with.
		// [orig: PrunePacketQueue @0x6292bb / @0x629082]
		if (built || c.seq.next_outbound_seq != sequence_before) {
			prune_session_send_boundary(c.seq);
			c.session_ack_owed = false;
		}
		if (open) {
			if (c.s2c_send_holdoff_dictated) {
				c.s2c_send_holdoff_countdown = c.s2c_send_holdoff_ticks;
				c.s2c_send_boundary_open = c.s2c_send_holdoff_ticks == 0;
			} else {
				c.s2c_send_holdoff_countdown = 0;
				c.s2c_send_boundary_open = true;
			}
		}
		// The host's send pump runs every Server_TickUpdate, open boundary or not, and
		// its 0x80 leg advances +0x64C on every call: a host ages its finite-lifetime
		// records (the 0x57 RTT echo's 62, the chat's 310) per server tick, where a
		// joiner, whose send pump sits behind the client send gate @0x42c3dd, ages
		// them per open boundary (D-NET-230).
		// [orig: Server_TickUpdate -> CNapiNetwork_PumpServerProtocolSend @0x51e487
		//  (flags 0x2E1, no holdoff test) -> CNapiNPConnection_PumpFlags increment
		//  @0x6297d5, after the 0x20 build @0x6297b7 and before the 0x200 reload
		//  @0x6297f3]
		advance_session_send_flush_counter(c.seq);
	}
}

namespace {

// The session's rules onto its World: the match, the spawn waves and the
// rule words every in-match reader tests. A fresh session's and the map
// change's World alike.
void configure_session_world(HostOwner &owner, const HostConfig &cfg) {
	if (owner.ctx.world == nullptr) return;
	world::MatchRules match_rules;
	match_rules.game_type = owner.ctx.config.game_type;
	// SET GameTime feeds g_RespawnTime in retail. The existing host model
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
	owner.ctx.world->zones.spawn_waves.build_from_mission(
			*owner.ctx.world, owner.ctx.config.spawn_wave_time_base,
			owner.ctx.config.spawn_wave_time_zone);
	owner.ctx.world->rules.mp_session =
			cfg.socket_mode != SocketMode::Socketless;
	owner.ctx.world->rules.destroy_buildings =
			owner.ctx.config.destroy_buildings != 0;
	// [orig: dword_24D1E34 & 0x8000, "TeamTriggerClaymore" admin set @ 0x405f16]
	owner.ctx.world->throwables.team_trigger_claymore =
			(owner.ctx.config.mp_attributes & GameConfig::kMpAttribClaymorePref) != 0;
	// The MP NoTracers rule: bit 0 of the same rules word kills the tracer
	// visual at round spawn unless the ammo is FORCETRACER; the lobby
	// publishes its inverse as the "Tracers" key [orig: g_RulesFlags
	// @ 0x24D1E34 & 1 at RoundData_SpawnRound @ 0x4ec41f; admin set
	// @ 0x405c80; Lobby_UpdateServerInfo "Tracers" @ 0x4fee4f]
	owner.ctx.world->round_sim.no_tracers_rule =
			(owner.ctx.config.mp_attributes & GameConfig::kMpAttribNoTracers) != 0;
}

} // namespace

CreateSessionResult start_host_session(HostOwner &owner, const HostConfig &cfg) {
	owner.now_tick = 0;
	// The process's log devices and the socket address, ahead of the session
	// create whose server start logs HOST STARTED and whose round init clears
	// the punt table.
	owner.ctx.logs = cfg.logs;
	owner.ctx.local_address = cfg.local_address;
	owner.ctx.local_address_known = cfg.local_address_known;
	// The ban files' directory, ahead of the session create's round init, which loads them
	// [orig: CNapiGameSession_BuildAndCreateSession @0x5695B3 -> Server_InitNewRoundState
	//  @0x51C92F (banlist.txt), @0x51CB3B (banned.txt)].
	owner.ctx.bans.directory = cfg.ban_directory;
	owner.serve_and_play = cfg.serve_and_play; // the pump's step-5 loopback handling reads this
	owner.pending_session_messages.clear();
	owner.pending_session_datagrams.clear();
	// Apply the configured 0x0A byte cap to the replication global (the retail
	// BANDWIDTH command's target [orig: g_EntitySendBudget @0xC8FC50]).
	replication::set_entity_send_budget(
			static_cast<int>(cfg.config.entity_send_budget));
	// The network type the host runs under: the NovaWorld menu's stays through a NovaWorld host.
	owner.ctx.transport_mode = cfg.network_type;
	// Select the witnessed §5.0 table row: serve-and-play is mode 3 (host + local client);
	// dedicated/headless is mode 1 (host only, no dcb-2 loopback player).
	set_connection_mode(
			owner.ctx,
			cfg.serve_and_play ? ConnectionMode::HostClient : ConnectionMode::HostOnly);
	set_transport_mode(owner.ctx, cfg.socket_mode);
	const SessionStartup startup = make_session_startup(cfg);
	// The create also runs Server_InitNewRoundState. A set mpreset word ends
	// the process there, so nothing of the session follows it
	// [orig: CNapiGameSession_CreateSession @0x4C97E7..0x4C97F0].
	const CreateSessionResult created = create_session(
			owner.ctx, cfg.config, startup,
			cfg.serve_and_play ? owner.host_loopback : nullptr);
	if (created == CreateSessionResult::ProcessExit) return created;
	// The connection template every server-side node is created with and the 0x82
	// advertises: 120000 ms / 1200 records, or the game directory's loose
	// `_NSTMOUT.TXT` override [orig: CNapiNetwork_Init @0x4ca9d7..0x4caa4b, stores
	//  @0x4caa81/@0x4cab20/@0x4cab54/@0x4cabf0].
	owner.ctx.np_protocol.connection_template = load_session_timeout_config(cfg.game_root);
	// The type-2 loopback is the host's own client: it never uploads ClientAuth CU
	// vars, so its per-side character selection is installed here — before
	// Server_ProcessPendingPlayerSpawns stamps the local player from it.
	for (NapiNPConnection &connection : owner.ctx.np_protocol.connection_list) {
		if (connection.type == NapiNPConnection::kTypeClientSide) {
			connection.char_vars = cfg.local_character_vars;
			// The round init's copy of the host's own profile word into its
			// slot +372 [orig: Server_InitNewRoundState @0x51ca55..0x51ca61].
			connection.link.auto_medic_enabled = cfg.local_auto_medic_disabled == 0;
		}
	}
	configure_session_world(owner, cfg);
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
	return CreateSessionResult::Created;
}

void continue_host_session(HostOwner &owner, const HostConfig &cfg) {
	// The same 0x0A byte cap, re-applied from the session config the host
	// screen or the host file still names.
	replication::set_entity_send_budget(static_cast<int>(cfg.config.entity_send_budget));
	continue_session(owner.ctx, cfg.config);
	configure_session_world(owner, cfg);
}

} // namespace opennova::inmatch
