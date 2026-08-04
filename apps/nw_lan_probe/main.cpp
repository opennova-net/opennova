// nw-lan-probe -- wait for a retail-compatible JO LAN host to answer the
// stateless 0x41 discovery probe. This is an automation/readiness tool: it
// deliberately stops before 0x42 admission and therefore never consumes a
// player slot or mutates the host session.

#include "lan_probe_client.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

void print_usage() {
	std::fprintf(stderr,
			"usage: nw-lan-probe [--host A.B.C.D] [--port N] [--timeout-ms N] "
			"[--interval-ms N] [--expect-name NAME]\n");
}

bool parse_bounded_int(const char *text, int minimum, int maximum, int &out) {
	if (!text || !*text) return false;
	char *end = nullptr;
	const long value = std::strtol(text, &end, 10);
	if (!end || *end != '\0' || value < minimum || value > maximum) return false;
	out = static_cast<int>(value);
	return true;
}

bool parse_ipv4(const char *text, opennova::net::Endpoint &endpoint) {
	if (!text) return false;
	unsigned a = 0, b = 0, c = 0, d = 0;
	char trailing = '\0';
	if (std::sscanf(text, "%u.%u.%u.%u%c", &a, &b, &c, &d, &trailing) != 4 ||
			a > 255 || b > 255 || c > 255 || d > 255) {
		return false;
	}
	endpoint.ip = {static_cast<uint8_t>(a), static_cast<uint8_t>(b),
			static_cast<uint8_t>(c), static_cast<uint8_t>(d)};
	return true;
}

std::string one_line(std::string value) {
	for (char &c : value) {
		if (c == '\r' || c == '\n') c = ' ';
	}
	return value;
}

} // namespace

int main(int argc, char *argv[]) {
	using namespace opennova;
	lanprobe::Options options;
	options.client_index = static_cast<uint32_t>(
			std::chrono::steady_clock::now().time_since_epoch().count());
	if (options.client_index == 0) options.client_index = 1;

	for (int i = 1; i < argc; ++i) {
		const char *arg = argv[i];
		if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
			print_usage();
			return 0;
		}
		if (i + 1 >= argc) {
			print_usage();
			return 2;
		}
		const char *value = argv[++i];
		int parsed = 0;
		if (std::strcmp(arg, "--host") == 0) {
			if (!parse_ipv4(value, options.endpoint)) {
				std::fprintf(stderr, "nw-lan-probe: --host must be a dotted IPv4 address\n");
				return 2;
			}
		} else if (std::strcmp(arg, "--port") == 0) {
			if (!parse_bounded_int(value, 1, 65535, parsed)) {
				std::fprintf(stderr, "nw-lan-probe: --port must be from 1 through 65535\n");
				return 2;
			}
			options.endpoint.port = static_cast<uint16_t>(parsed);
		} else if (std::strcmp(arg, "--timeout-ms") == 0) {
			if (!parse_bounded_int(value, 1, 300000, options.timeout_ms)) {
				std::fprintf(stderr, "nw-lan-probe: --timeout-ms must be from 1 through 300000\n");
				return 2;
			}
		} else if (std::strcmp(arg, "--interval-ms") == 0) {
			if (!parse_bounded_int(value, 10, 60000, options.retry_interval_ms)) {
				std::fprintf(stderr, "nw-lan-probe: --interval-ms must be from 10 through 60000\n");
				return 2;
			}
		} else if (std::strcmp(arg, "--expect-name") == 0) {
			options.expected_server_name = value;
		} else {
			std::fprintf(stderr, "nw-lan-probe: unknown option: %s\n", arg);
			print_usage();
			return 2;
		}
	}

	if (net::startup() != 0) {
		std::fprintf(stderr, "LAN_READY=0\nLAN_ERROR=socket_startup\n");
		return 3;
	}
	const lanprobe::Result result = lanprobe::wait_for_server(options);
	net::shutdown();

	std::printf("LAN_READY=%d\n", result.status == lanprobe::Status::Ready ? 1 : 0);
	std::printf("LAN_ENDPOINT=%s\n", net::endpoint_to_string(options.endpoint).c_str());
	std::printf("LAN_PROBES_SENT=%d\n", result.probes_sent);
	if (result.status != lanprobe::Status::Ready) {
		const char *reason = result.status == lanprobe::Status::Timeout ? "timeout" :
				result.status == lanprobe::Status::SendError ? "send_error" : "socket_error";
		std::printf("LAN_ERROR=%s\n", reason);
		return result.status == lanprobe::Status::Timeout ? 1 : 3;
	}

	std::printf("LAN_SERVER_NAME=%s\n", one_line(result.server.server_name).c_str());
	std::printf("LAN_SESSION_ID=%s\n", one_line(result.server.session_id).c_str());
	std::printf("LAN_EXPANSION=%s\n", one_line(result.server.expansion).c_str());
	std::printf("LAN_GAMETYPE=%u\n", static_cast<unsigned>(result.server.gametype));
	std::printf("LAN_PLAYERS=%u\n", static_cast<unsigned>(result.server.current_players));
	std::printf("LAN_MAX_PLAYERS=%u\n", static_cast<unsigned>(result.server.max_players));
	return 0;
}
