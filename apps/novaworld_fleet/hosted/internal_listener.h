#pragma once

// The service's internal listener: a second HTTP server of its own (a Crow app
// apart from web's public HttpListener), the fleet's, bound while it leads
// to one address, the fleet task's private one in a deployment (<its private
// IPv4>:8070, from its ECS task's metadata, which the deployment's Cloud Map
// name for the fleet follows; the machines'
// security group alone reaches it) or loopback in development
// (ONNET_INTERNAL_HTTP_BIND, 127.0.0.1:8070). It serves the machine agents' routes
// alone (agent_route.h); every other path and method gets their one bare 404. The retail client,
// the site and nginx never reach it. Service policy, not a port.
//
// start() runs Crow on a thread of its own and returns once it serves; stop()
// ends it and joins. Its handlers run on kHandlerThreads threads of Crow's,
// each leasing one connection of the service's pool for its request.

#include "fleet_desired_state.h"

#include "hosted/fleet_config.h"

#include <net/novaworld/db/database.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace opennova::novaworld_server::hosted {

class InternalListener {
public:
	static constexpr unsigned kHandlerThreads = 4;

	// `pool` and `desired` must outlive the listener.
	InternalListener(db::ConnectionPool &pool, FleetDesiredState &desired, AgentRouteConfig config);
	~InternalListener();
	InternalListener(const InternalListener &) = delete;
	InternalListener &operator=(const InternalListener &) = delete;

	// Binds `address` (a dotted quad) and `port` (0: the OS's pick) and returns
	// once Crow serves, or false when Crow's run() failed first (the address
	// is not this machine's, the port is taken).
	bool start(const std::string &address, uint16_t port);
	void stop();

	// The port Crow bound; valid once start() returned true.
	uint16_t bound_port() const { return bound_port_; }

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
	std::thread worker_;
	std::atomic<bool> running_{ false };
	uint16_t bound_port_ = 0;
};

} // namespace opennova::novaworld_server::hosted
