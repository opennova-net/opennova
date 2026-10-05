#include "policy.h"

#include <base/io/log.h>

namespace opennova::nw_lister {

using io::LogLevel;

bool resolve_destination(const std::string &host, net::Endpoint &out, bool allow_public, const char *what) {
	bool resolved = false;
	if (host == "localhost") {
		out.ip = {127, 0, 0, 1};
		resolved = true;
	} else {
		resolved = net::resolve_ipv4(host, out, allow_public);
	}
	if (!resolved) {
		io::logf(LogLevel::kWarn, "[net] cannot resolve %s '%s'%s", what, host.c_str(),
		         allow_public ? "" : " (names resolve only with --allow-public)");
		return false;
	}
	if (!allow_public && out.ip[0] != 127) {
		io::logf(LogLevel::kWarn, "[net] refused %s %s: not on 127.0.0.0/8 and --allow-public not given", what,
		         net::endpoint_to_string(out).c_str());
		return false;
	}
	return true;
}

void PolicySocket::send_to(const PeerAddr &to, const uint8_t *data, std::size_t len) {
	if (!allow_public_ && (to.ip & 0xFFu) != 127u) {
		if (refused_.insert(to.ip).second) {
			io::logf(LogLevel::kWarn, "[net] refused datagrams to %s: not on 127.0.0.0/8 and --allow-public not given",
			         net::endpoint_to_string(net::NetDatagramSocket::to_endpoint(to)).c_str());
		}
		return;
	}
	inner_.send_to(to, data, len);
}

} // namespace opennova::nw_lister
