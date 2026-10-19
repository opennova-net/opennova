#include "billing_repository.h"

#include "entitlement.h"

#include <base/io/strutil.h>
#include <base/os_random/os_random.h>

#include <algorithm>
#include <array>
#include <utility>

namespace opennova::novaworld_server::billing {

namespace {

using db::BindValue;

BindValue optional_int(std::optional<int64_t> value) {
	if (!value) return BindValue(std::monostate{});
	return BindValue(*value);
}

constexpr const char *kCustomerColumns =
		"user_id, stripe_customer_id, livemode, open_checkout_session, created_at, synced_at";

CustomerRow row_to_customer(const db::Row &r) {
	CustomerRow c;
	c.user_id = r.as_int(0).value_or(0);
	c.stripe_customer_id = r.as_text(1).value_or("");
	c.livemode = r.as_int(2).value_or(0) != 0;
	c.open_checkout_session = r.as_text(3).value_or("");
	c.created_at = r.as_text(4).value_or("");
	c.synced_at = r.as_text(5).value_or("");
	return c;
}

std::vector<CustomerRow> query_customers(db::Database &db, const std::string &tail, const std::vector<BindValue> &binds) {
	std::vector<CustomerRow> out;
	for (const auto &r : db.query(std::string("SELECT ") + kCustomerColumns + " FROM billing_customers " + tail, binds))
		out.push_back(row_to_customer(r));
	return out;
}

constexpr const char *kSubscriptionColumns =
		"stripe_subscription_id, stripe_customer_id, user_id, livemode, status, item, price_id, tier, quantity, "
		"extract(epoch FROM current_period_end)::bigint, cancel_at_period_end, status_since, updated_at";

SubscriptionRow row_to_subscription(const db::Row &r) {
	SubscriptionRow s;
	s.stripe_subscription_id = r.as_text(0).value_or("");
	s.stripe_customer_id = r.as_text(1).value_or("");
	s.user_id = r.as_int(2);
	s.livemode = r.as_int(3).value_or(0) != 0;
	s.status = r.as_text(4).value_or("");
	s.item = item_kind_from_name(r.as_text(5).value_or(""));
	s.price_id = r.as_text(6).value_or("");
	s.tier = static_cast<int>(r.as_int(7).value_or(0));
	s.quantity = r.as_int(8).value_or(0);
	s.current_period_end = r.as_int(9);
	s.cancel_at_period_end = r.as_int(10).value_or(0) != 0;
	s.status_since = r.as_text(11).value_or("");
	s.updated_at = r.as_text(12).value_or("");
	return s;
}

std::vector<SubscriptionRow> query_subscriptions(
		db::Database &db, const std::string &tail, const std::vector<BindValue> &binds) {
	std::vector<SubscriptionRow> out;
	for (const auto &r :
			db.query(std::string("SELECT ") + kSubscriptionColumns + " FROM billing_subscriptions " + tail, binds))
		out.push_back(row_to_subscription(r));
	return out;
}

size_t count_of(const std::vector<db::Row> &rows) {
	return rows.empty() ? 0 : static_cast<size_t>(rows.front().as_int(0).value_or(0));
}

} // namespace

std::string instance_id(db::Database &db) {
	const auto read = [&db]() -> std::string {
		const auto rows = db.query("SELECT value FROM service_meta WHERE key = 'instance_id';");
		return rows.empty() ? std::string() : rows.front().as_text(0).value_or("");
	};
	if (std::string id = read(); !id.empty()) return id;
	std::array<uint8_t, 16> raw{};
	os_random_bytes(raw.data(), raw.size());
	db.exec("INSERT INTO service_meta (key, value) VALUES ('instance_id', $1) ON CONFLICT (key) DO NOTHING;",
			{ BindValue(strutil::bytes_to_hex(raw.data(), raw.size())) });
	return read();
}

std::optional<CustomerRow> customer_of(db::Database &db, int64_t user_id) {
	auto rows = query_customers(db, "WHERE user_id = $1;", { BindValue(user_id) });
	if (rows.empty()) return std::nullopt;
	return std::move(rows.front());
}

std::optional<CustomerRow> customer_by_id(db::Database &db, const std::string &stripe_customer_id) {
	auto rows = query_customers(db, "WHERE stripe_customer_id = $1;", { BindValue(stripe_customer_id) });
	if (rows.empty()) return std::nullopt;
	return std::move(rows.front());
}

std::optional<int64_t> user_of_customer(db::Database &db, const std::string &stripe_customer_id) {
	const auto row = customer_by_id(db, stripe_customer_id);
	if (!row) return std::nullopt;
	return row->user_id;
}

bool insert_customer(db::Database &db, int64_t user_id, const std::string &stripe_customer_id, bool livemode) {
	db.exec("INSERT INTO billing_customers (user_id, stripe_customer_id, livemode) VALUES ($1, $2, $3) "
			"ON CONFLICT DO NOTHING;",
			{ BindValue(user_id), BindValue(stripe_customer_id), BindValue(db::Bool{ livemode }) });
	return db.changes() > 0;
}

std::vector<CustomerRow> list_customers(db::Database &db) { return query_customers(db, "ORDER BY user_id;", {}); }

void set_open_checkout_session(db::Database &db, int64_t user_id, const std::string &session_id) {
	db.exec("UPDATE billing_customers SET open_checkout_session = $1 WHERE user_id = $2;",
			{ BindValue(session_id), BindValue(user_id) });
}

void mark_customer_synced(db::Database &db, const std::string &stripe_customer_id) {
	db.exec("UPDATE billing_customers SET synced_at = now() WHERE stripe_customer_id = $1;",
			{ BindValue(stripe_customer_id) });
}

void delete_customer(db::Database &db, const std::string &stripe_customer_id) {
	db.exec("DELETE FROM billing_customers WHERE stripe_customer_id = $1;", { BindValue(stripe_customer_id) });
}

const char *item_kind_name(ItemKind kind) {
	switch (kind) {
		case ItemKind::Tier: return "tier";
		case ItemKind::OwnMachine: return "own_machine";
		case ItemKind::StaticAddress: return "static_address";
		case ItemKind::Unknown: break;
	}
	return "unknown";
}

ItemKind item_kind_from_name(std::string_view name) {
	if (name == "tier") return ItemKind::Tier;
	if (name == "own_machine") return ItemKind::OwnMachine;
	if (name == "static_address") return ItemKind::StaticAddress;
	return ItemKind::Unknown;
}

void upsert_subscription(db::Database &db, const SubscriptionRow &row) {
	db.exec("INSERT INTO billing_subscriptions (stripe_subscription_id, stripe_customer_id, user_id, livemode, "
			"status, item, price_id, tier, quantity, current_period_end, cancel_at_period_end) "
			"VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, to_timestamp($10::float8), $11) "
			"ON CONFLICT (stripe_subscription_id) DO UPDATE SET "
			"stripe_customer_id = excluded.stripe_customer_id, user_id = excluded.user_id, "
			"livemode = excluded.livemode, status = excluded.status, item = excluded.item, "
			"price_id = excluded.price_id, tier = excluded.tier, quantity = excluded.quantity, "
			"current_period_end = excluded.current_period_end, "
			"cancel_at_period_end = excluded.cancel_at_period_end, "
			"status_since = CASE WHEN billing_subscriptions.status = excluded.status "
			"THEN billing_subscriptions.status_since ELSE now() END, "
			"updated_at = now();",
			{ BindValue(row.stripe_subscription_id), BindValue(row.stripe_customer_id), optional_int(row.user_id),
					BindValue(db::Bool{ row.livemode }), BindValue(row.status),
					BindValue(std::string(item_kind_name(row.item))), BindValue(row.price_id),
					BindValue(static_cast<int64_t>(row.tier)), BindValue(row.quantity),
					optional_int(row.current_period_end), BindValue(db::Bool{ row.cancel_at_period_end }) });
}

std::vector<SubscriptionRow> subscriptions_of(db::Database &db, int64_t user_id) {
	return query_subscriptions(db, "WHERE user_id = $1 ORDER BY updated_at DESC, stripe_subscription_id;",
			{ BindValue(user_id) });
}

std::vector<SubscriptionRow> subscriptions_of_customer(db::Database &db, const std::string &stripe_customer_id) {
	return query_subscriptions(db,
			"WHERE stripe_customer_id = $1 ORDER BY updated_at DESC, stripe_subscription_id;",
			{ BindValue(stripe_customer_id) });
}

std::optional<SubscriptionRow> find_subscription(db::Database &db, const std::string &stripe_subscription_id) {
	auto rows = query_subscriptions(db, "WHERE stripe_subscription_id = $1;", { BindValue(stripe_subscription_id) });
	if (rows.empty()) return std::nullopt;
	return std::move(rows.front());
}

std::vector<SubscriptionRow> list_subscriptions(db::Database &db) {
	return query_subscriptions(db, "ORDER BY status_since DESC, stripe_subscription_id;", {});
}

std::map<int64_t, std::vector<SubscriptionRow>> subscriptions_by_account(db::Database &db) {
	std::map<int64_t, std::vector<SubscriptionRow>> out;
	for (SubscriptionRow &row : query_subscriptions(db,
				 "WHERE user_id IS NOT NULL ORDER BY user_id, updated_at DESC, stripe_subscription_id;", {})) {
		const int64_t user = *row.user_id;
		out[user].push_back(std::move(row));
	}
	return out;
}

size_t end_subscriptions_of_customer(db::Database &db, const std::string &stripe_customer_id,
		const std::vector<std::string> &keep, const std::string &status) {
	size_t ended = 0;
	for (const SubscriptionRow &row : subscriptions_of_customer(db, stripe_customer_id)) {
		if (row.status == "canceled" || row.status == "incomplete_expired") continue;
		bool kept = false;
		for (const std::string &id : keep) kept = kept || id == row.stripe_subscription_id;
		if (kept) continue;
		db.exec("UPDATE billing_subscriptions SET status = $1, status_since = now(), updated_at = now() "
				"WHERE stripe_subscription_id = $2;",
				{ BindValue(status), BindValue(row.stripe_subscription_id) });
		ended += static_cast<size_t>(db.changes());
	}
	return ended;
}

bool record_event(db::Database &db, const std::string &event_id, const std::string &type, bool livemode,
		const std::string &customer_id, std::optional<int64_t> stripe_created) {
	db.exec("INSERT INTO billing_events (event_id, type, livemode, customer_id, stripe_created, processed_at, "
			"outcome) VALUES ($1, $2, $3, $4::text, to_timestamp($5::float8), "
			"CASE WHEN $4::text = '' THEN now() END, CASE WHEN $4::text = '' THEN 'ignored' ELSE '' END) "
			"ON CONFLICT (event_id) DO NOTHING;",
			{ BindValue(event_id), BindValue(type), BindValue(db::Bool{ livemode }), BindValue(customer_id),
					optional_int(stripe_created) });
	return db.changes() > 0;
}

std::vector<std::string> pending_customers(db::Database &db, size_t limit) {
	std::vector<std::string> out;
	for (const auto &r : db.query("SELECT customer_id FROM billing_events "
								  "WHERE processed_at IS NULL AND customer_id <> '' "
								  "GROUP BY customer_id ORDER BY min(received_at), customer_id LIMIT $1;",
				 { BindValue(static_cast<int64_t>(limit)) })) {
		out.push_back(r.as_text(0).value_or(""));
	}
	return out;
}

size_t pending_event_count(db::Database &db) {
	return count_of(db.query("SELECT COUNT(*) FROM billing_events WHERE processed_at IS NULL;"));
}

int64_t oldest_pending_age_s(db::Database &db) {
	const auto rows = db.query("SELECT COALESCE(extract(epoch FROM now() - min(received_at))::bigint, 0) "
							   "FROM billing_events WHERE processed_at IS NULL;");
	return rows.empty() ? 0 : rows.front().as_int(0).value_or(0);
}

size_t mark_processed(db::Database &db, const std::string &customer_id, const std::string &outcome,
		const std::optional<std::string> &received_by) {
	db.exec("UPDATE billing_events SET processed_at = now(), outcome = $1 "
			"WHERE customer_id = $2 AND processed_at IS NULL "
			"AND ($3::text IS NULL OR received_at <= $3::timestamptz);",
			{ BindValue(outcome), BindValue(customer_id),
					received_by ? BindValue(*received_by) : BindValue(std::monostate{}) });
	return static_cast<size_t>(db.changes());
}

std::string database_clock(db::Database &db) {
	const auto rows = db.query("SELECT clock_timestamp()::text;");
	return rows.empty() ? std::string() : rows.front().as_text(0).value_or("");
}

size_t prune_events(db::Database &db, int days) {
	db.exec("DELETE FROM billing_events WHERE processed_at IS NOT NULL "
			"AND processed_at < now() - make_interval(secs => $1);",
			{ BindValue(static_cast<int64_t>(days) * 24 * 60 * 60) });
	return static_cast<size_t>(db.changes());
}

std::vector<EventRow> recent_events(db::Database &db, size_t limit) {
	std::vector<EventRow> out;
	for (const auto &r : db.query("SELECT event_id, type, customer_id, received_at, outcome, processed_at IS NOT NULL "
								  "FROM billing_events ORDER BY received_at DESC, event_id LIMIT $1;",
				 { BindValue(static_cast<int64_t>(limit)) })) {
		EventRow e;
		e.event_id = r.as_text(0).value_or("");
		e.type = r.as_text(1).value_or("");
		e.customer_id = r.as_text(2).value_or("");
		e.received_at = r.as_text(3).value_or("");
		e.outcome = r.as_text(4).value_or("");
		e.processed = r.as_int(5).value_or(0) != 0;
		out.push_back(std::move(e));
	}
	return out;
}

bool has_granting_subscription(db::Database &db, int64_t user_id, int64_t now) {
	for (const SubscriptionRow &row : subscriptions_of(db, user_id)) {
		if (subscription_grants(row, now)) return true;
	}
	return false;
}

namespace {

// A comp_grants row into `grants`.
void take_grant(CompGrants &grants, const std::string &item, int64_t quantity) {
	const int n = static_cast<int>(std::clamp<int64_t>(quantity, 0, 1 << 16));
	if (item == "servers")
		grants.servers = n;
	else if (item == "own_machine")
		grants.own_machines = n;
}

} // namespace

CompGrants comp_grants_of(db::Database &db, int64_t user_id) {
	CompGrants out;
	for (const auto &r : db.query("SELECT item, quantity FROM comp_grants WHERE user_id = $1;", { BindValue(user_id) }))
		take_grant(out, r.as_text(0).value_or(""), r.as_int(1).value_or(0));
	return out;
}

std::map<int64_t, CompGrants> comp_grants_by_account(db::Database &db) {
	std::map<int64_t, CompGrants> out;
	for (const auto &r : db.query("SELECT user_id, item, quantity FROM comp_grants;"))
		take_grant(out[r.as_int(0).value_or(0)], r.as_text(1).value_or(""), r.as_int(2).value_or(0));
	return out;
}

bool set_comp_grants(db::Database &db, int64_t user_id, const CompGrants &grants) {
	db::Transaction tx(db);
	if (db.query("SELECT 1 FROM players WHERE id = $1 AND kind = 'player' FOR UPDATE;", { BindValue(user_id) }).empty())
		return false;
	const auto put = [&](const char *item, int quantity) {
		if (quantity <= 0) {
			db.exec("DELETE FROM comp_grants WHERE user_id = $1 AND item = $2;",
					{ BindValue(user_id), BindValue(std::string(item)) });
			return;
		}
		db.exec("INSERT INTO comp_grants (user_id, item, quantity) VALUES ($1, $2, $3) "
				"ON CONFLICT (user_id, item) DO UPDATE SET quantity = excluded.quantity, updated_at = now();",
				{ BindValue(user_id), BindValue(std::string(item)), BindValue(static_cast<int64_t>(quantity)) });
	};
	put("servers", grants.servers);
	put("own_machine", grants.own_machines);
	tx.commit();
	return true;
}

namespace {

constexpr const char *kCompAddressesSql =
		"FROM hosted_addresses WHERE source = 'comp' AND state IN ('allocating', 'ready')";
constexpr const char *kNamedSubscriptionsSql =
		"FROM hosted_addresses WHERE source = 'subscription' AND state IN ('allocating', 'ready') "
		"AND stripe_subscription_id IS NOT NULL AND owner_user_id IS NOT NULL";

} // namespace

int comp_addresses_of(db::Database &db, int64_t user_id) {
	return static_cast<int>(
			count_of(db.query(std::string("SELECT COUNT(*) ") + kCompAddressesSql + " AND owner_user_id = $1;",
					{ BindValue(user_id) })));
}

std::map<int64_t, int> comp_addresses_by_account(db::Database &db) {
	std::map<int64_t, int> out;
	for (const auto &r : db.query(std::string("SELECT owner_user_id, COUNT(*) ") + kCompAddressesSql +
				 " AND owner_user_id IS NOT NULL GROUP BY owner_user_id;"))
		out[r.as_int(0).value_or(0)] = static_cast<int>(r.as_int(1).value_or(0));
	return out;
}

std::set<std::string> named_address_subscriptions(db::Database &db, int64_t user_id) {
	std::set<std::string> out;
	for (const auto &r : db.query(std::string("SELECT stripe_subscription_id ") + kNamedSubscriptionsSql +
				 " AND owner_user_id = $1;",
				 { BindValue(user_id) }))
		out.insert(r.as_text(0).value_or(""));
	return out;
}

std::map<int64_t, std::set<std::string>> named_address_subscriptions_by_account(db::Database &db) {
	std::map<int64_t, std::set<std::string>> out;
	for (const auto &r : db.query(std::string("SELECT owner_user_id, stripe_subscription_id ") + kNamedSubscriptionsSql + ";"))
		out[r.as_int(0).value_or(0)].insert(r.as_text(1).value_or(""));
	return out;
}

namespace {

constexpr const char *kReservationColumns = "id, user_id, item, tier, checkout_session";

std::vector<CheckoutReservation> query_reservations(
		db::Database &db, const std::string &tail, const std::vector<BindValue> &binds) {
	std::vector<CheckoutReservation> out;
	for (const auto &r : db.query(std::string("SELECT ") + kReservationColumns +
					 " FROM billing_checkout_reservations WHERE expires_at > now() " + tail,
				 binds)) {
		CheckoutReservation c;
		c.id = r.as_int(0).value_or(0);
		c.user_id = r.as_int(1).value_or(0);
		c.item = item_kind_from_name(r.as_text(2).value_or(""));
		c.tier = static_cast<int>(r.as_int(3).value_or(0));
		c.checkout_session = r.as_text(4).value_or("");
		out.push_back(std::move(c));
	}
	return out;
}

} // namespace

int64_t reserve_checkout(db::Database &db, int64_t user_id, ItemKind item, int tier, int64_t life_s) {
	db.exec("DELETE FROM billing_checkout_reservations WHERE expires_at <= now();");
	const auto rows = db.query("INSERT INTO billing_checkout_reservations (user_id, item, tier, expires_at) "
							   "VALUES ($1, $2, $3, now() + make_interval(secs => $4)) RETURNING id;",
			{ BindValue(user_id), BindValue(std::string(item_kind_name(item))), BindValue(static_cast<int64_t>(tier)),
					BindValue(life_s) });
	return rows.empty() ? 0 : rows.front().as_int(0).value_or(0);
}

void stamp_reservation(db::Database &db, int64_t id, const std::string &checkout_session) {
	db.exec("UPDATE billing_checkout_reservations SET checkout_session = $1 WHERE id = $2;",
			{ BindValue(checkout_session), BindValue(id) });
}

void drop_reservation(db::Database &db, int64_t id, int64_t user_id) {
	db.exec("DELETE FROM billing_checkout_reservations WHERE id = $1 AND user_id = $2;",
			{ BindValue(id), BindValue(user_id) });
}

size_t drop_session_reservation(db::Database &db, const std::string &checkout_session) {
	if (checkout_session.empty()) return 0;
	db.exec("DELETE FROM billing_checkout_reservations WHERE checkout_session = $1;", { BindValue(checkout_session) });
	return static_cast<size_t>(db.changes());
}

std::map<int64_t, std::vector<CheckoutReservation>> live_reservations_by_account(db::Database &db) {
	std::map<int64_t, std::vector<CheckoutReservation>> out;
	for (CheckoutReservation &r : query_reservations(db, "ORDER BY id;", {})) {
		const int64_t user = r.user_id;
		out[user].push_back(std::move(r));
	}
	return out;
}

std::vector<CheckoutReservation> live_reservations_of(db::Database &db, int64_t user_id) {
	return query_reservations(db, "AND user_id = $1 ORDER BY id;", { BindValue(user_id) });
}

SubscriptionRow reservation_row(const CheckoutReservation &reservation, int64_t now) {
	SubscriptionRow row;
	row.stripe_subscription_id = "reservation-" + std::to_string(reservation.id);
	row.user_id = reservation.user_id;
	row.status = "active";
	row.item = reservation.item;
	row.tier = reservation.item == ItemKind::Tier ? reservation.tier : 0;
	row.quantity = 1;
	row.current_period_end = now + 24 * 60 * 60;
	return row;
}

} // namespace opennova::novaworld_server::billing
