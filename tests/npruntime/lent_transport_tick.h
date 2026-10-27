#pragma once

// A host tick over a test host no HostOwner binds: each remote connection without a transport is
// lent one for the tick (the owner's role), and the records the tick queued on it come back
// framed, one record a session datagram, in that peer's TickOut, as the owner's send boundary
// would ship them. Production sends a connection's initial-state records through its transport
// alone (ship_burst_messages); this is the tests' stand-in for the owner that holds it.

#include <runtime/inmatch/napi_np_protocol.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/udp_session_transport.h>

#include <net/npwire/protocol_message.h>

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace lent_transport_tick {

inline std::vector<opennova::inmatch::TickOut> tick_connections(
		opennova::inmatch::NapiNPServerCtx &ctx, int elapsed_ms, uint32_t now_tick) {
	namespace inmatch = opennova::inmatch;
	namespace ns = opennova::replication;
	struct Lent {
		uint32_t connection_id;
		std::unique_ptr<ns::UdpSessionTransport> transport;
	};
	std::vector<Lent> lent;
	for (inmatch::NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (conn.link.transport != nullptr || conn.type == inmatch::NapiNPConnection::kTypeClientSide)
			continue;
		lent.push_back({conn.connection_id,
				std::make_unique<ns::UdpSessionTransport>(ns::UdpSessionTransport::Role::Host)});
		conn.link.transport = lent.back().transport.get();
	}
	std::vector<inmatch::TickOut> outs = inmatch::tick_connections(ctx, elapsed_ms, now_tick);
	for (Lent &l : lent) {
		for (inmatch::NapiNPConnection &conn : ctx.np_protocol.connection_list) {
			if (conn.connection_id != l.connection_id || conn.link.transport != l.transport.get())
				continue;
			inmatch::TickOut *to = nullptr;
			for (inmatch::TickOut &t : outs)
				if (t.peer == conn.peer) to = &t;
			if (to == nullptr) {
				outs.push_back(inmatch::TickOut{});
				outs.back().peer = conn.peer;
				to = &outs.back();
			}
			ns::Datagram record;
			while (l.transport->pop_outbound(record)) {
				opennova::ProtocolMessage message = record.protocol_flags_raw != 0
						? opennova::make_protocol_message(
								  record.tag, std::move(record.body), record.protocol_flags_raw)
						: opennova::make_protocol_message(record.tag, std::move(record.body));
				message.reliable = record.reliable;
				std::vector<uint8_t> datagram = inmatch::frame_session_replies(conn, {std::move(message)});
				if (!datagram.empty()) to->outbound.push_back(std::move(datagram));
			}
			conn.link.transport = nullptr;
		}
	}
	return outs;
}

} // namespace lent_transport_tick
