#pragma once

// The owners' limits' hold (supervisor.h, "The owners' limits"): set by the
// supervisor when one pass would stop the servers of more owners than it ever
// should at once (the billing cache in doubt). It releases itself once a full
// reconcile with Stripe has completed clean after it took its owners and a pass
// after that finds only those owners (or some of them) still past their
// entitlements: the cache is confirmed, and their stops apply. An admin's
// override (POST /api/admin/billing/reconcile {"force_limits": true}) makes the
// next pass apply the stops past the brake, once.
//
// One row (hosted_limits_hold, migration 0008), so the supervisor (the
// fleet's), the billing sync's reconcile (a worker's) and the admin's
// routes (the web's) share it through the database alone; each read-modify-
// write is one transaction on the row. Each function throws db::Error on a
// database failure. Service policy, not a port.

#include <net/novaworld/db/database.h>

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>

namespace opennova::novaworld_server::hosted {

// What the admin's view shows: the stops held back, as the last pass counted
// them.
struct LimitsHoldView {
	bool held = false;
	std::string reason;
	size_t owners = 0;
	size_t servers = 0;
};

// What a hold did.
enum class LimitsHeld : uint8_t {
	Now, // it did not hold before
	Widened, // owners not among its own: it took the pass's, and waits for a reconcile again
	Still, // the same owners (or some of them): its counts updated
};

namespace limits_hold {

LimitsHoldView view(db::Database &db);
bool held(db::Database &db);

// The billing sync: a full reconcile completed clean (BillingSync's
// on_reconciled).
void reconciled(db::Database &db);
// The clean reconciles so far. A pass reads it before the cache, so every one
// it counts landed before what the pass reads.
uint64_t reconciles(db::Database &db);

// A pass that read reconciles() as `reconciles` would stop `servers` servers
// of `owners` past the brake (or while held): it holds with `reason`. A hold
// already on keeps its owners, and the reconcile it waits for, while the
// pass's are among them; else it takes the pass's and waits for a reconcile
// after `reconciles`.
LimitsHeld hold(db::Database &db, const std::string &reason, const std::set<int64_t> &owners,
		size_t servers, uint64_t reconciles);
// Releases it when a clean reconcile completed after it took its owners (the
// pass read `reconciles` past that) and `owners`, the owners the pass would
// stop, are all among its own; true when it released.
bool release_if_confirmed(db::Database &db, const std::set<int64_t> &owners, uint64_t reconciles);
// Clears it; true when it held.
bool clear(db::Database &db);

// An admin's override: the next pass applies the stops past the brake.
void force_next_pass(db::Database &db);
// The pass: whether an override is pending, which the read takes.
bool take_force(db::Database &db);

} // namespace limits_hold

} // namespace opennova::novaworld_server::hosted
