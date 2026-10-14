#include "destination_policy.h"

#include <base/io/log.h>
#include <net/novaworld/gate_probe.h>

namespace opennova::serve {

using io::LogLevel;

namespace {

// A dotted quad on 127.0.0.0/8 (no lookup: names do not parse here).
bool loopback_literal(const std::string &host) {
	net::Endpoint ep;
	return net::resolve_ipv4(host, ep, /*allow_names=*/false) && ep.ip[0] == 127;
}

} // namespace

bool destination_permitted(const std::string &host, bool allow_public) {
	if (host == "localhost" || loopback_literal(host))
		return true;
	if (allow_public)
		return true;
	return !is_novaworld_domain_host(host);
}

bool resolve_destination(
		const std::string &host, net::Endpoint &out, bool allow_public, const char *what) {
	if (!destination_permitted(host, allow_public)) {
		io::logf(LogLevel::kWarn,
				"[net] refused %s '%s': NovaLogic's NovaWorld (%s) needs --allow-public", what,
				host.c_str(), NOVAWORLD_DOMAIN);
		return false;
	}
	bool resolved = false;
	if (host == "localhost") {
		out.ip = { 127, 0, 0, 1 };
		resolved = true;
	} else {
		resolved = net::resolve_ipv4(host, out, /*allow_names=*/true);
	}
	if (!resolved) {
		io::logf(LogLevel::kWarn, "[net] cannot resolve %s '%s'", what, host.c_str());
		return false;
	}
	return true;
}

} // namespace opennova::serve
