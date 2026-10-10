// The real apps/novaworld_server GateListener over loopback UDP, on port 0.
// start() binds the port the OS picks and hands that socket to the receive
// thread, so bound_port() is the port a probe reaches and the port the reply
// advertises as POSTIPPORT (the host-status sink). UDPNOVAWORLD and STARTUPURL
// name the sibling ports the config passed to start() carries (main() passes
// the ports the NW UDP and HTTP listeners bound). The probe goes out as soon
// as start() returns, with no wait: the socket is bound by then. A second
// listener asking for that same port fails start(), the boot-fatal path.

#include "gate_listener.h"
#include "net_sockets.h"
#include "server_config.h"

#include <net/napi/envelope.h>
#include <net/novaworld/gate_probe.h>
#include <net/novaworld/gate_response.h>

#include "common/test_expect.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace net = opennova::net;
namespace nws = opennova::novaworld_server;

namespace {

// One gate probe to 127.0.0.1:`port` and its reply, decoded into `out`.
int probe_gate(uint16_t port, opennova::GateResponse &out) {
	net::ScopedSocket client(net::udp_bind(0));
	TEST_EXPECT(client.is_valid());

	const auto inner = opennova::gate_probe_build(opennova::GATE_PROBE_TAG_JOINTOPS);
	std::vector<uint8_t> probe(inner.size() + 16);
	size_t probe_size = 0;
	TEST_EXPECT(opennova::napi_envelope_encode(inner.data(), inner.size(), probe.data(),
	                                           probe.size(), &probe_size) == 0);
	probe.resize(probe_size);
	net::Endpoint to;
	to.ip = {127, 0, 0, 1};
	to.port = port;
	TEST_EXPECT(net::udp_send_to(client.get(), to, probe.data(), probe.size()) > 0);

	uint8_t rx[2048];
	net::Endpoint from{};
	const int n = net::udp_recv_from(client.get(), rx, sizeof(rx), from, 3000);
	if (n <= 0) std::fprintf(stderr, "  no gate reply from :%u\n", static_cast<unsigned>(port));
	TEST_EXPECT(n > 0);
	TEST_EXPECT(from.port == port);
	std::vector<uint8_t> reply(static_cast<size_t>(n));
	size_t reply_len = 0;
	TEST_EXPECT(opennova::napi_envelope_decode(rx, static_cast<size_t>(n), reply.data(),
	                                           reply.size(), &reply_len) == 0);
	TEST_EXPECT(opennova::gate_response_decrypt_and_parse(reply.data(), reply_len, out));
	return 0;
}

} // namespace

int main() {
	TEST_EXPECT(net::startup() == 0);
	nws::ServerConfig config;
	config.public_host = "127.0.0.1";
	config.gate_udp_port = 0;
	// The siblings' ports as main() hands them over: what they bound.
	config.nw_udp_port = 40123;
	config.http_port = 40456;

	nws::GateListener gate;
	TEST_EXPECT(gate.start(config));
	const uint16_t port = gate.bound_port();
	TEST_EXPECT(port != 0);

	opennova::GateResponse reply;
	if (probe_gate(port, reply) != 0) return 1;
	TEST_EXPECT((reply.post_ip == std::array<uint8_t, 4>{127, 0, 0, 1}));
	TEST_EXPECT(reply.post_port == port);
	TEST_EXPECT(reply.udp_novaworld == "127.0.0.1:40123");
	const std::string startup = "http://127.0.0.1:40456/nwprepare.dll?";
	TEST_EXPECT(reply.startup_url.compare(0, startup.size(), startup) == 0);

	// The port is held: a second listener cannot bind it, and says so.
	nws::ServerConfig taken = config;
	taken.gate_udp_port = port;
	nws::GateListener second;
	TEST_EXPECT(!second.start(taken));

	gate.stop();
	net::shutdown();
	std::printf("OK: gate listener serves and advertises its bound port :%u\n",
	            static_cast<unsigned>(port));
	return 0;
}
