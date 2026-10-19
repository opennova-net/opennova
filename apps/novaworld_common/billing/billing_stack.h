#pragma once

// Billing as a program runs it (web's routes and webhook, the worker's sync),
// each over a curl transport of its own, decided alike from the same settings.
// Service policy, not a port.

#include "billing.h"
#include "billing_config.h"
#include "command_runner.h"
#include "core/scratch_dir.h"
#include "stripe_api.h"

#include <net/novaworld/db/database.h>

#include <memory>

namespace opennova::novaworld_server::billing {

// The curl transport (its runner and its owner-only scratch directory,
// removed last) and the billing over it.
struct BillingStack {
	ScratchDir scratch;
	std::unique_ptr<proc::CommandRunner> runner;
	std::unique_ptr<CurlStripeApi> api;
	std::unique_ptr<Billing> billing;
};

// Billing over `config`, or none with a boot warning naming why: settings not
// complete and well-formed (check_billing_config), no scratch directory, a
// transport that would not run, no instance id. Both secrets are log secrets
// either way.
BillingStack make_billing(const BillingConfig &config, db::ConnectionPool &pool, db::Database &db);

} // namespace opennova::novaworld_server::billing
