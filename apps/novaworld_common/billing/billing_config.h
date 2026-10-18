#pragma once

// The billing settings (monthly support tiers over Stripe Checkout and the
// Customer Portal), read from the service's environment:
//
//   STRIPE_SECRET_KEY               sk_/rk_ (test or live; a restricted rk_ key is the recommended one)
//   STRIPE_WEBHOOK_SECRET           whsec_...: the webhook endpoint's signing secret
//   STRIPE_PRICE_TIER1..3           price_... comma lists: the first is sold, the rest grandfathered
//   STRIPE_PRICE_OWN_MACHINE        the own-machine add-on's prices, the same way
//   STRIPE_PRICE_STATIC_ADDRESS     the static-address add-on's, the same way; needed only
//                                   while the fleet sells addresses
//                                   (ONNET_FLEET_MAX_ADDRESSES above 0)
//   ONNET_SITE_URL                  the site's origin, for Checkout's and the Portal's return URLs
//                                   (https:// in live mode)
//   ONNET_STRIPE_CURL               the curl binary (default /usr/bin/curl)
//   ONNET_BILLING_AUTOMATIC_TAX     1: Stripe Tax on Checkout
//   ONNET_BILLING_RECONCILE_MINUTES the full reconcile's period (default 360)
//
// Billing is on only when the set is complete and well-formed
// (check_billing_config answers ""); "REPLACE_ME", the vault's placeholder,
// counts as unset. No reason text ever holds a secret's value. Crow-free.
// Service policy, not a port.

#include "tiers.h"

#include <array>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::novaworld_server::billing {

// The vault's placeholder for a value the maintainer has not filled in yet.
inline constexpr const char *kPlaceholder = "REPLACE_ME";

struct BillingConfig {
	std::string secret_key;     // STRIPE_SECRET_KEY
	std::string webhook_secret; // STRIPE_WEBHOOK_SECRET
	std::string site_url;       // ONNET_SITE_URL, its trailing '/' dropped
	std::string curl_path = "/usr/bin/curl"; // ONNET_STRIPE_CURL
	// STRIPE_PRICE_TIER<n> at index n (1..kTierCount); index 0 stays empty.
	std::array<std::vector<std::string>, kTierCount + 1> tier_prices;
	std::vector<std::string> own_machine_prices; // STRIPE_PRICE_OWN_MACHINE
	std::vector<std::string> static_address_prices; // STRIPE_PRICE_STATIC_ADDRESS
	// The fleet sells static addresses (ONNET_FLEET_MAX_ADDRESSES above 0):
	// then their prices are required.
	bool sells_addresses = false;
	bool automatic_tax = false;                  // ONNET_BILLING_AUTOMATIC_TAX
	// ONNET_BILLING_RECONCILE_MINUTES; -1 when it was set to no whole number.
	int reconcile_minutes = 360;

	// Whether the key is a live-mode key (sk_live_ / rk_live_).
	bool live() const;
};

// The settings `getenv` answers (a name in, its value or null out; an empty
// value is unset). Values are trimmed; a list's empty entries are dropped.
BillingConfig billing_config_from(const std::function<const char *(const char *)> &getenv);
// The settings of this process's environment.
BillingConfig billing_config_from_env();

// "" when `config` is complete and well-formed, else the first reason it is
// not, naming the variable ("STRIPE_SECRET_KEY is not set"): a key of
// ^(sk|rk)_(test|live)_, a webhook secret of ^whsec_, every price ^price_ and
// in one list alone, a non-empty list per tier and for the add-on, an
// http(s) site URL with no path, query or fragment (https in live mode), the
// static-address add-on's list while sells_addresses (else it may be unset), an
// absolute curl path, and a reconcile period of 5 to 10080 minutes.
std::string check_billing_config(const BillingConfig &config);

// The tier (1..kTierCount) `price` sells or once sold; 0 when it is none of them.
int tier_of_price(const BillingConfig &config, std::string_view price);
// Whether `price` is (or was) the own-machine add-on's, or the static-address add-on's.
bool is_own_machine_price(const BillingConfig &config, std::string_view price);
bool is_static_address_price(const BillingConfig &config, std::string_view price);
// The price that sells tier `tier` now (its list's first), or the add-on; "" when none.
std::string tier_price_for_sale(const BillingConfig &config, int tier);
std::string own_machine_price_for_sale(const BillingConfig &config);
std::string static_address_price_for_sale(const BillingConfig &config);

} // namespace opennova::novaworld_server::billing
