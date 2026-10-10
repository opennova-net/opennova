// The NOVAWORLDUDP lobby answers a stock client's 0x44 resend request.
//
// A stock client's NWU session is an ordinary type-2 NAPI connection
// [orig: CNapiGameSession_InitNPConnection @0x4d3be0]: it admits only the next
// in-order session packet and queues later ones [orig: NapiNPProtocol_HandleSessionPacket
// @0x626be0..0x626c3a], and while one is queued it asks for the gap with a 0x44 every
// active-send interval [orig: CNapiNPConnection_PumpSendIntervals @0x629032]. The
// server side of that exchange resends the listed sequences from its retained
// reliable records, treating 0 as "send the next one", and the request is not
// receive activity [orig: NapiNP_HandleResendList @0x623800, @0x62395f]. Without
// it one lost service->client packet strands every later reply in the client's
// queue (a ServerHostResult never lands: NWEC52). This drives the real listener
// over loopback UDP: verify a session, lose the reply to a host request, ask for
// it with a 0x44, and require the same records back under the same sequence.

#include "nw_udp_listener.h"
#include "server_config.h"

#include <net/napi/session.h>
#include <net/novaworld/client_session.h>
#include <net/novaworld/connection/manager.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_keys.h>

#include "net_sockets.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

void expect(bool cond, const char *what) {
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		++g_failures;
	}
}

opennova::net::Endpoint loopback_ep(uint16_t port) {
	opennova::net::Endpoint ep;
	ep.ip = {127, 0, 0, 1};
	ep.port = port;
	return ep;
}

// The next datagram with `opcode` within `timeout_ms`, its NWU-stripped body in `body`.
bool receive_opcode(opennova::net::Socket &sock, uint8_t opcode, std::vector<uint8_t> &body,
		int timeout_ms) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	uint8_t rx[4096];
	while (std::chrono::steady_clock::now() < deadline) {
		opennova::net::Endpoint from;
		const int n = opennova::net::udp_recv_from(sock, rx, sizeof rx, from, 50);
		if (n <= 0) continue;
		uint8_t op = 0;
		std::vector<uint8_t> inner;
		if (opennova::nw_decode_inbound(rx, static_cast<size_t>(n), op, inner) && op == opcode) {
			body = std::move(inner);
			return true;
		}
	}
	return false;
}

// The next 0x83 carrying records (header-only ACK packets skipped), decoded with `scrk`.
bool receive_records(opennova::net::Socket &sock, const std::string &scrk,
		opennova::ProtocolPacketHeader &hdr, std::vector<opennova::ProtocolMessage> &messages,
		int timeout_ms) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	while (std::chrono::steady_clock::now() < deadline) {
		std::vector<uint8_t> body;
		if (!receive_opcode(sock, opennova::SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, body, 50))
			continue;
		messages.clear();
		if (opennova::decode_protocol_packet_plaintext(body.data(), body.size(), scrk, hdr,
				messages) && !messages.empty())
			return true;
	}
	return false;
}

} // namespace

int main() {
	using namespace std::chrono_literals;
	if (opennova::net::startup() != 0) {
		std::fprintf(stderr, "FAIL: net::startup\n");
		return 1;
	}
	opennova::ConnectionManager manager;
	opennova::novaworld_server::NwUdpListener listener(manager);
	manager.on_lost([&listener](const opennova::Connection &connection, opennova::DropReason reason) {
		listener.erase_lobby_state(connection.addr, opennova::drop_reason_name(reason));
	});
	opennova::novaworld_server::ServerConfig config;
	config.nw_udp_port = 0;
	if (!listener.start(config) || listener.bound_port() == 0) {
		std::fprintf(stderr, "FAIL: listener.start\n");
		return 1;
	}

	opennova::ClientSession::Config cfg;
	cfg.client_index = 0x52455345u;
	cfg.client_key = 0x4E444C53u;
	cfg.na = "resend:lobby";
	cfg.cookie_vars = []() {
		return std::vector<std::pair<std::string, std::string>>{{"NWUID", ""}};
	};
	opennova::ClientSession session(cfg);
	uint16_t client_port = 0;
	auto client = opennova::net::udp_bind(0, &client_port);
	expect(client.is_valid(), "client UDP socket bound");
	const auto server_ep = loopback_ep(listener.bound_port());
	auto send = [&](const std::vector<uint8_t> &dg) {
		if (!dg.empty()) opennova::net::udp_send_to(client, server_ep, dg.data(), dg.size());
	};

	send(session.start());
	uint8_t rx[4096];
	for (int i = 0; i < 40 && !session.is_verified(); ++i) {
		opennova::net::Endpoint from;
		const int n = opennova::net::udp_recv_from(client, rx, sizeof rx, from, 200);
		std::vector<std::vector<uint8_t>> out;
		if (n > 0 && !session.handle_datagram(rx, static_cast<size_t>(n), out)) break;
		session.finish_receive_batch(out);
		session.pump(out);
		session.process_periodic_update();
		for (const auto &dg : out) send(dg);
	}
	expect(session.is_verified(), "the session verifies against the real listener");

	if (session.is_verified()) {
		// A host request whose reply is "lost": read it off the socket but never feed it to
		// the session.
		auto host_req = opennova::make_client_host_request(
				0, {{0, "NWUID", session.server_nwuid()}},
				{{0, "LobbyName", "jop_2_consumer"}, {0, "MaxPlayers", "8"}},
				{{0, "ServerName", "Resend Host"}, {0, "Players", "1"}},
				{{0, "PlayerName", "Host"}});
		session.queue_statement(host_req);
		std::vector<std::vector<uint8_t>> request;
		session.pump(request);
		for (const auto &dg : request) send(dg);
		opennova::ProtocolPacketHeader lost_hdr;
		std::vector<opennova::ProtocolMessage> lost_messages;
		expect(receive_records(client, session.server_scrk(), lost_hdr, lost_messages, 1000),
		       "the host request draws a 0x83 carrying the ServerHostResult records");

		// The stock client's 0x44: [service local key][the missing sequence].
		std::vector<uint8_t> resend_body;
		expect(opennova::encode_session_resend_list(session.server_key(), {lost_hdr.seq_num},
		               resend_body),
		       "the 0x44 body encodes");
		send(opennova::nw_encode_outbound(opennova::SESSION_OPCODE_CLIENT_RESEND_LIST,
				resend_body));
		opennova::ProtocolPacketHeader resent_hdr;
		std::vector<opennova::ProtocolMessage> resent_messages;
		const bool got = receive_records(client, session.server_scrk(), resent_hdr,
				resent_messages, 1000);
		expect(got, "the service answers the 0x44 with a 0x83 carrying the records");
		if (got) {
			expect(resent_hdr.seq_num == lost_hdr.seq_num,
			       "the resend reuses the lost packet's sequence");
			expect(resent_messages.size() == lost_messages.size() &&
			               !resent_messages.empty() &&
			               resent_messages.front().payload == lost_messages.front().payload,
			       "the resend carries the same retained records");
		}
		// A 0x44 under another key is not this connection's and draws nothing.
		std::vector<uint8_t> foreign_body;
		opennova::encode_session_resend_list(session.server_key() ^ 1u, {lost_hdr.seq_num},
				foreign_body);
		send(opennova::nw_encode_outbound(opennova::SESSION_OPCODE_CLIENT_RESEND_LIST,
				foreign_body));
		opennova::ProtocolPacketHeader none_hdr;
		std::vector<opennova::ProtocolMessage> none;
		expect(!receive_records(client, session.server_scrk(), none_hdr, none, 300),
		       "a 0x44 under a foreign key draws nothing");
		send(session.build_goodbye());
	}
	opennova::net::close_socket(client);
	listener.stop();
	opennova::net::shutdown();
	if (g_failures == 0) {
		std::printf("OK: the lobby answers a 0x44 from its retained records\n");
		return 0;
	}
	std::fprintf(stderr, "%d assertion(s) failed\n", g_failures);
	return 1;
}
