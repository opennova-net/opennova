#pragma once

// The machine agent's two routes (agent_protocol.h), served on the internal
// listener alone (internal_listener.h), never on the public port:
//
//   POST /hosted/machines/<id>/bootstrap   Authorization: Bearer <one-time token>
//   POST /hosted/machines/<id>/sync        Authorization: Bearer <agent secret>
//
// The bootstrap redeems the machine's one-time token (fleet_repository.h
// redeem_bootstrap_token) for a new agent secret, minted here, kept as its
// SHA-256 alone, and handed over with the serve image the machine was
// launched with and its slots; until the agent's first sync spends the token,
// a redeem again (a lost reply's retry) mints a fresh secret in place of the
// last. The sync authenticates the agent's secret (its digest compared in
// constant time), stores its report (record_sync: the first one spends the
// token and makes a booting machine ready), asks
// the supervisor what the machine should run (FleetDesiredState) and answers
// it: the containers with the trees they use, the files of a container this
// report names left out, each container's uid its slot's, one whose slot is
// past the machine's dropped, and drain set whenever the machine's row is
// draining. A sync in another protocol version
// is not read: its machine is drained (a lost one stays lost, ended by the
// supervisor). A sync whose agent could not list its
// containers (listing_failed) records that the agent was heard from alone
// (record_unlisted_sync), logged at most once per peer per minute.
//
// Who is answered. A request is read only from a TCP peer (the socket's own
// address, never a header) that is a private or loopback address inside the
// configured networks (fleet_config.h agent_cidrs), and only from the machine's
// own private address as its launch recorded it. Every refusal, a request from
// elsewhere included, is the one bare 404 an unknown path gets, uncacheable; a
// refusal inside the networks is logged without the token or the secret, at
// most once per peer per minute (LogThrottle). The replies carry
// Cache-Control: no-store and are never logged. Service policy, not a port.

#include "hosted/net_cidr.h"
#include "hosted/fleet_config.h"
#include "hosted/fleet_desired_state.h"

#include <crow.h>

#include <net/novaworld/db/database.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace opennova::novaworld_server::hosted {

class AgentRoutes {
public:
	// A body past this is refused unread (a report of every slot is a few KB).
	static constexpr size_t kMaxBodyBytes = 256 * 1024;

	// `pool` and `desired` must outlive the routes.
	AgentRoutes(db::ConnectionPool &pool, FleetDesiredState &desired, AgentRouteConfig config);

	// Both routes, and the catch-all that answers every other path and method
	// with the same 404.
	void register_routes(crow::SimpleApp &app);

	crow::response bootstrap(const crow::request &req, uint64_t id);
	crow::response sync(const crow::request &req, uint64_t id);

	// The one refusal.
	static crow::response not_found();

private:
	bool admits(const std::string &peer) const;
	// A refusal inside the networks, logged (throttled per peer).
	crow::response refuse(
			const std::string &peer, const char *route, int64_t id, const std::string &why);

	db::ConnectionPool &pool_;
	FleetDesiredState &desired_;
	AgentRouteConfig config_;
	std::shared_ptr<LogThrottle> throttle_;
};

} // namespace opennova::novaworld_server::hosted
