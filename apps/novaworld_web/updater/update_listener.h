#pragma once

// The update listener (ADR 0061 d3, ADR 0064): UPDATE.EXE's gateway and update server, one
// machine playing both roles as retail's did, over the published releases. A thread of its
// own pumps net/update's connection machine (engine/net/update/update_server.h) through
// apps/common's TCP listener (update_tcp_server.h) on the gateway ports, and a second
// thread refreshes the catalog (release_catalog.h) from the rows every kRefreshInterval,
// fetching and unpacking new packages off the pump's way, so a stall watchdog watching
// the pump's turns is never tripped by a download. Started by web's main when
// ONNET_UPDATE_HOST is set (the name SERVER.LST sends a client of this deployment to;
// retail's own names reach it through the Switcher's hosts block); logs as
// opennova-update-tool's serve does. Service policy, not a port.

#include "updater/release_catalog.h"
#include "update_tcp_server.h"

#include <net/novaworld/db/database.h>
#include <net/update/update_server.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace opennova::novaworld_server {

class ObjectStore;

namespace update_service {

struct UpdateListenerConfig {
	// ServerConfig::update_host (ONNET_UPDATE_HOST): the update host SERVER.LST names for
	// a data set not served through retail's names; "" runs no listener.
	std::string update_host;
	// ServerConfig::update_ports (ONNET_UPDATE_PORTS): the gateway ports, retail's four by
	// default (20044, 20556, 21068, 21580). The tests pass 0 for one the OS picks.
	std::vector<uint16_t> ports;
	// ServerConfig::update_cache_dir (ONNET_UPDATE_CACHE_DIR): where the packages and their
	// unpacked sections are kept; "" for a scratch directory the embedder makes.
	std::string cache_dir;
	// Loopback alone (the tests).
	bool loopback_only = false;
};

// A comma-separated list of ports 1..65535 (ONNET_UPDATE_PORTS's form) into `out`; false
// for anything else.
bool parse_update_ports(std::string_view text, std::vector<uint16_t> &out);

class UpdateListener {
public:
	// How often the catalog is refreshed from the rows.
	static constexpr std::chrono::milliseconds kRefreshInterval{ 30 * 1000 };
	// The pump's sleep when nothing moved.
	static constexpr std::chrono::milliseconds kIdleSleep{ 2 };

	UpdateListener(db::ConnectionPool &pool, ObjectStore &store, UpdateListenerConfig config);
	~UpdateListener();
	UpdateListener(const UpdateListener &) = delete;
	UpdateListener &operator=(const UpdateListener &) = delete;

	// The first refresh, the ports bound, the two threads started. False (logged) when a
	// port cannot be bound; a first refresh that fails is logged and tried again on the
	// interval.
	bool start();
	void stop();

	// A refresh now, on the refresh thread, returned once it ran (the tests).
	void refresh_now();

	// The pump loop's turns, for the stall watchdog.
	uint64_t turns() const { return turns_.load(std::memory_order_relaxed); }
	// The ports bound (the OS's picks for 0), valid once start() returned true.
	const std::vector<uint16_t> &ports() const { return tcp_.ports(); }
	const ReleaseCatalog &catalog() const { return catalog_; }

private:
	void pump_loop();
	void refresh_loop();
	void refresh_once();

	db::ConnectionPool &pool_;
	ObjectStore &store_;
	UpdateListenerConfig config_;
	ReleaseCatalog catalog_;
	update::UpdateServer server_;
	net::UpdateTcpServer tcp_;
	std::thread pump_thread_;
	std::thread refresh_thread_;
	std::atomic<bool> stop_{ false };
	std::atomic<uint64_t> turns_{ 0 };
	// The refresh thread's wake-ups: refresh_now hands it a ticket and waits for it.
	std::mutex refresh_mu_;
	std::condition_variable refresh_cv_;
	uint64_t refresh_asked_ = 0;
	uint64_t refresh_done_ = 0;
};

} // namespace update_service
} // namespace opennova::novaworld_server
