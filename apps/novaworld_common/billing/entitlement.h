#pragma once

// An account's hosting entitlement: how many hosted servers it may have live
// at once (max_servers = the larger of its support tier's servers and an
// admin's comp grant of servers, comp_grants), how many of them may run on an
// own machine (the own-machine add-on's quantity, counted only while a tier
// also grants, plus an admin's comp grant of own machines, counted while the
// account has servers), and whether a new one may start now. Computed from the
// billing tables (billing_repository.h), the cache of Stripe the sync worker
// keeps, whether or not billing is configured, so a deploy that breaks billing
// never stops a payer's servers. Crow-free. Service policy, not a port.
//
// A subscription grants while Stripe calls it active, trialing or past_due
// (Stripe still retries the card: the servers stay up, but no new one starts),
// and only while its period, with a 48 h net, has not run out: a sync that
// stopped for good (the webhook endpoint gone, the key revoked) fails closed
// once the paid period is over. Every other status (incomplete,
// incomplete_expired, unpaid, canceled, paused, any unknown one) grants
// nothing.

#include "billing_repository.h"
#include "tiers.h"

#include <net/novaworld/db/database.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::novaworld_server::billing {

// The net past a period's end before a granting subscription stops granting.
inline constexpr int64_t kPeriodNetSeconds = 48 * 60 * 60;

struct Entitlement {
	int max_servers = 0;   // max(comp_servers, the granting tier's servers); 0 locks hosting
	// The granting add-on quantities while a tier grants, plus
	// comp_own_machines while max_servers > 0.
	int own_machines = 0;
	bool can_start = false; // max_servers > 0 and no granting subscription past_due
	// The admin's grants (comp_grants; also on an account that gets nothing).
	int comp_servers = 0;
	int comp_own_machines = 0;
	// Its static addresses: the granting static-address subscriptions'
	// quantities plus the admin's comp addresses (address_grants, each one
	// the cloud holds), and of them the ones its own machines can use
	// (static_addresses, at most own_machines).
	int address_grants = 0;
	int static_addresses = 0;
	int comp_addresses = 0;
	int tier = 0;          // the highest granting tier, 0 for none
	// The tier subscription the account's page shows: the granting one of the
	// highest tier, else the newest one; "" with none.
	std::string status;
	std::string source = "none"; // "none", "tier" (the tier gives at least the grant) or "comp"
	std::optional<int64_t> current_period_end; // that subscription's, Unix seconds
	bool cancel_at_period_end = false;
};

// The account facts the entitlement reads beside the subscriptions.
struct AccountFacts {
	std::string kind = "player";           // players.kind
	std::string account_status = "active"; // players.account_status
	int comp_servers = 0;                  // comp_grants' servers
	int comp_own_machines = 0;             // comp_grants' own_machine
	// Its comp addresses that count (allocating or ready;
	// hosted/model/address_repository.h count_comp_addresses_of).
	int comp_addresses = 0;
};

// Whether Stripe's `status` is one that grants (active, trialing, past_due).
bool status_grants(std::string_view status);
// Whether the subscription grants at `now` (Unix seconds): a granting status,
// a tier (1..3) or an add-on (an own machine, a static address), and now <
// current_period_end + kPeriodNetSeconds (no period end: no grant).
bool subscription_grants(const SubscriptionRow &row, int64_t now);

// The pure core: 0 servers unless the account is a player's and active.
Entitlement entitlement_from(const AccountFacts &account, const std::vector<SubscriptionRow> &subscriptions,
		int64_t now);
// The account's, read from the database (no such account: 0 servers).
Entitlement entitlement_of(db::Database &db, int64_t user_id, int64_t now);

// The wall clock in Unix seconds (the entitlement's `now` in the service).
int64_t unix_now();

} // namespace opennova::novaworld_server::billing
