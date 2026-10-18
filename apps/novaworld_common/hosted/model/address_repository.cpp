#include "address_repository.h"

#include "address_names.h"
#include "billing/billing_repository.h"
#include "billing/entitlement.h"
#include "hosted_repository.h"

#include <array>
#include <utility>

namespace opennova::novaworld_server::hosted {

namespace {

using db::BindValue;

constexpr std::array<const char *, 6> kStateNames = {
	"allocating", "ready", "lapsed", "releasing", "released", "failed",
};

constexpr const char *kAddressColumns =
		"id, owner_user_id, name, source, COALESCE(stripe_subscription_id, ''), state, allocation_id, public_ip, "
		"dns_name, dns_change_id, dns_state, COALESCE(extract(epoch FROM release_after)::bigint, 0), lapse_cause, "
		"reason, created_at, ready_at, lapsed_at, released_at";

constexpr const char *kHeldSql = "state IN ('allocating', 'ready', 'lapsed', 'releasing')";

HostedAddress row_to_address(const db::Row &r) {
	HostedAddress a;
	a.id = r.as_int(0).value_or(0);
	a.owner_user_id = r.as_int(1);
	a.name = r.as_text(2).value_or("");
	a.source = r.as_text(3).value_or("");
	a.stripe_subscription_id = r.as_text(4).value_or("");
	a.state = address_state_from_name(r.as_text(5).value_or("")).value_or(AddressState::Failed);
	a.allocation_id = r.as_text(6).value_or("");
	a.public_ip = r.as_text(7).value_or("");
	a.dns_name = r.as_text(8).value_or("");
	a.dns_change_id = r.as_text(9).value_or("");
	a.dns_state = r.as_text(10).value_or("");
	a.release_after = r.as_int(11).value_or(0);
	a.lapse_cause = r.as_text(12).value_or("");
	a.reason = r.as_text(13).value_or("");
	a.created_at = r.as_text(14).value_or("");
	a.ready_at = r.as_text(15).value_or("");
	a.lapsed_at = r.as_text(16).value_or("");
	a.released_at = r.as_text(17).value_or("");
	return a;
}

std::vector<HostedAddress> query_addresses(
		db::Database &db, const std::string &tail, const std::vector<BindValue> &binds) {
	std::vector<HostedAddress> out;
	for (const auto &r : db.query(std::string("SELECT ") + kAddressColumns + " FROM hosted_addresses " + tail, binds))
		out.push_back(row_to_address(r));
	return out;
}

bool name_taken(db::Database &db, const std::string &name, int64_t other_than = 0) {
	return !db.query("SELECT 1 FROM hosted_addresses WHERE name = $1 AND id <> $2;",
						 { BindValue(name), BindValue(other_than) })
					.empty();
}

// The refusal of `name` before any row is written, or nullopt.
std::optional<NameResult> refuse_name(db::Database &db, const std::string &name, int64_t other_than = 0) {
	NameResult out;
	out.why = address_name_refusal(name);
	if (!out.why.empty()) {
		out.outcome = NameOutcome::InvalidName;
		return out;
	}
	if (name_taken(db, name, other_than)) {
		out.outcome = NameOutcome::NameTaken;
		return out;
	}
	return std::nullopt;
}

bool active_player(db::Database &db, int64_t user_id) {
	return !db.query("SELECT 1 FROM players WHERE id = $1 AND kind = 'player' AND account_status = 'active';",
						  { BindValue(user_id) })
					.empty();
}

int64_t insert_address(db::Database &db, int64_t owner, const std::string &name, const char *source,
		const std::optional<std::string> &subscription) {
	const auto rows = db.query("INSERT INTO hosted_addresses (owner_user_id, name, source, stripe_subscription_id) "
							   "VALUES ($1, $2, $3, $4) RETURNING id;",
			{ BindValue(owner), BindValue(name), BindValue(std::string(source)),
					subscription ? BindValue(*subscription) : BindValue(std::monostate{}) });
	return rows.empty() ? 0 : rows.front().as_int(0).value_or(0);
}

// The row's move from one of `from` to `to` with `set` (more SET clauses, its
// binds from $3 on); false when it was in none of them.
bool move(db::Database &db, int64_t id, const char *from, const char *to, const std::string &set = {},
		std::vector<BindValue> binds = {}) {
	std::vector<BindValue> all = { BindValue(std::string(to)), BindValue(id) };
	for (BindValue &b : binds) all.push_back(std::move(b));
	db.exec(std::string("UPDATE hosted_addresses SET state = $1, updated_at = now()") + (set.empty() ? "" : ", " + set) +
					" WHERE id = $2 AND state IN (" + from + ");",
			all);
	return db.changes() > 0;
}

} // namespace

const char *address_state_name(AddressState state) {
	const auto index = static_cast<size_t>(state);
	return index < kStateNames.size() ? kStateNames[index] : "failed";
}

std::optional<AddressState> address_state_from_name(std::string_view name) {
	for (size_t i = 0; i < kStateNames.size(); ++i) {
		if (name == kStateNames[i]) return static_cast<AddressState>(i);
	}
	return std::nullopt;
}

bool address_is_held(AddressState state) {
	return state == AddressState::Allocating || state == AddressState::Ready || state == AddressState::Lapsed ||
			state == AddressState::Releasing;
}

std::optional<HostedAddress> find_address(db::Database &db, int64_t id) {
	auto rows = query_addresses(db, "WHERE id = $1;", { BindValue(id) });
	if (rows.empty()) return std::nullopt;
	return std::move(rows.front());
}

std::vector<HostedAddress> addresses_of(db::Database &db, int64_t owner_user_id) {
	return query_addresses(db,
			std::string("WHERE owner_user_id = $1 ORDER BY (") + kHeldSql + ") DESC, CASE WHEN " + kHeldSql +
					" THEN id ELSE -id END;",
			{ BindValue(owner_user_id) });
}

std::vector<HostedAddress> list_addresses(db::Database &db) {
	return query_addresses(db, std::string("ORDER BY (") + kHeldSql + ") DESC, id DESC;", {});
}

std::vector<HostedAddress> addresses_in(db::Database &db, AddressState state) {
	return query_addresses(db, "WHERE state = $1 ORDER BY id;", { BindValue(std::string(address_state_name(state))) });
}

int count_held_addresses(db::Database &db) {
	const auto rows = db.query(std::string("SELECT COUNT(*) FROM hosted_addresses WHERE ") + kHeldSql + ";");
	return rows.empty() ? 0 : static_cast<int>(rows.front().as_int(0).value_or(0));
}

std::vector<std::string> unnamed_address_subscriptions(db::Database &db, int64_t owner_user_id, int64_t now) {
	std::vector<std::string> out;
	// subscriptions_of answers newest first: the oldest is named first.
	const std::vector<billing::SubscriptionRow> rows = billing::subscriptions_of(db, owner_user_id);
	for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
		if (it->item != billing::ItemKind::StaticAddress || !billing::subscription_grants(*it, now)) continue;
		if (db.query("SELECT 1 FROM hosted_addresses WHERE stripe_subscription_id = $1;",
					  { BindValue(it->stripe_subscription_id) })
						.empty())
			out.push_back(it->stripe_subscription_id);
	}
	return out;
}

NameResult name_subscription_address(db::Database &db, int64_t owner_user_id, const std::string &name, int64_t now) {
	NameResult out;
	if (!active_player(db, owner_user_id)) {
		out.outcome = NameOutcome::NoSuchAccount;
		return out;
	}
	if (auto refused = refuse_name(db, name)) return *refused;
	const std::vector<std::string> unnamed = unnamed_address_subscriptions(db, owner_user_id, now);
	if (unnamed.empty()) {
		out.outcome = NameOutcome::NoAddress;
		return out;
	}
	out.id = insert_address(db, owner_user_id, name, "subscription", unnamed.front());
	return out;
}

NameResult add_comp_address(db::Database &db, int64_t owner_user_id, const std::string &name, int64_t now) {
	NameResult out;
	if (!active_player(db, owner_user_id)) {
		out.outcome = NameOutcome::NoSuchAccount;
		return out;
	}
	if (auto refused = refuse_name(db, name)) return *refused;
	// One more address than the account's own machines would hold nothing.
	const billing::Entitlement e = billing::entitlement_of(db, owner_user_id, now);
	if (e.address_grants + 1 > e.own_machines) {
		out.outcome = NameOutcome::AddressLimit;
		return out;
	}
	out.id = insert_address(db, owner_user_id, name, "comp", std::nullopt);
	return out;
}

NameResult rename_address(db::Database &db, int64_t id, const std::string &name) {
	NameResult out;
	if (!find_address(db, id)) {
		out.outcome = NameOutcome::NoSuchAccount;
		return out;
	}
	if (auto refused = refuse_name(db, name, id)) return *refused;
	db.exec("UPDATE hosted_addresses SET name = $1, updated_at = now() WHERE id = $2;", { BindValue(name), BindValue(id) });
	out.id = id;
	return out;
}

bool record_allocation(db::Database &db, int64_t id, const std::string &allocation_id, const std::string &public_ip) {
	db.exec("UPDATE hosted_addresses SET allocation_id = $1, public_ip = $2, updated_at = now() "
			"WHERE id = $3 AND state = 'allocating';",
			{ BindValue(allocation_id), BindValue(public_ip), BindValue(id) });
	return db.changes() > 0;
}

bool record_dns_upsert(db::Database &db, int64_t id, const std::string &dns_name, const std::string &change_id) {
	db.exec("UPDATE hosted_addresses SET state = 'ready', dns_name = $1, dns_change_id = $2, dns_state = 'insync', "
			"ready_at = COALESCE(ready_at, now()), updated_at = now() "
			"WHERE id = $3 AND state IN ('allocating', 'ready') AND public_ip <> '';",
			{ BindValue(dns_name), BindValue(change_id), BindValue(id) });
	return db.changes() > 0;
}

bool fail_address(db::Database &db, int64_t id, const std::string &reason) {
	return move(db, id, "'allocating'", "failed", "reason = $3", { BindValue(reason) });
}

bool record_dns_delete(db::Database &db, int64_t id, const std::string &change_id) {
	db.exec("UPDATE hosted_addresses SET dns_change_id = $1, dns_state = 'pending', updated_at = now() "
			"WHERE id = $2 AND state = 'releasing';",
			{ BindValue(change_id), BindValue(id) });
	return db.changes() > 0;
}

bool record_dns_gone(db::Database &db, int64_t id, int64_t release_after) {
	db.exec("UPDATE hosted_addresses SET dns_name = '', dns_state = 'none', "
			"release_after = to_timestamp($1::float8), updated_at = now() WHERE id = $2 AND state = 'releasing';",
			{ BindValue(release_after), BindValue(id) });
	return db.changes() > 0;
}

bool mark_released(db::Database &db, int64_t id) {
	return move(db, id, "'releasing'", "released", "released_at = now()");
}

bool restore_address(db::Database &db, int64_t id) {
	return move(db, id, "'releasing'", "ready", "lapse_cause = '', reason = '', release_after = NULL");
}

bool lapse_address(db::Database &db, int64_t id, const std::string &cause, const std::string &reason) {
	return move(db, id, "'allocating', 'ready', 'failed'", "lapsed", "lapse_cause = $3, reason = $4, lapsed_at = now()",
			{ BindValue(cause), BindValue(reason) });
}

bool begin_release(db::Database &db, int64_t id) { return move(db, id, "'lapsed'", "releasing"); }

std::optional<int64_t> server_of_address(db::Database &db, int64_t id) {
	const auto rows = db.query(
			std::string("SELECT id FROM hosted_servers WHERE address_id = $1 AND ") + kLiveServerSql + " LIMIT 1;",
			{ BindValue(id) });
	if (rows.empty()) return std::nullopt;
	return rows.front().as_int(0);
}

void replace_address_orphans(db::Database &db, const std::vector<AddressOrphan> &orphans) {
	db::Transaction tx(db);
	db.exec("DELETE FROM hosted_address_orphans;");
	for (const AddressOrphan &o : orphans) {
		db.exec("INSERT INTO hosted_address_orphans (allocation_id, public_ip, address_id) VALUES ($1, $2, $3) "
				"ON CONFLICT (allocation_id) DO NOTHING;",
				{ BindValue(o.allocation_id), BindValue(o.public_ip), BindValue(o.address_id) });
	}
	tx.commit();
}

std::vector<AddressOrphan> address_orphans(db::Database &db) {
	std::vector<AddressOrphan> out;
	for (const auto &r : db.query("SELECT allocation_id, public_ip, address_id, seen_at FROM hosted_address_orphans "
								  "ORDER BY allocation_id;")) {
		AddressOrphan o;
		o.allocation_id = r.as_text(0).value_or("");
		o.public_ip = r.as_text(1).value_or("");
		o.address_id = r.as_int(2).value_or(0);
		o.seen_at = r.as_text(3).value_or("");
		out.push_back(std::move(o));
	}
	return out;
}

} // namespace opennova::novaworld_server::hosted
