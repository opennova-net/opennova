#pragma once

// The owners' limits' hold (supervisor.h, "The owners' limits"): set by the
// supervisor when one pass would stop the servers of more owners than it ever
// should at once (the billing cache in doubt). It releases itself once a full
// reconcile with Stripe has completed clean after it took its owners and a pass
// after that finds only those owners (or some of them) still past their
// entitlements: the cache is confirmed, and their stops apply. An admin's
// override (POST /api/admin/billing/reconcile {"force_limits": true}) makes the
// next pass apply the stops past the brake, once. main() owns one that outlives
// the supervisor, the billing worker and the HTTP listener; a supervisor given
// none keeps its own. Thread-safe. Service policy, not a port.

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <utility>

namespace opennova::novaworld_server::hosted {

class LimitsHold {
public:
	// What the admin's view shows: the stops held back, as the last pass
	// counted them.
	struct View {
		bool held = false;
		std::string reason;
		size_t owners = 0;
		size_t servers = 0;
	};
	// What hold() did.
	enum class Held : uint8_t {
		Now,     // it did not hold before
		Widened, // owners not among its own: it took the pass's, and waits for a reconcile again
		Still,   // the same owners (or some of them): its counts updated
	};

	View view() const {
		std::lock_guard<std::mutex> lock(mu_);
		return View{ held_, reason_, held_ ? last_owners_ : 0, held_ ? servers_ : 0 };
	}
	bool held() const {
		std::lock_guard<std::mutex> lock(mu_);
		return held_;
	}
	std::string reason() const {
		std::lock_guard<std::mutex> lock(mu_);
		return reason_;
	}

	// The billing worker: a full reconcile completed clean (BillingSync's
	// on_reconciled).
	void reconciled() {
		std::lock_guard<std::mutex> lock(mu_);
		++reconciles_;
	}
	// The clean reconciles so far. A pass reads it before the cache, so every
	// one it counts landed before what the pass reads.
	uint64_t reconciles() const {
		std::lock_guard<std::mutex> lock(mu_);
		return reconciles_;
	}

	// A pass that read reconciles() as `reconciles` would stop `servers` servers
	// of `owners` past the brake (or while held): it holds with `reason`. A hold
	// already on keeps its owners, and the reconcile it waits for, while the
	// pass's are among them; else it takes the pass's and waits for a reconcile
	// after `reconciles`.
	Held hold(std::string reason, const std::set<int64_t> &owners, size_t servers, uint64_t reconciles) {
		std::lock_guard<std::mutex> lock(mu_);
		Held what = Held::Still;
		if (!held_) {
			what = Held::Now;
		} else if (!among_locked(owners)) {
			what = Held::Widened;
		}
		if (what != Held::Still) {
			owners_ = owners;
			taken_at_ = reconciles;
		}
		held_ = true;
		reason_ = std::move(reason);
		last_owners_ = owners.size();
		servers_ = servers;
		return what;
	}
	// Releases it when a clean reconcile completed after it took its owners (the
	// pass read `reconciles` past that) and `owners`, the owners the pass would
	// stop, are all among its own; true when it released.
	bool release_if_confirmed(const std::set<int64_t> &owners, uint64_t reconciles) {
		std::lock_guard<std::mutex> lock(mu_);
		if (!held_ || reconciles <= taken_at_ || !among_locked(owners)) return false;
		clear_locked();
		return true;
	}
	// Clears it; true when it held.
	bool clear() {
		std::lock_guard<std::mutex> lock(mu_);
		const bool was = held_;
		clear_locked();
		return was;
	}

	// An admin's override: the next pass applies the stops past the brake.
	void force_next_pass() {
		std::lock_guard<std::mutex> lock(mu_);
		force_ = true;
	}
	// The pass: whether an override is pending, which the read takes.
	bool take_force() {
		std::lock_guard<std::mutex> lock(mu_);
		return std::exchange(force_, false);
	}

private:
	bool among_locked(const std::set<int64_t> &owners) const {
		for (const int64_t o : owners) {
			if (owners_.count(o) == 0) return false;
		}
		return true;
	}
	void clear_locked() {
		held_ = false;
		reason_.clear();
		owners_.clear();
		last_owners_ = 0;
		servers_ = 0;
	}

	mutable std::mutex mu_;
	bool held_ = false;
	std::string reason_;
	std::set<int64_t> owners_; // the owners it waits on a reconcile for
	uint64_t taken_at_ = 0;    // reconciles_ as the pass that took owners_ read it
	size_t last_owners_ = 0;   // the last pass's counts, for the admin's view
	size_t servers_ = 0;
	uint64_t reconciles_ = 0;
	bool force_ = false;
};

} // namespace opennova::novaworld_server::hosted
