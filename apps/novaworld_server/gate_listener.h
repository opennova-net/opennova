#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace opennova {
class UnknownTracker;
}

namespace opennova::server {

struct ServerConfig;

// Port-7597 UDP listener implementing the simple GATEPROTOCOL bootstrap
// (NWU-encrypted text-tag request → VAR-encoded plain-text response).
//
// Runs on its own thread. start() returns immediately; the listener stays
// up until stop() is called or the destructor runs. Designed to be cheap
// to instantiate and own — main() owns one per process.
class GateListener {
public:
	GateListener();
	~GateListener();

	GateListener(const GateListener &) = delete;
	GateListener &operator=(const GateListener &) = delete;

	// Optional unknown-message tracker. When set, gate probes carrying a
	// tag we don't recognize (not jop:cus2 / jopd:cus4 / dfx2 variants) are
	// recorded (deduped) for /api/unknowns. Null is safe.
	void set_unknown_tracker(opennova::UnknownTracker *tracker) { tracker_ = tracker; }

	// Bind the UDP socket and spawn the receive loop. Returns false if
	// the socket couldn't be bound (port in use, perms, etc.) — main()
	// should treat that as fatal.
	bool start(const ServerConfig &config);

	// Signal stop and join the worker thread. Idempotent.
	void stop();

	bool running() const { return running_.load(); }

private:
	void run_loop();

	std::thread worker_;
	std::atomic<bool> running_{false};
	std::atomic<bool> stop_requested_{false};
	uint16_t bound_port_ = 0;
	std::string public_host_;
	uint16_t nw_udp_port_ = 0;
	uint16_t http_port_   = 0;
	// Reflection override (ONNET_CLIENT_REFLECT_IP/PORT). When set, advertised
	// as ReflectedIpAddress/Port instead of the observed source. 0 port = unset.
	std::string reflect_ip_;
	uint16_t reflect_port_ = 0;
	opennova::UnknownTracker *tracker_ = nullptr;
};

} // namespace opennova::server
