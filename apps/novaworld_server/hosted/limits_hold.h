#pragma once

// The owners' limits' hold (supervisor.h, "The owners' limits"): set by the
// supervisor when one pass would stop the servers of more owners than it ever
// should at once, and cleared by an admin's billing reconcile (POST
// /api/admin/billing/reconcile). main() owns one that outlives both the
// supervisor and the HTTP listener; a supervisor given none keeps its own.
// Thread-safe. Service policy, not a port.

#include <mutex>
#include <string>
#include <utility>

namespace opennova::novaworld_server::hosted {

class LimitsHold {
public:
	bool held() const {
		std::lock_guard<std::mutex> lock(mu_);
		return held_;
	}
	std::string reason() const {
		std::lock_guard<std::mutex> lock(mu_);
		return reason_;
	}
	// Holds with `reason`; true when it did not hold before.
	bool hold(std::string reason) {
		std::lock_guard<std::mutex> lock(mu_);
		if (held_) return false;
		held_ = true;
		reason_ = std::move(reason);
		return true;
	}
	// Clears it; true when it held.
	bool clear() {
		std::lock_guard<std::mutex> lock(mu_);
		const bool was = held_;
		held_ = false;
		reason_.clear();
		return was;
	}

private:
	mutable std::mutex mu_;
	bool held_ = false;
	std::string reason_;
};

} // namespace opennova::novaworld_server::hosted
