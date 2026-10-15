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
	} else if (row.item != ItemKind::OwnMachine) {
		return false;
	}
	return row.current_period_end && now < *row.current_period_end + kPeriodNetSeconds;
}

Entitlement entitlement_from(const AccountFacts &account, const std::vector<SubscriptionRow> &subscriptions,
		int64_t now) {
	Entitlement e;
	e.comp_servers = std::clamp(account.comp_servers, 0, kMaxCompServers);
	if (const SubscriptionRow *shown = shown_tier(subscriptions, now)) {
		e.status = shown->status;
		e.current_period_end = shown->current_period_end;
		e.cancel_at_period_end = shown->cancel_at_period_end;
	}
	if (account.kind != "player" || account.account_status != "active") return e;

	int64_t own_quantity = 0;
	bool past_due = false;
	for (const SubscriptionRow &s : subscriptions) {
		if (!subscription_grants(s, now)) continue;
		past_due = past_due || s.status == "past_due";
		if (s.item == ItemKind::Tier)
			e.tier = std::max(e.tier, s.tier);
		else
			own_quantity += std::max<int64_t>(s.quantity, 0);
	}
	const int tier_servers = kTierServers[e.tier];
	e.max_servers = std::max(e.comp_servers, tier_servers);
	if (e.tier > 0 && tier_servers >= e.comp_servers)
		e.source = "tier";
	else if (e.comp_servers > 0)
		e.source = "comp";
	// The add-on moves one of the tier's servers onto a machine of its own: no
	// tier, nothing to move.
	e.own_machines = e.tier > 0 ? static_cast<int>(std::min<int64_t>(own_quantity, int64_t{ 1 } << 16)) : 0;
	e.can_start = e.max_servers > 0 && !past_due;
	return e;
}

Entitlement entitlement_of(db::Database &db, int64_t user_id, int64_t now) {
	const auto rows = db.query("SELECT kind, account_status, comp_servers FROM players WHERE id = $1;",
			{ db::BindValue(user_id) });
	if (rows.empty()) return Entitlement{};
	AccountFacts account;
	account.kind = rows.front().as_text(0).value_or("");
	account.account_status = rows.front().as_text(1).value_or("");
	account.comp_servers = static_cast<int>(rows.front().as_int(2).value_or(0));
	return entitlement_from(account, subscriptions_of(db, user_id), now);
}

int64_t unix_now() {
	using namespace std::chrono;
	return static_cast<int64_t>(duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
}

} // namespace opennova::novaworld_server::billing
