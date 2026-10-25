// The NapiNPConnection factory the npruntime tests share: one connection in
// the phase a test wants (New, or InMatch with its owned entity announced),
// bound to a transport. Construction contract in one place, so a change to
// NapiNPConnection's link/burst/phase fields is edited once. Beside it, the
// joiner tests' two session-wire helpers: a host's framed S2C 0x83 and a
// joiner's C2S 0x43 decoded back.
#pragma once

#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/server_tick.h>       // inmatch::Server_RerollPlayerTickSeed
#include <runtime/replication/connection.h>        // replication::TransportMode
#include <runtime/inmatch/session_transport.h> // replication::ISessionTransport
#include <runtime/world/entity.h>         // world::EntityHandle

#include <net/npwire/nw_session_framing.h> // nw_encode_outbound / nw_decode_inbound
#include <net/npwire/protocol_message.h>   // frame_session_packet / decode_protocol_packet_plaintext
#include <net/npwire/session_keys.h>       // SESSION_OPCODE_*

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace conn_fixture {

// One S2C 0x83 datagram as a host frames it for a joiner: `messages` under the
// host's sequencing, SCRK-encrypted with `server_scrk`, the header's session id
// the joiner's local key `client_key`. Empty when the framing fails.
inline std::vector<uint8_t> frame_server_session(opennova::SessionSequencing &seq,
		const std::string &server_scrk, uint32_t client_key,
		const std::vector<opennova::ProtocolMessage> &messages) {
	std::vector<uint8_t> body;
	if (!opennova::frame_session_packet(
			seq, opennova::SessionCrypto{server_scrk, {}, client_key}, messages, body)) {
		return {};
	}
	return opennova::nw_encode_outbound(
			opennova::SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(body));
}

// A joiner's C2S 0x43 datagram decoded with its `client_scrk`: false for any
// other opcode or an undecodable packet.
inline bool decode_client_session(const std::vector<uint8_t> &datagram,
		const std::string &client_scrk, opennova::ProtocolPacketHeader &header,
		std::vector<opennova::ProtocolMessage> &messages) {
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	return opennova::nw_decode_inbound(datagram.data(), datagram.size(), opcode, body) &&
			opcode == opennova::SESSION_OPCODE_PROTOCOL_MESSAGE &&
			opennova::decode_protocol_packet_plaintext(
					body.data(), body.size(), client_scrk, header, messages);
}

inline opennova::inmatch::NapiNPConnection make_conn(uint32_t id, int type,
                                                opennova::replication::ISessionTransport *t,
                                                opennova::replication::TransportMode mode,
                                                opennova::world::EntityHandle owned, bool spawned) {
	opennova::inmatch::NapiNPConnection c;
	c.connection_id = id;
	c.type = type;
	c.link.transport = t;
	c.link.mode = mode;
	c.link.owned_entity = owned;
	c.burst.spawned = spawned;
	c.spawned_announced = spawned;
	c.phase = spawned ? opennova::inmatch::ConnectionPhase::InMatch : opennova::inmatch::ConnectionPhase::New;
	// An in-match connection's slot is joined (state 6).
	if (spawned) c.slot_state = opennova::inmatch::PlayerSlotState::Joined;
	return c;
}

// make_conn, with a spawned connection's tick seed re-rolled the way join and
// deployment arm it (Server_RerollPlayerTickSeed), at a session's first server
// frame clock, 1 (world::RoundRing::frame_clock's start).
inline opennova::inmatch::NapiNPConnection make_seeded_conn(uint32_t id, int type,
                                                opennova::replication::ISessionTransport *t,
                                                opennova::replication::TransportMode mode,
                                                opennova::world::EntityHandle owned, bool spawned) {
	opennova::inmatch::NapiNPConnection c = make_conn(id, type, t, mode, owned, spawned);
	if (spawned) (void)opennova::inmatch::Server_RerollPlayerTickSeed(c, 1u);
	return c;
}

} // namespace conn_fixture
