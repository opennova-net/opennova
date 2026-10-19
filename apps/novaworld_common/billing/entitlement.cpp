#include "entitlement.h"

#include "tiers.h"

#include <algorithm>
#include <chrono>

namespace opennova::novaworld_server::billing {

namespace {

// The tier subscription the account's page shows: the granting one of the
// highest tier, else the newest (`subscriptions` come newest first).
const SubscriptionRow *shown_tier(const std::vector<SubscriptionRow> &subscriptions, int64_t now) {
	const SubscriptionRow *best = nullptr;
	for (const SubscriptionRow &s : subscriptions) {
		if (s.item != ItemKind::Tier || !subscription_grants(s, now)) continue;
		if (!best || s.tier > best->tier) best = &s;
	}
	if (best) return best;
	for (const SubscriptionRow &s : subscriptions) {
		if (s.item == ItemKind::Tier) return &s;
	}
	return nullptr;
}

} // namespace

bool status_grants(std::string_view status) {
	return status == "active" || status == "trialing" || status == "past_due";
}

bool subscription_grants(const SubscriptionRow &row, int64_t now) {
	if (!status_grants(row.status)) return false;
	if (row.item == ItemKind::Tier) {
		if (row.tier < 1 || row.tier > kTierCount) return false;
	} else if (row.item != ItemKind::OwnMachine && row.item != ItemKind::StaticAddress) {
		return false;
	}
	return row.current_period_end && now < *row.current_period_end + kPeriodNetSeconds;
}

Entitlement entitlement_from(const AccountFacts &account, const std::vector<SubscriptionRow> &subscriptions,
		int64_t now) {
	Entitlement e;
	e.comp_servers = std::clamp(account.comp_servers, 0, kMaxCompServers);
	e.comp_own_machines = std::clamp(account.comp_own_machines, 0, kMaxCompOwnMachines);
	e.comp_addresses = std::max(account.comp_addresses, 0);
	if (const SubscriptionRow *shown = shown_tier(subscriptions, now)) {
		e.status = shown->status;
		e.current_period_end = shown->current_period_end;
		e.cancel_at_period_end = shown->cancel_at_period_end;
	}
	if (account.kind != "player" || account.account_status != "active") return e;

	int64_t own_quantity = 0;
	int64_t address_quantity = 0;
	bool past_due = false;
	for (const SubscriptionRow &s : subscriptions) {
		if (!subscription_grants(s, now)) continue;
		past_due = past_due || s.status == "past_due";
		if (s.item == ItemKind::Tier)
			e.tier = std::max(e.tier, s.tier);
		else if (s.item == ItemKind::OwnMachine)
			own_quantity += std::max<int64_t>(s.quantity, 0);
		else
			address_quantity += std::max<int64_t>(s.quantity, 0);
	}
	const int tier_servers = kTierServers[e.tier];
	e.shared_servers = std::max(e.comp_servers, tier_servers);
	if (e.tier > 0 && tier_servers >= e.comp_servers)
		e.source = "tier";
	else if (e.comp_servers > 0)
		e.source = "comp";
	// A dedicated machine is one more server, on a machine of its own, a tier or
	// not; an admin's are added to the paid ones.
	e.own_machines = static_cast<int>(std::min<int64_t>(own_quantity + e.comp_own_machines, int64_t{ 1 } << 16));
	e.max_servers = e.shared_servers + e.own_machines;
	// An address is its dedicated machine's: one past them is held, and used by none.
	e.address_grants = static_cast<int>(std::min<int64_t>(address_quantity + e.comp_addresses, int64_t{ 1 } << 16));
	e.static_addresses = std::min(e.address_grants, e.own_machines);
	e.can_start = e.max_servers > 0 && !past_due;
	return e;
}

Entitlement entitlement_of(db::Database &db, int64_t user_id, int64_t now) {
	const auto rows = db.query("SELECT kind, account_status FROM players WHERE id = $1;", { db::BindValue(user_id) });
	if (rows.empty()) return Entitlement{};
	AccountFacts account;
	account.kind = rows.front().as_text(0).value_or("");
	account.account_status = rows.front().as_text(1).value_or("");
	const CompGrants grants = comp_grants_of(db, user_id);
	account.comp_servers = grants.servers;
	account.comp_own_machines = grants.own_machines;
	account.comp_addresses = comp_addresses_of(db, user_id);
	return entitlement_from(account, subscriptions_of(db, user_id), now);
}

bool admin_granted(const Entitlement &e) {
	return e.comp_servers > 0 || e.comp_own_machines > 0 || e.comp_addresses > 0;
}

std::vector<SubscriptionRow> add_ons_to_end(const std::vector<SubscriptionRow> &subscriptions, int64_t now) {
	std::vector<SubscriptionRow> out;
	for (const SubscriptionRow &s : subscriptions) {
		if (s.item == ItemKind::Tier && subscription_grants(s, now)) return {};
		if (s.item == ItemKind::Unknown && status_grants(s.status) && s.current_period_end &&
				now < *s.current_period_end + kPeriodNetSeconds)
			return {};
		if ((s.item == ItemKind::OwnMachine || s.item == ItemKind::StaticAddress) && status_grants(s.status) &&
				!s.cancel_at_period_end)
			out.push_back(s);
	}
	return out;
}

int64_t unix_now() {
	using namespace std::chrono;
	return static_cast<int64_t>(duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
}

} // namespace opennova::novaworld_server::billing
