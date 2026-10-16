#pragma once

// GET /livez for the programs that serve no HTTP of their own (the gate, the
// lobby, the controller and the worker; program.h): one plain TCP port,
// ONNET_HEALTH_PORT (8081), answered from the program's stall watchdog
// (health.h), so the ECS container health check (curl, in every runtime image)
// and the load balancer's check of the gate's target group read the process's
// liveness as web's own /livez gives it: 200 "ok", or 503 "stalled" while
// Watchdog::healthy() is false; never another detail. HEAD is answered alike
// without the body; another path is a 404, another method a 405, a request
// that does not read a 400. Every reply closes its connection.
//
// One thread of its own accepts and answers one connection at a time, each
// bounded (its request read for at most kRequestTimeoutMs, at most
// kMaxRequestBytes of it), so a probe that stalls holds the port that long at
// most. Crow-free and database-free. Service policy, not a port.

#include "health.h"

#include <net_sockets.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>

namespace opennova::novaworld_server {

class HealthListener {
public:
	static constexpr int kRequestTimeoutMs = 2000;
	static constexpr size_t kMaxRequestBytes = 4096;

	explicit HealthListener(const Watchdog &watchdog) : watchdog_(watchdog) {}
	~HealthListener() { stop(); }
	HealthListener(const HealthListener &) = delete;
	HealthListener &operator=(const HealthListener &) = delete;

	// Binds TCP `port` on every interface (0: the OS picks) and serves on a
	// thread of its own, logging "[health] listening on TCP :<port>"; false when
	// the port cannot be bound.
	bool start(uint16_t port);
	// Ends the thread and closes the port. Idempotent.
	void stop();
	// The port start() bound.
	uint16_t bound_port() const { return bound_port_; }

	// The whole reply to `request` (the bytes read of one, its request line
	// first) for a process `healthy` or not.
	static std::string reply_to(std::string_view request, bool healthy);

private:
	void run();
	void answer(net::Socket &client);

	const Watchdog &watchdog_;
	net::Socket listener_;
	uint16_t bound_port_ = 0;
	std::atomic<bool> stopping_{ false };
	std::thread thread_;
};

} // namespace opennova::novaworld_server
