#pragma once

// The monthly support tiers (a Stripe subscription that supports the project
// and sets the hosting entitlement): how many hosted servers each tier grants.
// The prices that sell each tier come from the service's configuration
// (billing_config.h: STRIPE_PRICE_TIER1..3, the first price of each list sold,
// the rest grandfathered), never an amount here. Service policy, not a port.

namespace opennova::novaworld_server::billing {

// Tiers 1..3; 0 is no tier.
inline constexpr int kTierCount = 3;

// Hosted servers a tier grants, by tier (index 0: none).
inline constexpr int kTierServers[kTierCount + 1] = { 0, 1, 2, 3 };

// The most hosted servers, and own machines, an admin may grant an account
// outside any subscription (comp_grants, 0..16 each; billing_repository.h).
inline constexpr int kMaxCompServers = 16;
inline constexpr int kMaxCompOwnMachines = 16;

} // namespace opennova::novaworld_server::billing
