// D-NET-256 (Q7-9 (a)): the host's send-interval legs and its receive reap measure the host's
// millisecond clock, GetTickCount's, not its tick. Retail's PumpSendIntervals compares
// GetTickCount with the connection's last-build stamp (+0x640) and PumpStateMachine's reap
// compares it with the last in-order receive's stamp (+0x5E8), and both run in every send /
// receive pump, the mission load's in-load pumps included. So a long load, which runs no tick,
// still fires the EMPTY keepalive (30000 ms) or the ACTIVE probe (10000 ms) at a load pump once
// the interval has passed, and the receive reap counts the load's time.
// Pinned here over an injected clock (HostConfig::clock_ms's seam, set on the owner directly):
//   - quiet: no load pump sends before 30000 ms have passed on the clock; the first load pump
//     past it sends the one header-only keepalive, with no tick run;
//   - retained records: the load pump past 10000 ms sends the probe;
//   - the reap: one pump after 120001 ms of clock time (a long load) latches the receive
//     timeout ("NP.C:PT:SERTMOUT") and destroys the node silent since its last receive; a node
//     heard during the load (its ping at a load pump stamps it) is charged only the time since
//     that receive, so the first walk after the load keeps it.
// [orig: CNapiNPConnection_PumpSendIntervals @0x628FD0 - the active leg @0x628FF1..0x629017
//  (`sub ebp,[esi+640h]; cmp; jbe` @0x62900C..0x629015), the empty leg @0x629041..0x629067;
//  CNapiNPConnection_PumpStateMachine @0x6292E0 state 1 - elapsed = now - conn+0x5E8
//  (`sub esi, [ebp+5E8h]` @0x62935C; the stamps CNapiNPConnection_ParseMessages @0x625D54 and
//  Nwu_HandlePing @0x623C56)
//  `elapsed > timeout_ms` @0x629362, "NP.C:PT:SERTMOUT" @0x6293B7..0x6293E6;
//  Server_PumpNetworkTransport @0x4FD960 (the send pump's call @0x4FD98B), from
//  Game_StartMission's in-load calls]
#include <net/npwire/idatagram_socket.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/peer_addr.h>
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_keys.h>
#include <net/npwire/session_ping.h>
#include <runtime/inmatch/host_session.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_session.h>
#include <runtime/inmatch/udp_session_transport.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <vector>

namespace {

namespace inmatch = opennova::inmatch;

int g_failures = 0;
#define CHECK(c)                                                                     \
	do {                                                                             \
		if (!(c)) {                                                                  \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                 \
			++g_failures;                                                            \
		}                                                                            \
	} while (0)

const opennova::PeerAddr kPeer{0x0100007Fu, 33216};

struct CaptureSocket final : opennova::IDatagramSocket {
	std::vector<std::vector<uint8_t>> sent;
	std::deque<std::vector<uint8_t>> inbound; // from kPeer
	int recv_from(uint8_t *buf, std::size_t cap, opennova::PeerAddr &from) override {
		if (inbound.empty()) return 0;
		const std::vector<uint8_t> d = std::move(inbound.front());
		inbound.pop_front();
		const std::size_t n = std::min(cap, d.size());
		std::copy(d.begin(), d.begin() + static_cast<std::ptrdiff_t>(n), buf);
		from = kPeer;
		return static_cast<int>(n);
	}
	void send_to(const opennova::PeerAddr &, const uint8_t *data, std::size_t len) override {
		sent.emplace_back(data, data + len);
	}
};

// A Serve Only host with one in-match remote, its clock injected.
struct ClockHost {
	inmatch::HostOwner owner;
	uint32_t clock = 5000;

	ClockHost() {
		inmatch::set_connection_mode(owner.ctx, inmatch::ConnectionMode::HostOnly);
		inmatch::set_transport_mode(owner.ctx, inmatch::SocketMode::Lan);
		inmatch::GameConfig config;
		inmatch::create_session(owner.ctx, config, inmatch::SessionStartup{}, nullptr);
		owner.clock_ms = [this] { return clock; };
		auto &link = owner.peers[kPeer];
		link.transport = std::make_unique<opennova::replication::UdpSessionTransport>(
				opennova::replication::UdpSessionTransport::Role::Host);
		inmatch::NapiNPConnection conn;
		conn.peer = kPeer;
		conn.type = inmatch::NapiNPConnection::kTypeServerSide;
		conn.phase = inmatch::ConnectionPhase::InMatch;
		conn.burst.spawned = true;
		conn.server_scrk = "SERVERCLOCKLEGSSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ012345";
		conn.client_ck = 0x10203040u;
		conn.link.transport = link.transport.get();
		conn.link.mode = opennova::replication::TransportMode::Client;
		conn.receive_stamp_ms = clock; // heard at the session's start
		owner.ctx.np_protocol.connection_list.push_back(std::move(conn));
	}
	inmatch::NapiNPConnection *remote() {
		for (inmatch::NapiNPConnection &c : owner.ctx.np_protocol.connection_list)
			if (c.peer == kPeer) return &c;
		return nullptr;
	}
};

bool header_only(const inmatch::NapiNPConnection &conn, const std::vector<uint8_t> &datagram) {
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	opennova::ProtocolPacketHeader header;
	std::vector<opennova::ProtocolMessage> messages;
	return opennova::nw_decode_inbound(datagram.data(), datagram.size(), opcode, body) &&
			opcode == opennova::SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE &&
			opennova::decode_protocol_packet_plaintext(
					body.data(), body.size(), conn.server_scrk, header, messages) &&
			messages.empty();
}

// A quiet connection through a long load: the load pumps run no tick; the clock alone fires
// the EMPTY keepalive, once.
void test_a_long_load_fires_the_empty_leg() {
	auto host = std::make_unique<ClockHost>();
	CaptureSocket socket;
	const uint32_t tick_before = host->owner.now_tick;
	inmatch::host_session_load_pump(host->owner, socket); // arms the leg clock
	host->clock += 30000;                                 // exactly the interval: not more
	inmatch::host_session_load_pump(host->owner, socket);
	CHECK(socket.sent.empty());
	host->clock += 1;
	inmatch::host_session_load_pump(host->owner, socket);
	CHECK(socket.sent.size() == 1 && header_only(*host->remote(), socket.sent[0]));
	inmatch::host_session_load_pump(host->owner, socket); // the build re-armed the clock
	CHECK(socket.sent.size() == 1);
	CHECK(host->owner.now_tick == tick_before); // no tick ran
}

// Retained records through a long load: the ACTIVE probe once 10000 ms have passed.
void test_a_long_load_fires_the_active_leg() {
	auto host = std::make_unique<ClockHost>();
	CaptureSocket socket;
	inmatch::host_session_load_pump(host->owner, socket); // arms the leg clock
	inmatch::NapiNPConnection &remote = *host->remote();
	remote.seq.retained_outbound[remote.seq.next_outbound_seq] = {
			opennova::make_protocol_message(0x49, {0x01, 0x00, 0x07, 0x10})};
	remote.seq.retained_outbound_message_count = 1;
	host->clock += 10000;
	inmatch::host_session_load_pump(host->owner, socket);
	CHECK(socket.sent.empty());
	host->clock += 1;
	inmatch::host_session_load_pump(host->owner, socket);
	CHECK(socket.sent.size() == 1);
}

// The joiner's 0x45 ClientPing (WR clear), keyed to the host's receiver-local key: it stamps the
// connection's receive clock like an in-order session packet.
std::vector<uint8_t> client_ping(const inmatch::NapiNPConnection &conn) {
	return opennova::nw_encode_outbound(opennova::SESSION_OPCODE_CLIENT_PING,
			opennova::build_session_ping_body(conn.server_sk, /*wants_reply=*/false, 0));
}

// The reap measures the clock against the connection's last receive stamp: a joiner heard during
// a long load (its ping handled at a load pump) is charged only the time since then at the first
// walk after the load, and is reaped only when its silence passes the timeout from that receive
// (`elapsed > timeout`: 120000 ms stays, 120001 ms reaps).
void test_a_joiner_heard_in_the_load_is_not_reaped() {
	auto host = std::make_unique<ClockHost>();
	CaptureSocket socket;
	inmatch::host_session_pump(host->owner, socket);
	CHECK(host->remote() != nullptr);
	host->clock += 120001;
	socket.inbound.push_back(client_ping(*host->remote()));
	inmatch::host_session_load_pump(host->owner, socket);
	CHECK(host->remote() != nullptr && host->remote()->receive_stamp_ms == host->clock);
	const uint32_t heard = host->clock;
	host->clock += 16;
	inmatch::host_session_pump(host->owner, socket);
	CHECK(host->remote() != nullptr && host->remote()->receive_inactive_ms == 16);
	host->clock = heard + 120000;
	inmatch::host_session_pump(host->owner, socket);
	CHECK(host->remote() != nullptr);
	host->clock = heard + 120001;
	inmatch::host_session_pump(host->owner, socket);
	CHECK(host->remote() == nullptr);
}

// The receive reap counts a long load: one pump after a 120001 ms load reaps a node silent since
// the session's start, where the tick clock would need 7500 pumps.
void test_the_reap_counts_a_long_load() {
	auto host = std::make_unique<ClockHost>();
	CaptureSocket socket;
	inmatch::host_session_pump(host->owner, socket);
	CHECK(host->remote() != nullptr);
	host->clock += 120001;
	inmatch::host_session_pump(host->owner, socket);
	CHECK(host->remote() == nullptr);
	// The reap's goodbye burst (the 0x86s) carries the receive-timeout record.
	CHECK(!socket.sent.empty());
}

} // namespace

int main() {
	test_a_long_load_fires_the_empty_leg();
	test_a_long_load_fires_the_active_leg();
	test_the_reap_counts_a_long_load();
	test_a_joiner_heard_in_the_load_is_not_reaped();
	if (g_failures != 0) {
		std::printf("host_clock_legs: %d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("host_clock_legs: ok\n");
	return 0;
}
