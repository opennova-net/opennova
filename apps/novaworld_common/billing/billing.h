#pragma once

// Billing as the service runs it (the monthly support tiers over Stripe
// Checkout and the Customer Portal; README "Billing (monthly support tiers)"):
// the complete settings, the typed client over a transport, the sync worker and
// this database's instance id. Web and the worker each make one when
// check_billing_config answers "" (parts.h; the transport curl,
// CurlStripeApi), the tests over the fake
// (tests/novaworld/billing_fake_stripe.h); the routes (billing_routes.h) and the
// heartbeat read it, and with none the routes answer billing_disabled. Crow-free.
// Service policy, not a port.

#include "billing_config.h"
#include "billing_sync.h"
#include "entitlement.h"
#include "stripe_api.h"
#include "stripe_client.h"

#include <net/novaworld/db/database.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace opennova::novaworld_server::billing {

class Billing {
public:
	// `api` and `pool` must outlive it; `config` complete.
	Billing(BillingConfig config, StripeApi &api, db::ConnectionPool &pool, std::string instance_id,
			BillingSync::Clock clock = unix_now) :
			config_(std::move(config)),
			client_(api),
			instance_id_(std::move(instance_id)),
			sync_(pool, client_, config_, instance_id_, std::move(clock)) {}
	Billing(const Billing &) = delete;
	Billing &operator=(const Billing &) = delete;

	const BillingConfig &config() const { return config_; }
	StripeClient &client() { return client_; }
	BillingSync &sync() { return sync_; }
	const BillingSync &sync() const { return sync_; }
	const std::string &instance_id() const { return instance_id_; }

	// A webhook delivery refused (a bad signature, a body that is no event):
	// the heartbeat reports how many since its last line.
	void count_webhook_reject() { webhook_rejects_.fetch_add(1, std::memory_order_relaxed); }
	uint64_t webhook_rejects() const { return webhook_rejects_.load(std::memory_order_relaxed); }

private:
	BillingConfig config_;
	StripeClient client_;
	std::string instance_id_;
	BillingSync sync_;
	std::atomic<uint64_t> webhook_rejects_{ 0 };
};

} // namespace opennova::novaworld_server::billing
