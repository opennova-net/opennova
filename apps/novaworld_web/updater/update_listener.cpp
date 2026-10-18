#include "update_listener.h"

#include "core/object_store.h"
#include "net_sockets.h"

#include <base/io/strutil.h>
#include <net/update/update_wire.h>

#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <utility>

namespace opennova::novaworld_server::update_service {

bool parse_update_ports(std::string_view text, std::vector<uint16_t> &out) {
	out.clear();
	size_t at = 0;
	while (at <= text.size()) {
		const size_t comma = text.find(',', at);
		const std::string_view part = strutil::trim_view(text.substr(
				at, comma == std::string_view::npos ? std::string_view::npos : comma - at));
		if (part.empty() || !strutil::all_digits(part))
			return false;
		const auto port = strutil::parse_int(std::string(part));
		if (!port || *port < 1 || *port > 65535)
			return false;
		out.push_back(static_cast<uint16_t>(*port));
		if (comma == std::string_view::npos)
			break;
		at = comma + 1;
	}
	return !out.empty();
}

UpdateListener::UpdateListener(
		db::ConnectionPool &pool, ObjectStore &store, UpdateListenerConfig config) :
		pool_(pool),
		store_(store),
		config_(std::move(config)),
		catalog_(config_.cache_dir, config_.update_host),
		server_(catalog_),
		tcp_(server_, [](const std::string &line) {
			std::printf("[update] %s\n", line.c_str());
			std::fflush(stdout);
		}) {}

UpdateListener::~UpdateListener() {
	stop();
}

bool UpdateListener::start() {
	if (pump_thread_.joinable())
		return true;
	if (net::startup() != 0) {
		std::fprintf(stderr, "[update] net::startup failed\n");
		return false;
	}
	refresh_once();
	if (!tcp_.listen(config_.ports, config_.loopback_only)) {
		std::fprintf(stderr, "[update] cannot listen on the update ports\n");
		return false;
	}
	std::printf("[update] serving %zu product(s) on", catalog_.product_count());
	for (const uint16_t port : tcp_.ports())
		std::printf(" %u", static_cast<unsigned>(port));
	std::printf("%s; SERVER.LST names %s (ftp.novaworld.com for a retail updater); cache %s\n",
			config_.loopback_only ? " (loopback)" : "", config_.update_host.c_str(),
			config_.cache_dir.c_str());
	stop_.store(false);
	pump_thread_ = std::thread([this] { pump_loop(); });
	refresh_thread_ = std::thread([this] { refresh_loop(); });
	return true;
}

void UpdateListener::stop() {
	stop_.store(true);
	{
		std::lock_guard<std::mutex> lock(refresh_mu_);
		refresh_cv_.notify_all();
	}
	if (pump_thread_.joinable())
		pump_thread_.join();
	if (refresh_thread_.joinable())
		refresh_thread_.join();
	tcp_.close();
}

void UpdateListener::refresh_now() {
	std::unique_lock<std::mutex> lock(refresh_mu_);
	if (!refresh_thread_.joinable()) {
		lock.unlock();
		refresh_once();
		return;
	}
	const uint64_t ticket = ++refresh_asked_;
	refresh_cv_.notify_all();
	refresh_cv_.wait(lock, [this, ticket] { return refresh_done_ >= ticket || stop_.load(); });
}

void UpdateListener::pump_loop() {
	while (!stop_.load()) {
		if (!tcp_.pump())
			std::this_thread::sleep_for(kIdleSleep);
		turns_.fetch_add(1, std::memory_order_relaxed);
	}
}

void UpdateListener::refresh_loop() {
	uint64_t served = 0;
	while (!stop_.load()) {
		uint64_t ticket = 0;
		{
			std::unique_lock<std::mutex> lock(refresh_mu_);
			refresh_cv_.wait_for(lock, kRefreshInterval,
					[this, served] { return stop_.load() || refresh_asked_ > served; });
			if (stop_.load())
				break;
			ticket = refresh_asked_;
		}
		refresh_once();
		served = ticket;
		{
			std::lock_guard<std::mutex> lock(refresh_mu_);
			refresh_done_ = ticket;
			refresh_cv_.notify_all();
		}
	}
	// Whoever waits on a ticket is released by the stop.
	std::lock_guard<std::mutex> lock(refresh_mu_);
	refresh_cv_.notify_all();
}

void UpdateListener::refresh_once() {
	std::string error;
	try {
		auto db = pool_.acquire();
		if (!catalog_.refresh(*db, store_, error))
			std::fprintf(stderr, "[update] WARN refresh failed: %s\n", error.c_str());
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[update] WARN refresh failed: %s\n", e.what());
	}
}

} // namespace opennova::novaworld_server::update_service
