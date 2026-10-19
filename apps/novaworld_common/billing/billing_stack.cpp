#include "billing_stack.h"

#include "billing_repository.h"
#include "core/loggable.h"

#include <cstdio>
#include <string>
#include <utility>

namespace opennova::novaworld_server::billing {

BillingStack make_billing(const BillingConfig &config, db::ConnectionPool &pool, db::Database &db) {
	BillingStack s;
	add_log_secret(config.secret_key);
	add_log_secret(config.webhook_secret);
	const auto off = [](const std::string &why) {
		std::fprintf(stderr, "[boot] WARN billing: %s; billing off\n", why.c_str());
	};
	if (const std::string why = check_billing_config(config); !why.empty()) {
		off(why);
		return s;
	}
	std::string error;
	ScratchDir scratch = ScratchDir::make("opennova-billing-", error);
	if (scratch.empty()) {
		off("no scratch directory for the Stripe calls" +
				(error.empty() ? std::string() : ": " + error));
		return s;
	}
	CurlStripeApi::Config api;
	api.curl = config.curl_path;
	api.secret_key = config.secret_key;
	api.scratch_dir = scratch.path().u8string();
	if (const std::string why = CurlStripeApi::check_config(api); !why.empty()) {
		off("the Stripe transport: " + why);
		return s;
	}
	std::string instance;
	try {
		instance = instance_id(db);
	} catch (const db::Error &e) {
		off(std::string("no instance id: ") + e.what());
		return s;
	}
	s.scratch = std::move(scratch);
	s.runner = std::make_unique<proc::ProcessCommandRunner>(curl_environment(), api.scratch_dir);
	s.api = std::make_unique<CurlStripeApi>(*s.runner, std::move(api));
	s.billing = std::make_unique<Billing>(config, *s.api, pool, instance);
	std::printf("[boot] billing: on, %s mode, instance %s, checkout returns to %s%s\n",
			config.live() ? "live" : "test", instance.c_str(), config.site_url.c_str(),
			config.automatic_tax ? ", Stripe Tax on" : "");
	return s;
}

} // namespace opennova::novaworld_server::billing
