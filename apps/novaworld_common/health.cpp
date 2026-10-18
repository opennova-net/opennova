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
	std::string fields;
	const auto field = [&fields](const char *name, std::optional<int64_t> value) {
		if (value)
			fields += std::string(",\"") + name + "\":" + std::to_string(*value);
	};
	field("nw_sessions", b.nw_sessions);
	field("listed_hosts", b.listed_hosts);
	field("hosted_live", b.hosted_live);
	field("hosted_failed", b.hosted_failed);
	field("machines_lost", b.machines_lost);
	field("machines_held", b.machines_held);
	field("launch_failures", b.launch_failures);
	field("billing_sync_age_s", b.billing_sync_age_s);
	field("webhook_rejects", b.webhook_rejects);
	field("limits_held", b.limits_held);
	field("probes_unanswered", b.probes_unanswered);
	field("lobbies_live", b.lobbies_live);
	field("attention_danger", b.attention_danger);
	field("attention_warn", b.attention_warn);
	field("machines_stuck", b.machines_stuck);
	field("machine_disk_free_pct", b.machine_disk_free_pct);
	field("fleet_pass_age_s", b.fleet_pass_age_s);
	field("db_lock_timeouts", b.db_lock_timeouts);
	field("db_deadlocks", b.db_deadlocks);
	// The program is one of the five names (program.h): no escaping needed.
	return "{\"heartbeat\":{\"program\":\"" + b.program + "\"" + fields + "}}";
}

} // namespace opennova::novaworld_server
