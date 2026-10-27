#include "service_settings.h"

#include <charconv>
#include <string_view>
#include <utility>

namespace opennova::novaworld_server::hosted {

using db::BindValue;

namespace {

// The service_meta keys.
constexpr const char *kSubscriptions = "subscriptions_enabled";
constexpr const char *kMaxShared = "fleet_max_shared";
constexpr const char *kSharedSlots = "fleet_shared_slots";
constexpr const char *kMaxOwn = "fleet_max_own";
constexpr const char *kMaxAddresses = "fleet_max_addresses";

std::optional<int> whole(std::string_view text) {
	int value = 0;
	const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
	if (ec != std::errc() || end != text.data() + text.size() || value < 0)
		return std::nullopt;
	return value;
}

std::string in_range(const char *what, const std::optional<int> &value, int low, int high) {
	if (!value || (*value >= low && *value <= high))
		return {};
	return std::string(what) + " is " + std::to_string(low) + " to " + std::to_string(high);
}

} // namespace

ServiceSettings read_service_settings(db::Database &db) {
	ServiceSettings out;
	const auto rows =
			db.query("SELECT key, value FROM service_meta WHERE key IN ($1, $2, $3, $4, $5);",
					{ BindValue(std::string(kSubscriptions)), BindValue(std::string(kMaxShared)),
							BindValue(std::string(kSharedSlots)), BindValue(std::string(kMaxOwn)),
							BindValue(std::string(kMaxAddresses)) });
	for (const auto &r : rows) {
		const std::string key = r.as_text(0).value_or("");
		const std::string value = r.as_text(1).value_or("");
		if (key == kSubscriptions)
			out.subscriptions_enabled = value == "true";
		else if (key == kMaxShared)
			out.max_shared = whole(value);
		else if (key == kSharedSlots)
			out.shared_slots = whole(value);
		else if (key == kMaxOwn)
			out.max_own = whole(value);
		else if (key == kMaxAddresses)
			out.max_addresses = whole(value);
	}
	return out;
}

std::string check_service_settings(const ServiceSettings &settings, const PlacementPolicy &boot) {
	for (std::string why : { in_range("max_shared", settings.max_shared, 0, kMaxFleetMachines),
				 in_range("shared_slots", settings.shared_slots, 1, kMaxSlots),
				 in_range("max_own", settings.max_own, 0, kMaxFleetMachines),
				 in_range("max_addresses", settings.max_addresses, 0, kMaxFleetMachines) })
		if (!why.empty())
			return why;
	if (settings.max_addresses && *settings.max_addresses > 0 && boot.max_addresses <= 0)
		return "max_addresses needs a deployment that runs static addresses "
			   "(ONNET_FLEET_MAX_ADDRESSES above 0)";
	return {};
}

void write_service_settings(db::Database &db, const ServiceSettings &settings) {
	db::Transaction tx(db);
	const auto put = [&db](const char *key, const std::optional<std::string> &value) {
		if (!value) {
			db.exec("DELETE FROM service_meta WHERE key = $1;", { BindValue(std::string(key)) });
			return;
		}
		db.exec("INSERT INTO service_meta (key, value) VALUES ($1, $2) "
				"ON CONFLICT (key) DO UPDATE SET value = excluded.value;",
				{ BindValue(std::string(key)), BindValue(*value) });
	};
	const auto number = [](const std::optional<int> &value) -> std::optional<std::string> {
		if (!value)
			return std::nullopt;
		return std::to_string(*value);
	};
	put(kSubscriptions, std::string(settings.subscriptions_enabled ? "true" : "false"));
	put(kMaxShared, number(settings.max_shared));
	put(kSharedSlots, number(settings.shared_slots));
	put(kMaxOwn, number(settings.max_own));
	put(kMaxAddresses, number(settings.max_addresses));
	tx.commit();
}

PlacementPolicy live_placement(const PlacementPolicy &boot, const ServiceSettings &settings) {
	PlacementPolicy out = boot;
	if (settings.max_shared)
		out.max_shared = *settings.max_shared;
	if (settings.shared_slots)
		out.shared_slots = *settings.shared_slots;
	if (settings.max_own)
		out.max_own = *settings.max_own;
	if (settings.max_addresses && boot.max_addresses > 0)
		out.max_addresses = *settings.max_addresses;
	// A row an older range let through falls back to the boot value rather than a policy placement
	// refuses.
	if (!check_placement_policy(out).empty())
		return boot;
	return out;
}

PlacementPolicy live_placement(db::Database &db, const PlacementPolicy &boot) {
	return live_placement(boot, read_service_settings(db));
}

bool subscriptions_enabled(db::Database &db) {
	return read_service_settings(db).subscriptions_enabled;
}

} // namespace opennova::novaworld_server::hosted
