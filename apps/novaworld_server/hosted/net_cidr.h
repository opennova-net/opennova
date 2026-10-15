#pragma once

// IPv4 networks as the fleet's settings and the internal listener read them:
// the networks the machine agents' routes answer (ONNET_HOSTED_BOOTSTRAP_CIDRS,
// fleet_config.h), the one address a machine's agent may call from
// (agent_route.h) and the private address the internal listener binds. And the
// throttle that keeps a refused peer's log lines to one a minute. Crow-free.
// Service policy, not a port.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::novaworld_server::hosted {

// An IPv4 network: the addresses whose top bits match `network` under `mask`.
struct Ipv4Cidr {
	uint32_t network = 0;
	uint32_t mask = 0;
};

// A dotted quad ("10.44.0.5"): four decimal parts of 0..255, no sign, no
// leading zero past a lone 0, nothing around it.
std::optional<uint32_t> parse_ipv4(std::string_view text);
// "10.44.0.0/16", or a bare address (/32). The address's bits below the
// prefix must be 0.
std::optional<Ipv4Cidr> parse_ipv4_cidr(std::string_view text);
// Whether `peer` (a TCP peer's address as the HTTP listener reports it: a
// dotted quad, or the same as an IPv4-mapped IPv6 "::ffff:a.b.c.d") lies in
// one of `cidrs`. An IPv6 peer, or no CIDR, admits nothing.
bool ipv4_admitted(std::string_view peer, const std::vector<Ipv4Cidr> &cidrs);
// Whether `cidr` lies wholly inside one of the RFC 1918 private ranges
// (10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16), so no prefix shorter than /8:
// the networks ONNET_HOSTED_BOOTSTRAP_CIDRS may name (the agent routes must
// never answer a public peer).
bool ipv4_cidr_private(const Ipv4Cidr &cidr);
// "10.44.0.0/16", for the log.
std::string ipv4_cidr_text(const Ipv4Cidr &cidr);

// One log line per key (a peer) per kWindowMs, for at most kMaxKeys keys at
// once: the agent routes' refusals inside their networks, which a machine
// retrying its bootstrap (or anything else in the subnet) could otherwise
// repeat without end. Thread-safe.
class LogThrottle {
public:
	static constexpr uint64_t kWindowMs = 60 * 1000;
	static constexpr size_t kMaxKeys = 256;
	using Clock = std::function<uint64_t()>;

	explicit LogThrottle(Clock now_ms = {});
	// Whether a line about `key` may be logged now (and if so, it counts).
	bool admit(const std::string &key);

private:
	Clock now_;
	std::mutex mu_;
	std::map<std::string, uint64_t> last_ms_;
};

} // namespace opennova::novaworld_server::hosted
