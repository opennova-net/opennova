#include "net_sockets.h"

#include <net/napi/envelope.h>
#include <net/novacrypto/nwu.h>
#include <net/novaworld/gate_probe.h>
#include <net/novaworld/gate_response.h>

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
// spawning a subprocess. It answers on `server`, the socket main() bound.
bool serve_one_probe(opennova::net::Socket &server, const std::string &response_body) {
	uint8_t rx[1024];
	opennova::net::Endpoint from{};
	const int n = opennova::net::udp_recv_from(server, rx, sizeof(rx), from, 1500);
	if (n <= 0) return false;

	// Strip the LSB-scatter CRC envelope the client wraps the probe in
	// (matches apps/novaworld_server/gate_listener.cpp + retail).
	std::vector<uint8_t> plain(static_cast<size_t>(n));
	size_t inner_len = 0;
	if (opennova::napi_envelope_decode(rx, static_cast<size_t>(n),
			plain.data(), plain.size(), &inner_len) != 0) {
		return false;
	}
	plain.resize(inner_len);
	opennova::nwu_encrypt(plain.data(), plain.size(), opennova::GATE_NWU_KEY);
	const std::string tag(reinterpret_cast<const char *>(plain.data()),
			plain.empty() ? 0 : plain.size() - 1);
	if (tag != opennova::GATE_PROBE_TAG_JOINTOPS) return false;

	std::vector<uint8_t> reply_inner(response_body.begin(), response_body.end());
	opennova::nwu_decrypt(reply_inner.data(), reply_inner.size(), opennova::GATE_NWU_KEY);
	std::vector<uint8_t> reply(reply_inner.size() + 16);
	size_t reply_size = 0;
	if (opennova::napi_envelope_encode(reply_inner.data(), reply_inner.size(),
			reply.data(), reply.size(), &reply_size) != 0) {
		return false;
	}
	reply.resize(reply_size);
	return opennova::net::udp_send_to(server, from, reply.data(), reply.size()) > 0;
}

} // namespace

int main() {
	if (!expect(opennova::net::startup() == 0, "net startup")) return 1;

	// Bind the server on the port the OS picks, so the test doesn't collide
	// with a real NovaWorld service running locally, and keep that socket: the
	// responder answers on it, so no other process can take the port between
	// the bind and the probe.
	uint16_t server_port = 0;
	opennova::net::ScopedSocket server(opennova::net::udp_bind(0, &server_port));
	if (!expect(server.is_valid() && server_port != 0, "ephemeral bind")) return 1;

	// Quote keys + values exactly as the real gate (and our own
	// gate_listener.cpp) emit them — the parser must strip the quotes
	// (String_TokenizeQuotedToArray @ 0x616d60). A prior unquoted body here
	// masked the "bad gate response" bug against real NovaWorld.
	const std::string response_body =
			"VAR \"POSTIPADDRESS\" \"127.0.0.1\"\r\n"
			"VAR \"POSTIPPORT\" \"7597\"\r\n"
			"VAR \"UDPNOVAWORLD\" \"127.0.0.1:64206\"\r\n"
			"VAR \"STARTUPURL\" \"http://127.0.0.1:8080\"\r\n";

	// Kick off the server in a background thread. Its socket is bound already,
	// so a probe sent before it reads waits in the socket's queue.
	std::thread server_thread([&] { serve_one_probe(server.get(), response_body); });

	// Client: build gate probe, send, receive response, decrypt+parse.
	opennova::net::ScopedSocket client(opennova::net::udp_bind(0));
	if (!expect(client.is_valid(), "client bind")) return 1;

	const auto probe_inner = opennova::gate_probe_build(opennova::GATE_PROBE_TAG_JOINTOPS);
	std::vector<uint8_t> probe(probe_inner.size() + 16);
	size_t probe_size = 0;
	if (!expect(opennova::napi_envelope_encode(probe_inner.data(), probe_inner.size(),
			probe.data(), probe.size(), &probe_size) == 0, "probe envelope encode")) return 1;
	probe.resize(probe_size);
	opennova::net::Endpoint server_ep;
	server_ep.ip = {127, 0, 0, 1};
	server_ep.port = server_port;
	const int sent = opennova::net::udp_send_to(client.get(), server_ep, probe.data(), probe.size());
	if (!expect(sent > 0, "probe send")) return 1;

	uint8_t rx[2048];
	opennova::net::Endpoint from{};
	const int n = opennova::net::udp_recv_from(client.get(), rx, sizeof(rx), from, 2000);
	if (!expect(n > 0, "received reply")) return 1;

	// Strip the envelope the server wraps the response in, then decrypt+parse.
	std::vector<uint8_t> reply_inner(static_cast<size_t>(n));
	size_t reply_inner_len = 0;
	if (!expect(opennova::napi_envelope_decode(rx, static_cast<size_t>(n),
			reply_inner.data(), reply_inner.size(), &reply_inner_len) == 0,
			"reply envelope decode")) return 1;
	reply_inner.resize(reply_inner_len);

	opennova::GateResponse r;
	if (!expect(opennova::gate_response_decrypt_and_parse(reply_inner.data(), reply_inner.size(), r),
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
