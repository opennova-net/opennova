#include "sold_capacity.h"

#include <algorithm>
#include <utility>

namespace opennova::novaworld_server::hosted {

namespace {

// A granting row of `item` as one more subscription of it reads at `now`.
billing::SubscriptionRow one_more(billing::ItemKind item, int tier, int64_t now) {
	billing::CheckoutReservation r;
	r.item = item;
	r.tier = tier;
	return billing::reservation_row(r, now);
}

} // namespace

Capacity &Capacity::operator+=(const Capacity &other) {
	shared += other.shared;
	own += other.own;
	addresses += other.addresses;
	return *this;
}

Capacity operator-(const Capacity &a, const Capacity &b) {
	Capacity out;
	out.shared = a.shared - b.shared;
	out.own = a.own - b.own;
	out.addresses = a.addresses - b.addresses;
	return out;
}

Capacity held_by(const billing::Entitlement &e) {
	Capacity out;
	// An own machine past the account's servers would run nothing: it holds no
	// more own machines than servers, and each it holds is a shared place less.
	out.own = std::min(e.own_machines, e.max_servers);
	out.shared = e.max_servers - out.own;
	// Every address granted is one the cloud holds, used or not.
	out.addresses = e.address_grants;
	return out;
}

Capacity sellable_of(const PlacementPolicy &policy) {
	Capacity out;
	out.shared = static_cast<int64_t>(policy.max_shared) * policy.shared_slots;
	out.own = policy.max_own;
	out.addresses = policy.max_addresses;
	return out;
}

bool sold_exceeds_capacity(const Capacity &sold, const Capacity &sellable) {
	return sold.shared > sellable.shared || sold.own > sellable.own || sold.addresses > sellable.addresses;
}

bool fits(const Capacity &sold, const Capacity &more, const Capacity &sellable) {
	const auto part = [](int64_t has, int64_t grows, int64_t cap) { return grows <= 0 || has + grows <= cap; };
	return part(sold.shared, more.shared, sellable.shared) && part(sold.own, more.own, sellable.own) &&
			part(sold.addresses, more.addresses, sellable.addresses);
}

Capacity added_by(const billing::AccountFacts &account, const std::vector<billing::SubscriptionRow> &subscriptions,
		billing::ItemKind item, int tier, int64_t now) {
	std::vector<billing::SubscriptionRow> after = subscriptions;
	after.push_back(one_more(item, tier, now));
	return held_by(billing::entitlement_from(account, after, now)) -
			held_by(billing::entitlement_from(account, subscriptions, now));
}

Capacity added_by_new_account(billing::ItemKind item, int tier, int64_t now) {
	billing::AccountFacts account;
	std::vector<billing::SubscriptionRow> subscriptions;
	// An add-on is bought beside a tier: the account a new one comes to has the
	// lowest.
	if (item != billing::ItemKind::Tier) subscriptions.push_back(one_more(billing::ItemKind::Tier, 1, now));
	return added_by(account, subscriptions, item, tier, now);
}

Capacity sold_now(db::Database &db, int64_t now, const std::string &except_session) {
	const auto subscriptions = billing::subscriptions_by_account(db);
	const auto grants = billing::comp_grants_by_account(db);
	const auto reservations = billing::live_reservations_by_account(db);
	const auto addresses = billing::comp_addresses_by_account(db);
	Capacity out;
	for (const auto &r : db.query("SELECT id, account_status FROM players WHERE kind = 'player';")) {
		const int64_t user = r.as_int(0).value_or(0);
		billing::AccountFacts facts;
		facts.account_status = r.as_text(1).value_or("");
		if (const auto g = grants.find(user); g != grants.end()) {
			facts.comp_servers = g->second.servers;
			facts.comp_own_machines = g->second.own_machines;
		}
		if (const auto a = addresses.find(user); a != addresses.end()) facts.comp_addresses = a->second;
		std::vector<billing::SubscriptionRow> rows;
		if (const auto s = subscriptions.find(user); s != subscriptions.end()) rows = s->second;
		if (const auto held = reservations.find(user); held != reservations.end()) {
			for (const billing::CheckoutReservation &c : held->second) {
				if (except_session.empty() || c.checkout_session != except_session)
					rows.push_back(billing::reservation_row(c, now));
			}
		}
		out += held_by(billing::entitlement_from(facts, rows, now));
	}
	return out;
}

AccountHoldings holdings_of(db::Database &db, int64_t user_id, int64_t now, const std::string &except_session) {
	AccountHoldings out;
	const auto rows = db.query("SELECT kind, account_status FROM players WHERE id = $1;", { db::BindValue(user_id) });
	// No such account: one that holds nothing (entitlement_from's other kinds).
	out.facts.kind = rows.empty() ? std::string() : rows.front().as_text(0).value_or("");
	if (!rows.empty()) out.facts.account_status = rows.front().as_text(1).value_or("");
	const billing::CompGrants grants = billing::comp_grants_of(db, user_id);
	out.facts.comp_servers = grants.servers;
	out.facts.comp_own_machines = grants.own_machines;
	out.facts.comp_addresses = billing::comp_addresses_of(db, user_id);
	out.subscriptions = billing::subscriptions_of(db, user_id);
	for (const billing::CheckoutReservation &c : billing::live_reservations_of(db, user_id)) {
		if (except_session.empty() || c.checkout_session != except_session)
			out.subscriptions.push_back(billing::reservation_row(c, now));
	}
	return out;
}

} // namespace opennova::novaworld_server::hosted
