#include "net_sockets.h"

#include <novacrypto/nwu.h>
#include <novaworld/gate_probe.h>
#include <novaworld/gate_response.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

// Minimal gate responder matching the behavior of apps/novaworld/main.cpp.
// Keeping this test a single file so the socket path is exercised without
// spawning a subprocess.
bool serve_one_probe(uint16_t bound_port, const std::string &response_body) {
	opennova::net::ScopedSocket server(opennova::net::udp_bind(bound_port));
	if (!server.is_valid()) return false;

	uint8_t rx[1024];
	opennova::net::Endpoint from{};
	const int n = opennova::net::udp_recv_from(server.get(), rx, sizeof(rx), from, 1500);
	if (n <= 0) return false;

	std::vector<uint8_t> plain(rx, rx + n);
	opennova::nwu_encrypt(plain.data(), plain.size(), opennova::GATE_NWU_KEY);
	const std::string tag(reinterpret_cast<const char *>(plain.data()),
			plain.empty() ? 0 : plain.size() - 1);
	if (tag != opennova::GATE_PROBE_TAG_JODEMO) return false;

	std::vector<uint8_t> reply(response_body.begin(), response_body.end());
	opennova::nwu_decrypt(reply.data(), reply.size(), opennova::GATE_NWU_KEY);
	return opennova::net::udp_send_to(server.get(), from, reply.data(), reply.size()) > 0;
}

} // namespace

int main() {
	if (!expect(opennova::net::startup() == 0, "net startup")) return 1;

	// Bind the server on an ephemeral port so the test doesn't collide
	// with a real NovaWorld service running locally.
	uint16_t server_port = 0;
	{
		opennova::net::ScopedSocket probe(opennova::net::udp_bind(0, &server_port));
		if (!expect(probe.is_valid() && server_port != 0, "ephemeral bind")) return 1;
	}

	const std::string response_body =
			"VAR POSTIPADDRESS 127.0.0.1\r\n"
			"VAR POSTIPPORT 7597\r\n"
			"VAR UDPNOVAWORLD 127.0.0.1:64206\r\n"
			"VAR STARTUPURL http://127.0.0.1:8080\r\n";

	// Kick off the server in a background thread.
	std::thread server_thread([&] { serve_one_probe(server_port, response_body); });

	// Give the server a moment to bind.
	std::this_thread::sleep_for(std::chrono::milliseconds(100));

	// Client: build gate probe, send, receive response, decrypt+parse.
	opennova::net::ScopedSocket client(opennova::net::udp_bind(0));
	if (!expect(client.is_valid(), "client bind")) return 1;

	const auto probe = opennova::gate_probe_build();
	opennova::net::Endpoint server_ep;
	server_ep.ip = {127, 0, 0, 1};
	server_ep.port = server_port;
	const int sent = opennova::net::udp_send_to(client.get(), server_ep, probe.data(), probe.size());
	if (!expect(sent > 0, "probe send")) return 1;

	uint8_t rx[2048];
	opennova::net::Endpoint from{};
	const int n = opennova::net::udp_recv_from(client.get(), rx, sizeof(rx), from, 2000);
	if (!expect(n > 0, "received reply")) return 1;

	opennova::GateResponse r;
	if (!expect(opennova::gate_response_decrypt_and_parse(rx, static_cast<size_t>(n), r),
			"decrypt+parse reply")) return 1;
	if (!expect(r.var_count == 4, "4 VARs absorbed")) return 1;
	if (!expect((r.post_ip == std::array<uint8_t, 4>{127, 0, 0, 1}),
			"POSTIPADDRESS")) return 1;
	if (!expect(r.post_port == 7597, "POSTIPPORT")) return 1;
	if (!expect(r.udp_novaworld == "127.0.0.1:64206", "UDPNOVAWORLD")) return 1;
	if (!expect(r.startup_url == "http://127.0.0.1:8080", "STARTUPURL")) return 1;

	server_thread.join();
	opennova::net::shutdown();
	std::printf("OK: gate probe round-tripped across UDP loopback\n");
	return 0;
}
