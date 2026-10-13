#include "bootstrap_route.h"

#include "http_json.h"
#include "loggable.h"

#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace opennova::novaworld_server::hosted {

namespace {

// The one refusal: an unknown path's 404, uncacheable.
crow::response not_found() {
	crow::response res = json_error(404, "not_found");
	res.set_header("Cache-Control", "no-store");
	return res;
}

} // namespace

void register_bootstrap_route(WebApp &app, BootstrapStore &store, std::vector<Ipv4Cidr> admitted) {
	// A refusal inside the networks is logged once per peer per minute.
	auto throttle = std::make_shared<LogThrottle>();
	CROW_ROUTE(app, "/hosted/bootstrap/<uint>")
			.methods("POST"_method)([&store, admitted = std::move(admitted), throttle](const crow::request &req,
											uint64_t id) {
				// The socket's own peer: no header names the client here.
				const std::string &peer = req.remote_ip_address;
				if (!ipv4_admitted(peer, admitted)) return not_found();
				if (id > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) return not_found();
				const int64_t server_id = static_cast<int64_t>(id);
				const std::string auth = req.get_header_value("Authorization");
				const std::string token = auth.rfind("Bearer ", 0) == 0 ? auth.substr(7) : std::string();
				std::string why;
				auto redeemed = store.redeem(server_id, token, peer, why);
				if (!redeemed) {
					if (throttle->admit(peer))
						std::fprintf(stderr, "[hosted] WARN bootstrap for server %lld from %s refused: %s\n",
								static_cast<long long>(server_id), loggable(peer).c_str(), why.c_str());
					return not_found();
				}
				std::printf("[hosted] server %lld: machine %s fetched its bootstrap bundle\n",
						static_cast<long long>(server_id), loggable(redeemed->machine_id).c_str());
				crow::response res(200);
				res.body = bootstrap_bundle_json(redeemed->bundle);
				res.set_header("Content-Type", "application/json");
				res.set_header("Cache-Control", "no-store");
				return res;
			});
}

} // namespace opennova::novaworld_server::hosted
