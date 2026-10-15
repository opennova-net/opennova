#include "health.h"

#include <chrono>
#include <utility>

namespace opennova::novaworld_server {

Watchdog::Watchdog(uint64_t stall_ms, Clock clock) : stall_ms_(stall_ms), clock_(std::move(clock)) {
	last_check_ms_ = clock_();
}

uint64_t Watchdog::steady_now_ms() {
	using namespace std::chrono;
	return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

void Watchdog::watch(std::string name, std::function<uint64_t()> progress) {
	std::lock_guard<std::mutex> lock(mu_);
	Loop loop;
	loop.name = std::move(name);
	loop.progress = std::move(progress);
	loop.last_count = loop.progress();
	loop.moved_ms = clock_();
	loops_.push_back(std::move(loop));
}

std::string Watchdog::check() {
	std::lock_guard<std::mutex> lock(mu_);
	const uint64_t now = clock_();
	last_check_ms_ = now;
	std::string stalled;
	for (Loop &loop : loops_) {
		const uint64_t count = loop.progress();
		if (count != loop.last_count) {
			loop.last_count = count;
			loop.moved_ms = now;
		} else if (stalled.empty() && now - loop.moved_ms >= stall_ms_) {
			stalled = loop.name;
		}
	}
	return stalled;
}

bool Watchdog::healthy() const {
	std::lock_guard<std::mutex> lock(mu_);
	const uint64_t now = clock_();
	if (now - last_check_ms_ >= stall_ms_) return false;
	for (const Loop &loop : loops_) {
		if (loop.progress() == loop.last_count && now - loop.moved_ms >= stall_ms_) return false;
	}
	return true;
}

std::string heartbeat_line(const Heartbeat &b) {
	const auto field = [](const char *name, int64_t value) {
		return std::string("\"") + name + "\":" + std::to_string(value);
	};
	return "{\"heartbeat\":{" + field("nw_sessions", b.nw_sessions) + "," + field("listed_hosts", b.listed_hosts) +
			"," + field("hosted_live", b.hosted_live) + "," + field("hosted_failed", b.hosted_failed) + "," +
			field("machines_lost", b.machines_lost) + "," + field("launch_failures", b.launch_failures) + "," +
			field("billing_sync_age_s", b.billing_sync_age_s) + "," + field("webhook_rejects", b.webhook_rejects) +
			"}}";
}

} // namespace opennova::novaworld_server
