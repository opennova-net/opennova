#include "policy.h"

#include <base/io/log.h>
#include <net/novaworld/gate_probe.h>

namespace opennova::nw_lister {

using io::LogLevel;

namespace {

// A dotted quad on 127.0.0.0/8 (no lookup: names do not parse here).
bool loopback_literal(const std::string &host) {
	net::Endpoint ep;
	return net::resolve_ipv4(host, ep, /*allow_names=*/false) && ep.ip[0] == 127;
}

} // namespace

bool destination_permitted(const std::string &host, DestinationPolicy policy, bool allow_public) {
	if (host == "localhost" || loopback_literal(host)) return true;
	if (allow_public) return true;
	if (policy == DestinationPolicy::NovaLogicGated) return !is_novaworld_domain_host(host);
	return false;
}

bool resolve_destination(const std::string &host, net::Endpoint &out, DestinationPolicy policy, bool allow_public,
                         const char *what) {
	if (!destination_permitted(host, policy, allow_public)) {
		if (policy == DestinationPolicy::NovaLogicGated) {
			io::logf(LogLevel::kWarn, "[net] refused %s '%s': NovaLogic's NovaWorld (%s) needs --allow-public", what,
			         host.c_str(), NOVAWORLD_DOMAIN);
		} else {
			io::logf(LogLevel::kWarn,
			         "[net] refused %s '%s': not on 127.0.0.0/8 and --allow-public not given (names resolve only "
			         "with --allow-public)",
			         what, host.c_str());
		}
		return false;
	}
	bool resolved = false;
	if (host == "localhost") {
		out.ip = {127, 0, 0, 1};
		resolved = true;
	} else {
		resolved = net::resolve_ipv4(host, out, allow_public || policy == DestinationPolicy::NovaLogicGated);
	}
	if (!resolved) {
		io::logf(LogLevel::kWarn, "[net] cannot resolve %s '%s'", what, host.c_str());
		return false;
	}
	if (policy == DestinationPolicy::LoopbackOnly && !allow_public && out.ip[0] != 127) {
		io::logf(LogLevel::kWarn, "[net] refused %s %s: not on 127.0.0.0/8 and --allow-public not given", what,
		         net::endpoint_to_string(out).c_str());
		return false;
	}
	return true;
}

void PolicySocket::send_to(const PeerAddr &to, const uint8_t *data, std::size_t len) {
	if (policy_ == DestinationPolicy::LoopbackOnly && !allow_public_ && (to.ip & 0xFFu) != 127u) {
		if (refused_.insert(to.ip).second) {
			io::logf(LogLevel::kWarn, "[net] refused datagrams to %s: not on 127.0.0.0/8 and --allow-public not given",
			         net::endpoint_to_string(net::NetDatagramSocket::to_endpoint(to)).c_str());
		}
		return;
	}
	inner_.send_to(to, data, len);
}

} // namespace opennova::nw_lister
