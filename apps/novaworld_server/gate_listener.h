#pragma once

#include <net/novaworld/db/sqlite.h>

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>

namespace opennova {
class UnknownTracker;
}

namespace opennova::novaworld_server {

struct ServerConfig;

// Port-7597 UDP listener implementing the simple GATEPROTOCOL bootstrap
// (NWU-encrypted text-tag request -> VAR-encoded plain-text response) and,
// on the same socket, the POSTIPADDRESS:POSTIPPORT sink for the plaintext
// host-status heartbeat a hosting retail client posts every ~30 s
// [orig: Lobby_UpdateServerInfo @0x4ff448..0x4ff62c]. policy: the gate
// advertises its own port as POSTIPPORT so no second port needs opening;
// the blob is told apart from a probe by its "HostKey =" preamble before
// the NWU tag decrypt runs.
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

	// Optional DB pool. When set, start() leases one connection and hands it
	// to the receive thread for its lifetime, and a received host-status blob
	// refreshes the active_hosts row that owns its HostKey on it
	// (hostdb::apply_status_blob).
	void set_db_pool(opennova::db::ConnectionPool *pool) { db_pool_ = pool; }

	// Bind the UDP socket, lease the thread's DB connection and spawn the
	// receive loop. Returns false if the socket couldn't be bound (port in
	// use, perms, etc.) or the connection couldn't be opened — main() should
	// treat that as fatal.
	bool start(const ServerConfig &config);

	// Signal stop and join the worker thread. Idempotent.
	void stop();

private:
	void run_loop(std::optional<db::ConnectionPool::Lease> db_conn);

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
	// MET endpoint + GLSVSS parameters (ServerConfig); emitted when configured.
	std::string met_ip_;
	uint16_t    met_port_ = 0;
	std::string met_label_;
	int         met_ping_ = 0;
	int         met_ext_  = 0;
	std::string glsvss_request_;
	int         glsvss_rims_  = 0;
	int         glsvss_agrms_ = 0;
	opennova::UnknownTracker *tracker_ = nullptr;
	opennova::db::ConnectionPool *db_pool_ = nullptr;
};

} // namespace opennova::novaworld_server
