#include "gate_listener.h"

#include "server_config.h"

#include <net_sockets.h>
#include <napi/envelope.h>
#include <novacrypto/nwu.h>
#include <novaworld/gate_probe.h>
#include <novaworld/unknown_tracker.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <sstream>
#include <string>
#include <vector>

namespace opennova::server {

namespace {

// Build a VAR-encoded gate response. Format mirrors the working
// implementations in worktree-net-final/apps/novaworld_gate/src/main.cpp
// (build_gate_response_body) and onnet's onnw/gate_server.py
// (build_gate_response). Both are validated against retail jodemo per
// memory `project_net_phase_c2_visibility`.
//
// Wire shape (all CRLF-terminated):
//   GATEPROTOCOL "1.0"
//   VAR "key" "value"   ← keys + values double-quoted
//   ...
//
// `public_host` is the IP/host the server advertises; `nw_udp_port` and
// `http_port` are the ports clients should redirect to.
//
// POSTIPADDRESS + POSTIPPORT are REQUIRED: the retail parser
// [orig: CNapiGateManager_ProcessResponse @ 0x4ced20] fails the gate to
// state -9 ("NO NW POST IP" / "NO NW POST PORT") without them, unless the
// junction bypass is set. Emitting them here is the load-bearing fix that
// onnet was missing (grilled 2026-06-11; docs/net/novaworld-net-re.md §8).
std::string build_gate_response(const std::string &public_host,
                                uint16_t nw_udp_port,
                                uint16_t http_port,
                                const std::string &reflected_ip,
                                uint16_t reflected_port) {
	auto append_var = [](std::ostringstream &os, const char *key, const std::string &value) {
		os << "VAR \"" << key << "\" \"" << value << "\"\r\n";
	};

	std::ostringstream os;
	os << "GATEPROTOCOL \"1.0\"\r\n";
	append_var(os, "LobbyName", "jop_2_consumer");
	append_var(os, "POSTIPADDRESS", public_host);
	append_var(os, "POSTIPPORT", std::to_string(http_port));
	append_var(os, "NovaworldWebDomainNameAndPortNumber",
	           public_host + ":" + std::to_string(http_port));
	append_var(os, "NovaworldName", "jop_2_consumer");
	append_var(os, "udpnovaworld",
	           public_host + ":" + std::to_string(nw_udp_port));
	append_var(os, "udpcode1", "abc");
	append_var(os, "udpcode2", "xyz");
	append_var(os, "startupurl",
	           "http://" + public_host + ":" + std::to_string(http_port) +
	           "/nwprepare.dll?ver1=[VER1]&ver2=[VER2]&cc=[CC]&gt=[GT]&url=jop_2_start.htm");
	append_var(os, "usejunction", "0");
	append_var(os, "clearjunction", "0");
	append_var(os, "ReflectedIpAddress", reflected_ip);
	append_var(os, "ReflectedPortNumber", std::to_string(reflected_port));

	const std::time_t now = std::time(nullptr);
	std::tm tm{};
#ifdef _WIN32
	gmtime_s(&tm, &now);
#else
	gmtime_r(&now, &tm);
#endif
	char time_buf[64];
	if (std::strftime(time_buf, sizeof(time_buf), "%a %b %d %H:%M:%S %Y", &tm) == 0) {
		std::snprintf(time_buf, sizeof(time_buf), "unknown");
	}
	append_var(os, "TimeOnGateServer", time_buf);

	return os.str();
}

// Per memory `reference_gate_tags.md`:
//   `jopd:cus4` = jodemo (Joint Operations demo)
//   `jop:cus2`  = retail Joint Operations
// Both share LobbyName `jop_2_consumer`. Reject anything else for now.
bool is_known_tag(const std::string &tag) {
	return tag == "jopd:cus4"
	    || tag == "jop:cus2"
	    || tag == "dfx2:0:cus:buffy";
}

} // namespace

GateListener::GateListener() = default;

GateListener::~GateListener() {
	stop();
}

bool GateListener::start(const ServerConfig &config) {
	if (running_.load()) {
		return true;
	}

	if (opennova::net::startup() != 0) {
		std::fprintf(stderr, "[gate] net::startup failed\n");
		return false;
	}

	uint16_t bound = 0;
	auto sock = opennova::net::udp_bind(config.gate_udp_port, &bound);
	if (!sock.is_valid()) {
		std::fprintf(stderr, "[gate] failed to bind UDP %u\n",
		             static_cast<unsigned>(config.gate_udp_port));
		return false;
	}
	// Close — we'll re-bind inside run_loop so the socket lifetime tracks
	// the worker thread, simplifying error paths.
	opennova::net::close_socket(sock);

	bound_port_     = config.gate_udp_port;
	public_host_    = config.public_host;
	nw_udp_port_    = config.nw_udp_port;
	http_port_      = config.http_port;

	reflect_ip_   = config.client_reflect_ip;
	reflect_port_ = config.client_reflect_gate_port;

	stop_requested_.store(false);
	running_.store(true);
	worker_ = std::thread([this] { run_loop(); });
	std::printf("[gate] listening on UDP :%u\n",
	            static_cast<unsigned>(bound_port_));
	return true;
}

void GateListener::stop() {
	stop_requested_.store(true);
	if (worker_.joinable()) {
		worker_.join();
	}
	running_.store(false);
}

void GateListener::run_loop() {
	opennova::net::ScopedSocket socket(opennova::net::udp_bind(bound_port_));
	if (!socket.is_valid()) {
		std::fprintf(stderr, "[gate] re-bind failed; aborting loop\n");
		running_.store(false);
		return;
	}

	uint8_t rx[2048];
	while (!stop_requested_.load()) {
		opennova::net::Endpoint from{};
		const int n = opennova::net::udp_recv_from(socket.get(), rx, sizeof(rx),
		                                           from, /*timeout_ms=*/250);
		if (n <= 0) continue;

		// Strip the LSB-scatter CRC envelope first (retail wraps the
		// gate probe in it — confirmed by net-final's working server at
		// worktree-net-final/apps/novaworld_gate/src/main.cpp).
		std::vector<uint8_t> plain(static_cast<size_t>(n));
		size_t inner_len = 0;
		if (opennova::napi_envelope_decode(rx, static_cast<size_t>(n),
		                                   plain.data(), plain.size(),
		                                   &inner_len) != 0) {
			std::fprintf(stderr, "[gate] %s:%u — bad envelope (%d bytes)\n",
			             opennova::net::endpoint_to_string(from).c_str(),
			             from.port, n);
			continue;
		}
		plain.resize(inner_len);

		// Decrypt with NWU + GATE_NWU_KEY ("GATEAPI") to recover the tag.
		// (Names are swapped vs onnet — see memory
		// `reference_nwu_names_swapped.md`.)
		opennova::nwu_encrypt(plain.data(), plain.size(),
		                      opennova::GATE_NWU_KEY);

		// Trim trailing NUL the client appended (gate_probe_build adds one).
		const size_t tag_len = (!plain.empty() && plain.back() == 0)
		                       ? plain.size() - 1 : plain.size();
		std::string tag(reinterpret_cast<const char *>(plain.data()), tag_len);

		if (!is_known_tag(tag)) {
			std::fprintf(stderr, "[gate] %s:%u — unknown tag '%s'; ignoring\n",
			             opennova::net::endpoint_to_string(from).c_str(),
			             from.port, tag.c_str());
			if (tracker_) {
				using namespace std::chrono;
				const auto gate_now = static_cast<uint64_t>(
					duration_cast<milliseconds>(
						steady_clock::now().time_since_epoch()).count());
				tracker_->record("gate", tag, plain.data(), plain.size(),
				                 opennova::net::endpoint_to_string(from),
				                 gate_now);
			}
			continue;
		}

		const std::string client_ip =
			std::to_string(from.ip[0]) + "." + std::to_string(from.ip[1]) + "." +
			std::to_string(from.ip[2]) + "." + std::to_string(from.ip[3]);
		// Reflection override (dev/NAT): tell the client its reachable endpoint
		// is the configured reflect addr, not the observed source (the docker
		// gateway behind a bridge). Empty/0 => advertise the observed source
		// (the prod path). [cf onnw/gate_server.py:24-25]
		const std::string reflected_ip =
			reflect_ip_.empty() ? client_ip : reflect_ip_;
		const uint16_t reflected_port =
			reflect_port_ != 0 ? reflect_port_ : from.port;
		const std::string body = build_gate_response(public_host_, nw_udp_port_,
		                                             http_port_, reflected_ip,
		                                             reflected_port);

		std::vector<uint8_t> reply_inner(body.begin(), body.end());
		opennova::nwu_decrypt(reply_inner.data(), reply_inner.size(),
		                      opennova::GATE_NWU_KEY);
		// Wrap in the LSB-scatter CRC envelope retail expects.
		std::vector<uint8_t> reply(reply_inner.size() + 16);
		size_t reply_size = 0;
		if (opennova::napi_envelope_encode(reply_inner.data(), reply_inner.size(),
		                                   reply.data(), reply.size(),
		                                   &reply_size) != 0) {
			std::fprintf(stderr, "[gate] %s:%u — envelope encode failed\n",
			             client_ip.c_str(), from.port);
			continue;
		}
		reply.resize(reply_size);
		const int sent = opennova::net::udp_send_to(socket.get(), from,
		                                            reply.data(), reply.size());
		if (sent > 0) {
			std::printf("[gate] %s:%u tag=%s -> %d bytes\n",
			            client_ip.c_str(), from.port, tag.c_str(), sent);
		} else {
			std::fprintf(stderr, "[gate] %s:%u send failed\n",
			             client_ip.c_str(), from.port);
		}
	}

	std::printf("[gate] loop exiting\n");
}

} // namespace opennova::server
