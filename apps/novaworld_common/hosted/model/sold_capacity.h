#pragma once

// What the hosting the service has sold comes to, against what its fleet can
// hold (ADR 0066): a new subscription is never sold past the fleet, so a
// paying owner's start is not refused at_capacity for room someone else
// bought after it. Every account holds what its hosting entitlement grants
// (billing/entitlement.h), whether or not it runs a server: its shared servers
// (the larger of its comp grant and its tier's, never the sum) are places on
// shared machines, its dedicated machines are dedicated machines, and its
// static addresses are addresses. Of the
// addresses, what the cloud holds counts too (address_repository.h
// held_addresses_by_owner): an account's are the larger of those granted and
// its rows that hold a grant, plus its rows on their way out (lapsed,
// releasing: their Elastic IP held until released), and a gone account's rows
// count as they are, so the fleet never holds more Elastic IPs than it sells.
// An open checkout's reservation (billing_repository.h "A checkout's hold")
// counts as the subscription it sells. The fleet sells ONNET_FLEET_MAX_SHARED x
// ONNET_FLEET_SHARED_SLOTS shared places, ONNET_FLEET_MAX_OWN dedicated machines and
// ONNET_FLEET_MAX_ADDRESSES addresses (placement.h PlacementPolicy). An admin's
// comp grant counts as sold too, and may take the sold past what the fleet
// sells (the user page warns), as may a lowered maximum; then no new
// subscription of that item is sold until the sold is back under it, and a
// start's at_capacity (hosted_repository.h request_server) stays the hard
// gate. Pure functions, but sold_now, which reads the rows. Service policy,
// not a port.

#include "billing/billing_repository.h"
#include "billing/entitlement.h"
#include "placement.h"

#include <net/novaworld/db/database.h>

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::novaworld_server::hosted {

// The three things the fleet holds.
struct Capacity {
	int64_t shared = 0;    // places on shared machines
	int64_t own = 0;       // dedicated machines
	int64_t addresses = 0; // static addresses

	Capacity &operator+=(const Capacity &other);
};
Capacity operator-(const Capacity &a, const Capacity &b);

// What an account with entitlement `e` holds.
Capacity held_by(const billing::Entitlement &e);
// What the fleet sells under `policy`.
Capacity sellable_of(const PlacementPolicy &policy);
// Whether `sold` is past `sellable` in any of the three (an admin's grant, a
// lowered maximum): the admin's attention.
bool sold_exceeds_capacity(const Capacity &sold, const Capacity &sellable);
// Whether `sold` takes `more` (what a new subscription adds; a part of it
// below 0 frees room and is no refusal) under `sellable`: each part that
// grows stays within its sellable.
bool fits(const Capacity &sold, const Capacity &more, const Capacity &sellable);

// What one more subscription of `item` (`tier` for a tier) adds for an
// account whose entitlement's facts are `account` and whose granting
// subscriptions (its reservations among them, reservation_row) are
// `subscriptions`, at `now`.
Capacity added_by(const billing::AccountFacts &account, const std::vector<billing::SubscriptionRow> &subscriptions,
		billing::ItemKind item, int tier, int64_t now);
// The same for an account that has nothing yet (GET /api/billing/tiers'
// availability; a dedicated machine needs no tier beside it).
Capacity added_by_new_account(billing::ItemKind item, int tier, int64_t now);

// What every player account holds at `now`, its reservations included but
// checkout session `except_session`'s (the one a new checkout replaces; ""
// for none), with the addresses the cloud holds (the header), in the caller's
// transaction (under the hosting-start lock for a checkout's judgement).
Capacity sold_now(db::Database &db, int64_t now, const std::string &except_session = {});
// The account's facts and its subscriptions with its live reservations as
// added_by reads them.
struct AccountHoldings {
	billing::AccountFacts facts;
	std::vector<billing::SubscriptionRow> subscriptions;
};
AccountHoldings holdings_of(db::Database &db, int64_t user_id, int64_t now, const std::string &except_session = {});

} // namespace opennova::novaworld_server::hosted
