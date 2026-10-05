#include "lan_probe_client.h"

#include <net/npwire/nw_session_framing.h>
#include <net/npwire/session_hello.h>
#include <net/npwire/session_keys.h>

#include "net_sockets.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

using namespace opennova;

bool expect(bool condition, const char *message) {
	if (condition) return true;
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

} // namespace

int main() {
	if (!expect(net::startup() == 0, "socket subsystem starts")) return 1;

	uint16_t host_port = 0;
	net::ScopedSocket host(net::udp_bind(0, &host_port));
	if (!expect(host.is_valid() && host_port != 0, "fake LAN host binds an ephemeral port")) {
		net::shutdown();
		return 1;
	}

	bool responder_ok = false;
	std::thread responder([&]() {
		std::array<uint8_t, 2048> bytes{};
		net::Endpoint client;
		const int received = net::udp_recv_from(
				host.get(), bytes.data(), bytes.size(), client, 2000);
		if (received <= 0) return;

		uint8_t opcode = 0;
		std::vector<uint8_t> body;
		ClientHello client_hello;
		if (!nw_decode_inbound(bytes.data(), static_cast<size_t>(received), opcode, body) ||
				opcode != SESSION_OPCODE_CLIENT_HELLO ||
				!parse_client_hello(body.data(), body.size(), client_hello) ||
				!matches_jointoperations_identity(client_hello)) {
			return;
		}

		ServerHello hello = build_server_hello(client_hello, 0x7F000001u, client.port);
		hello.is_game_server = true;
		hello.sn = "Expected Host";
		hello.p1 = 0x00010020u;
		hello.np = 1;
		hello.mp = 4;
		hello.sus1 = "GSID-ready-test";
		hello.sus2 = "revx02";
		const std::vector<uint8_t> reply = nw_encode_outbound(
				SESSION_OPCODE_SERVER_HELLO, server_hello_to_bytes(hello));
		responder_ok = net::udp_send_to(
				host.get(), client, reply.data(), reply.size()) == static_cast<int>(reply.size());
	});

	lan_probe::Options options;
	options.endpoint = {{127, 0, 0, 1}, host_port};
	options.timeout_ms = 1500;
	options.retry_interval_ms = 50;
	options.client_index = 0x11223344u;
	options.expected_server_name = "Expected Host";
	const lan_probe::Result found = lan_probe::wait_for_server(options);
	responder.join();

	bool ok = true;
	ok = expect(responder_ok, "fake host answered the retail discovery probe") && ok;
	ok = expect(found.status == lan_probe::Status::Ready,
			"probe reports ready only after a valid game-server reply") && ok;
	ok = expect(found.source.port == host_port && found.server.server_name == "Expected Host",
			"probe returns the answering endpoint and advertised identity") && ok;
	ok = expect(found.server.gametype == 0x00010020u && found.server.current_players == 1 &&
				found.server.max_players == 4 && found.server.expansion == "revx02",
			"probe returns the live discovery metadata") && ok;
	ok = expect(found.probes_sent >= 1, "probe reports its send count") && ok;

	const uint16_t now_unanswered_port = host_port;
	host = net::ScopedSocket{};
	options.endpoint.port = now_unanswered_port;
	options.timeout_ms = 60;
	options.retry_interval_ms = 20;
	options.expected_server_name.clear();
	const lan_probe::Result absent = lan_probe::wait_for_server(options);
	ok = expect(absent.status == lan_probe::Status::Timeout,
			"an unanswered endpoint stops at the bounded deadline") && ok;

	net::shutdown();
	if (!ok) return 1;
	std::puts("PASS: LAN responder readiness probe");
	return 0;
}
