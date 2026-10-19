#include "billing_config.h"

#include "core/loggable.h"
#include "core/text_util.h"

#include <base/io/strutil.h>

#include <algorithm>
#include <cstdlib>
#include <map>

namespace opennova::novaworld_server::billing {

namespace {

constexpr int kReconcileMin = 5;
constexpr int kReconcileMax = 7 * 24 * 60;

// `text` is `head` and then one or more letters and digits.
bool is_token(std::string_view text, std::string_view head) {
	if (!starts_with(text, head) || text.size() == head.size()) return false;
	return std::all_of(text.begin() + static_cast<std::ptrdiff_t>(head.size()), text.end(), is_alnum);
}

bool is_unset(std::string_view value) { return value.empty() || value == kPlaceholder; }

bool is_unset(const std::vector<std::string> &list) {
	return list.empty() || (list.size() == 1 && list[0] == kPlaceholder);
}

bool is_price(std::string_view text) { return is_token(text, "price_"); }

std::string tier_variable(int tier) { return "STRIPE_PRICE_TIER" + std::to_string(tier); }

// "" when `list` (the value of `variable`) is set and every entry a price id.
std::string price_list_refusal(const std::string &variable, const std::vector<std::string> &list) {
	if (is_unset(list)) return variable + " is not set";
	for (const std::string &price : list) {
		if (!is_price(price)) return variable + " holds '" + loggable(price, 80) + "', not a price id (price_...)";
	}
	return {};
}

// "" when `url` is an http(s) origin: the scheme, a host of letters, digits,
// '.' and '-', an optional port, nothing after.
std::string site_url_refusal(const std::string &url, bool live) {
	if (is_unset(url)) return "ONNET_SITE_URL is not set";
	const bool https = starts_with(url, "https://");
	if (!https && !starts_with(url, "http://"))
		return "ONNET_SITE_URL '" + loggable(url, 120) + "' is not an http(s):// origin";
	if (live && !https) return "ONNET_SITE_URL must be https:// with a live key";
	const std::string_view host_port = std::string_view(url).substr(https ? 8 : 7);
	const size_t colon = host_port.find(':');
	const std::string_view host = host_port.substr(0, colon);
	const std::string_view port = colon == std::string_view::npos ? std::string_view() : host_port.substr(colon + 1);
	const bool host_ok = !host.empty() && std::all_of(host.begin(), host.end(), [](char c) {
		return is_alnum(c) || c == '.' || c == '-';
	});
	const bool port_ok = colon == std::string_view::npos ||
			(!port.empty() && port.size() <= 5 && strutil::all_digits(port));
	if (!host_ok || !port_ok)
		return "ONNET_SITE_URL '" + loggable(url, 120) + "' is not an http(s):// origin (no path, query or fragment)";
	return {};
}

} // namespace

bool BillingConfig::live() const { return starts_with(secret_key, "sk_live_") || starts_with(secret_key, "rk_live_"); }

BillingConfig billing_config_from(const std::function<const char *(const char *)> &getenv) {
	const auto read = [&getenv](const char *name) {
		const char *v = getenv(name);
		return v != nullptr ? std::string(strutil::trim_view(v)) : std::string();
	};
	BillingConfig c;
	c.secret_key = read("STRIPE_SECRET_KEY");
	c.webhook_secret = read("STRIPE_WEBHOOK_SECRET");
	c.site_url = read("ONNET_SITE_URL");
	while (!c.site_url.empty() && c.site_url.back() == '/') c.site_url.pop_back();
	if (const std::string curl = read("ONNET_STRIPE_CURL"); !curl.empty()) c.curl_path = curl;
	for (int tier = 1; tier <= kTierCount; ++tier) c.tier_prices[tier] = split_list(read(tier_variable(tier).c_str()));
	c.own_machine_prices = split_list(read("STRIPE_PRICE_OWN_MACHINE"));
	c.static_address_prices = split_list(read("STRIPE_PRICE_STATIC_ADDRESS"));
	// The vault's placeholder stands for no list, as for any unset variable.
	if (is_unset(c.static_address_prices)) c.static_address_prices.clear();
	const std::string max_addresses = read("ONNET_FLEET_MAX_ADDRESSES");
	c.sells_addresses = !max_addresses.empty() && strutil::all_digits(max_addresses) &&
			strutil::parse_int(max_addresses).value_or(0) > 0;
	const std::string tax = read("ONNET_BILLING_AUTOMATIC_TAX");
	c.automatic_tax = !tax.empty() && std::string_view("1tTyY").find(tax[0]) != std::string_view::npos;
	if (const std::string minutes = read("ONNET_BILLING_RECONCILE_MINUTES"); !minutes.empty()) {
		const bool whole = minutes.size() <= 6 && strutil::all_digits(minutes);
		c.reconcile_minutes = whole ? strutil::parse_int(minutes).value_or(-1) : -1;
	}
	return c;
}

BillingConfig billing_config_from_env() {
	return billing_config_from([](const char *name) -> const char * { return std::getenv(name); });
}

std::string check_billing_config(const BillingConfig &config) {
	if (is_unset(config.secret_key)) return "STRIPE_SECRET_KEY is not set";
	const std::string_view key = config.secret_key;
	if (!is_token(key, "sk_test_") && !is_token(key, "sk_live_") && !is_token(key, "rk_test_") &&
			!is_token(key, "rk_live_"))
		return "STRIPE_SECRET_KEY is not a Stripe secret or restricted key (sk_ or rk_, test or live)";
	if (is_unset(config.webhook_secret)) return "STRIPE_WEBHOOK_SECRET is not set";
	if (!is_token(config.webhook_secret, "whsec_"))
		return "STRIPE_WEBHOOK_SECRET is not a webhook signing secret (whsec_...)";

	std::map<std::string, std::string> listed; // price -> the variable that lists it
	const auto take = [&listed](const std::string &variable, const std::vector<std::string> &list) -> std::string {
		if (std::string why = price_list_refusal(variable, list); !why.empty()) return why;
		for (const std::string &price : list) {
			const auto [it, fresh] = listed.emplace(price, variable);
			if (!fresh)
				return "price '" + loggable(price, 80) + "' is listed twice (" + it->second + " and " + variable +
						")";
		}
		return {};
	};
	for (int tier = 1; tier <= kTierCount; ++tier) {
		if (std::string why = take(tier_variable(tier), config.tier_prices[tier]); !why.empty()) return why;
	}
	if (std::string why = take("STRIPE_PRICE_OWN_MACHINE", config.own_machine_prices); !why.empty()) return why;
	if (config.sells_addresses || !config.static_address_prices.empty()) {
		if (std::string why = take("STRIPE_PRICE_STATIC_ADDRESS", config.static_address_prices); !why.empty())
			return why;
	}

	if (std::string why = site_url_refusal(config.site_url, config.live()); !why.empty()) return why;
	// An absolute path: a POSIX root or a drive letter.
	const std::string &curl = config.curl_path;
	if (curl.empty() || !(curl[0] == '/' || (curl.size() > 2 && curl[1] == ':')))
		return "ONNET_STRIPE_CURL '" + loggable(curl, 120) + "' is not an absolute path";
	if (config.reconcile_minutes < kReconcileMin || config.reconcile_minutes > kReconcileMax)
		return "ONNET_BILLING_RECONCILE_MINUTES must be a whole number of minutes, " + std::to_string(kReconcileMin) +
				" to " + std::to_string(kReconcileMax);
	return {};
}

int tier_of_price(const BillingConfig &config, std::string_view price) {
	if (price.empty()) return 0;
	for (int tier = 1; tier <= kTierCount; ++tier) {
		const std::vector<std::string> &list = config.tier_prices[tier];
		if (std::find(list.begin(), list.end(), price) != list.end()) return tier;
	}
	return 0;
}

bool is_own_machine_price(const BillingConfig &config, std::string_view price) {
	if (price.empty()) return false;
	const std::vector<std::string> &list = config.own_machine_prices;
	return std::find(list.begin(), list.end(), price) != list.end();
}

bool is_static_address_price(const BillingConfig &config, std::string_view price) {
	if (price.empty()) return false;
	const std::vector<std::string> &list = config.static_address_prices;
	return std::find(list.begin(), list.end(), price) != list.end();
}

std::string tier_price_for_sale(const BillingConfig &config, int tier) {
	if (tier < 1 || tier > kTierCount || config.tier_prices[tier].empty()) return {};
	return config.tier_prices[tier].front();
}

std::string own_machine_price_for_sale(const BillingConfig &config) {
	return config.own_machine_prices.empty() ? std::string() : config.own_machine_prices.front();
}

std::string static_address_price_for_sale(const BillingConfig &config) {
	return config.static_address_prices.empty() ? std::string() : config.static_address_prices.front();
}

} // namespace opennova::novaworld_server::billing
