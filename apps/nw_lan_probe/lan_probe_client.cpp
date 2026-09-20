#include "lan_probe_client.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <vector>

namespace opennova::lanprobe {

Result wait_for_server(const Options &options) {
	Result result;
	if (options.endpoint.port == 0 || options.timeout_ms <= 0 ||
			options.retry_interval_ms <= 0) {
		result.status = Status::SocketError;
		return result;
	}

	net::ScopedSocket socket(net::udp_bind(0));
	if (!socket.is_valid()) {
		result.status = Status::SocketError;
		return result;
	}

	const std::vector<uint8_t> probe = opennova::build_lan_discovery_probe(options.client_index);
	using Clock = std::chrono::steady_clock;
	const auto deadline = Clock::now() + std::chrono::milliseconds(options.timeout_ms);
	auto next_send = Clock::now();
	std::array<uint8_t, 4096> bytes{};

	while (Clock::now() < deadline) {
		const auto now = Clock::now();
		if (now >= next_send) {
			const int sent = net::udp_send_to(socket.get(), options.endpoint,
					probe.data(), probe.size());
			++result.probes_sent;
			if (sent != static_cast<int>(probe.size())) {
				result.status = Status::SendError;
				return result;
			}
			next_send = now + std::chrono::milliseconds(options.retry_interval_ms);
		}

		const auto receive_until = std::min(deadline, next_send);
		const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
				receive_until - Clock::now());
		const int receive_timeout_ms = std::max(1, static_cast<int>(remaining.count()));
		net::Endpoint source;
		const int received = net::udp_recv_from(
				socket.get(), bytes.data(), bytes.size(), source, receive_timeout_ms);
		// Windows may surface an ICMP port-unreachable from a cold UDP endpoint
		// as WSAECONNRESET on recvfrom. That is precisely the transient state this
		// readiness loop is meant to outlive, so receive errors consume the same
		// bounded retry window as an ordinary timeout.
		if (received < 0) continue;
		if (received == 0 || source.port != options.endpoint.port ||
				source.ip != options.endpoint.ip) {
			continue;
		}

		opennova::LanDiscoveryServer server;
		if (!opennova::parse_lan_discovery_reply(
				bytes.data(), static_cast<size_t>(received), options.client_index, server)) {
			continue;
		}
		if (!options.expected_server_name.empty() &&
				server.server_name != options.expected_server_name) {
			continue;
		}

		result.status = Status::Ready;
		result.server = std::move(server);
		result.source = source;
		return result;
	}

	result.status = Status::Timeout;
	return result;
}

} // namespace opennova::lanprobe
