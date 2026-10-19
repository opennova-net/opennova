// The hosting entitlement (apps/novaworld_common/billing/entitlement.h) and the
// billing tables it reads (billing/billing_repository.h, backend/migrations/
// 0002_billing.sql). The pure core first: which Stripe statuses grant, the
// period's 48 h net, a comp grant against a tier, the own-machine add-on that
// counts only beside a tier, an admin's own machines counting while the
// account has servers, past_due keeping the servers but starting none, an
// inactive or service account getting nothing, and the add-ons that end with
// the tier (add_ons_to_end). Then the same through a
// migrated database of the test's own (common/test_database.h), and the
// tables' own rules: the instance id minted once, the customers, a
// subscription's status_since moving only with its status, the webhook
// events' dedupe, their pending order and cutoff, and the prune.

#include "billing/billing_repository.h"
#include "billing/entitlement.h"
#include "billing/tiers.h"

#include <net/novaworld/db/database.h>

#include "common/test_database.h"
#include "common/test_expect.h"

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace billing = opennova::novaworld_server::billing;
using billing::Entitlement;
using billing::ItemKind;
using billing::SubscriptionRow;
using opennova::db::BindValue;
using opennova::db::Database;
using test_db::TestDatabase;

namespace {

constexpr int64_t kNow = 1760000000;
constexpr int64_t kDay = 24 * 60 * 60;

SubscriptionRow sub(const std::string &id, ItemKind item, int tier, const std::string &status,
		std::optional<int64_t> period_end = kNow + 20 * kDay, int64_t quantity = 1) {
	SubscriptionRow s;
	s.stripe_subscription_id = id;
	s.stripe_customer_id = "cus_test";
	s.status = status;
	s.item = item;
	s.price_id = "price_" + id;
	s.tier = tier;
	s.quantity = quantity;
	s.current_period_end = period_end;
	return s;
}

SubscriptionRow tier_sub(int tier, const std::string &status, std::optional<int64_t> period_end = kNow + 20 * kDay) {
	return sub("t" + std::to_string(tier) + status, ItemKind::Tier, tier, status, period_end);
}

billing::AccountFacts player(int comp = 0, int comp_own = 0) {
	billing::AccountFacts a;
	a.comp_servers = comp;
	a.comp_own_machines = comp_own;
	return a;
}

int test_status_truth_table() {
	struct Row {
		const char *status;
		bool grants;
	};
	const Row rows[] = {
		{ "active", true }, { "trialing", true }, { "past_due", true }, { "incomplete", false },
		{ "incomplete_expired", false }, { "unpaid", false }, { "canceled", false }, { "paused", false },
		{ "", false }, { "something_new", false },
	};
	for (const Row &r : rows) {
		TEST_EXPECT(billing::status_grants(r.status) == r.grants);
		TEST_EXPECT(billing::subscription_grants(tier_sub(1, r.status), kNow) == r.grants);
		TEST_EXPECT(billing::subscription_grants(sub("om", ItemKind::OwnMachine, 0, r.status), kNow) == r.grants);
		TEST_EXPECT(billing::subscription_grants(sub("sa", ItemKind::StaticAddress, 0, r.status), kNow) == r.grants);
	}
	// An unknown price, or a tier row with no tier, grants nothing.
	TEST_EXPECT(!billing::subscription_grants(sub("u", ItemKind::Unknown, 0, "active"), kNow));
	TEST_EXPECT(!billing::subscription_grants(sub("t0", ItemKind::Tier, 0, "active"), kNow));
	TEST_EXPECT(!billing::subscription_grants(sub("t4", ItemKind::Tier, 4, "active"), kNow));
	return 0;
}

// The fail-closed net: a granting status grants until 48 h past the period's
// end, never with no period end.
int test_period_net() {
	const int64_t end = kNow - 10;
	TEST_EXPECT(billing::subscription_grants(tier_sub(1, "active", end), kNow));
	TEST_EXPECT(billing::subscription_grants(tier_sub(1, "active", end), end + billing::kPeriodNetSeconds - 1));
	TEST_EXPECT(!billing::subscription_grants(tier_sub(1, "active", end), end + billing::kPeriodNetSeconds));
	TEST_EXPECT(!billing::subscription_grants(tier_sub(1, "past_due", end), end + billing::kPeriodNetSeconds));
	TEST_EXPECT(!billing::subscription_grants(tier_sub(1, "active", std::nullopt), kNow));
	const Entitlement lapsed =
			billing::entitlement_from(player(), { tier_sub(3, "active", kNow - billing::kPeriodNetSeconds) }, kNow);
	TEST_EXPECT(lapsed.max_servers == 0 && lapsed.tier == 0 && !lapsed.can_start && lapsed.source == "none");
	return 0;
}

int test_entitlement_core() {
	// Nothing: no servers, no start.
	Entitlement e = billing::entitlement_from(player(), {}, kNow);
	TEST_EXPECT(e.max_servers == 0 && !e.can_start && e.source == "none" && e.tier == 0 && e.status.empty());
	// Each tier its servers.
	for (int tier = 1; tier <= billing::kTierCount; ++tier) {
		e = billing::entitlement_from(player(), { tier_sub(tier, "active") }, kNow);
		TEST_EXPECT(e.max_servers == billing::kTierServers[tier] && e.tier == tier && e.can_start);
		TEST_EXPECT(e.source == "tier" && e.status == "active" && e.current_period_end == kNow + 20 * kDay);
	}
	// Trialing grants and starts.
	e = billing::entitlement_from(player(), { tier_sub(2, "trialing") }, kNow);
	TEST_EXPECT(e.max_servers == 2 && e.can_start);
	// A comp grant alone: its servers, and it starts.
	e = billing::entitlement_from(player(3), {}, kNow);
	TEST_EXPECT(e.max_servers == 3 && e.can_start && e.source == "comp" && e.comp_servers == 3 && e.tier == 0);
	// Comp against tier: the larger, the tier named when it gives at least the grant.
	e = billing::entitlement_from(player(2), { tier_sub(1, "active") }, kNow);
	TEST_EXPECT(e.max_servers == 2 && e.source == "comp" && e.tier == 1);
	e = billing::entitlement_from(player(2), { tier_sub(2, "active") }, kNow);
	TEST_EXPECT(e.max_servers == 2 && e.source == "tier");
	e = billing::entitlement_from(player(1), { tier_sub(3, "active") }, kNow);
	TEST_EXPECT(e.max_servers == 3 && e.source == "tier");
	// The highest granting tier counts; a canceled higher one does not.
	e = billing::entitlement_from(player(), { tier_sub(3, "canceled"), tier_sub(1, "active") }, kNow);
	TEST_EXPECT(e.max_servers == 1 && e.tier == 1 && e.status == "active");
	e = billing::entitlement_from(player(), { tier_sub(1, "active"), tier_sub(2, "active") }, kNow);
	TEST_EXPECT(e.max_servers == 2 && e.tier == 2);
	// Shown with none granting: the newest tier subscription's status.
	e = billing::entitlement_from(player(), { tier_sub(2, "canceled"), tier_sub(1, "unpaid") }, kNow);
	TEST_EXPECT(e.max_servers == 0 && e.status == "canceled");
	// past_due keeps the servers and starts none, comp or not.
	e = billing::entitlement_from(player(), { tier_sub(2, "past_due") }, kNow);
	TEST_EXPECT(e.max_servers == 2 && !e.can_start && e.status == "past_due");
	e = billing::entitlement_from(player(3), { tier_sub(1, "past_due") }, kNow);
	TEST_EXPECT(e.max_servers == 3 && !e.can_start);
	// unpaid or canceled: nothing.
	e = billing::entitlement_from(player(), { tier_sub(2, "unpaid") }, kNow);
	TEST_EXPECT(e.max_servers == 0 && !e.can_start);
	// The add-on: its quantity, only while a tier grants.
	const SubscriptionRow two_machines = sub("om2", ItemKind::OwnMachine, 0, "active", kNow + kDay, 2);
	e = billing::entitlement_from(player(), { tier_sub(2, "active"), two_machines }, kNow);
	TEST_EXPECT(e.own_machines == 2 && e.max_servers == 2);
	e = billing::entitlement_from(player(), { two_machines }, kNow);
	TEST_EXPECT(e.own_machines == 0 && e.max_servers == 0);
	e = billing::entitlement_from(player(3), { two_machines }, kNow);
	TEST_EXPECT(e.own_machines == 0 && e.max_servers == 3);
	e = billing::entitlement_from(
			player(), { tier_sub(1, "active"), sub("omc", ItemKind::OwnMachine, 0, "canceled", kNow + kDay, 4) },
			kNow);
	TEST_EXPECT(e.own_machines == 0);
	// An admin's own machines: counted while the account has servers (a tier's
	// or a grant's), on top of the add-on's.
	e = billing::entitlement_from(player(2, 1), {}, kNow);
	TEST_EXPECT(e.own_machines == 1 && e.max_servers == 2 && e.comp_own_machines == 1 && e.can_start);
	e = billing::entitlement_from(player(0, 1), {}, kNow);
	TEST_EXPECT(e.own_machines == 0 && e.max_servers == 0 && e.comp_own_machines == 1);
	e = billing::entitlement_from(player(0, 1), { tier_sub(2, "active"), two_machines }, kNow);
	TEST_EXPECT(e.own_machines == 3 && e.max_servers == 2);
	// The static addresses: each subscription's and each comp one, used by no more
	// than the own machines.
	const SubscriptionRow address = sub("sa1", ItemKind::StaticAddress, 0, "active", kNow + kDay);
	billing::AccountFacts comped = player(0, 1);
	comped.comp_addresses = 1;
	e = billing::entitlement_from(comped, { tier_sub(2, "active"), two_machines, address }, kNow);
	TEST_EXPECT(e.own_machines == 3 && e.address_grants == 2 && e.static_addresses == 2 && e.comp_addresses == 1);
	e = billing::entitlement_from(player(), { tier_sub(1, "active"), address }, kNow);
	TEST_EXPECT(e.own_machines == 0 && e.address_grants == 1 && e.static_addresses == 0);
	e = billing::entitlement_from(player(), { tier_sub(1, "active"), sub("sac", ItemKind::StaticAddress, 0, "canceled") }, kNow);
	TEST_EXPECT(e.address_grants == 0);
	// A past-due add-on keeps its machines and blocks a start.
	e = billing::entitlement_from(
			player(), { tier_sub(1, "active"), sub("omp", ItemKind::OwnMachine, 0, "past_due", kNow + kDay, 1) }, kNow);
	TEST_EXPECT(e.own_machines == 1 && e.max_servers == 1 && !e.can_start);
	// Inactive and service accounts: nothing, whatever they pay or were granted.
	billing::AccountFacts banned = player(5);
	banned.account_status = "banned";
	e = billing::entitlement_from(banned, { tier_sub(3, "active") }, kNow);
	TEST_EXPECT(e.max_servers == 0 && !e.can_start && e.own_machines == 0 && e.source == "none");
	TEST_EXPECT(e.comp_servers == 5 && e.status == "active");
	billing::AccountFacts service = player(5);
	service.kind = "service";
	e = billing::entitlement_from(service, { tier_sub(3, "active") }, kNow);
	TEST_EXPECT(e.max_servers == 0 && !e.can_start);
	return 0;
}

// The ids of add_ons_to_end's answer.
std::vector<std::string> ending(const std::vector<SubscriptionRow> &subscriptions) {
	std::vector<std::string> out;
	for (const SubscriptionRow &s : billing::add_ons_to_end(subscriptions, kNow)) out.push_back(s.stripe_subscription_id);
	return out;
}

// The add-ons that end with the tier: with no tier granting (none, canceled,
// unpaid, past its period's net), each renewing add-on of either kind; with a
// tier that grants (past_due among them), none; an add-on already set to cancel
// at its period's end, or ended, is none of them; a granting subscription of a
// price no list names holds them all.
int test_add_ons_to_end() {
	const SubscriptionRow own = sub("om", ItemKind::OwnMachine, 0, "active");
	const SubscriptionRow address = sub("sa", ItemKind::StaticAddress, 0, "past_due");
	TEST_EXPECT(ending({ own, address }) == (std::vector<std::string>{ "om", "sa" }));
	for (const char *status : { "canceled", "unpaid", "incomplete_expired", "paused" })
		TEST_EXPECT(ending({ tier_sub(2, status), own, address }).size() == 2);
	TEST_EXPECT(ending({ tier_sub(1, "active", kNow - 3 * kDay), own }) == std::vector<std::string>{ "om" });
	for (const char *status : { "active", "trialing", "past_due" })
		TEST_EXPECT(ending({ tier_sub(1, "canceled"), tier_sub(1, status), own, address }).empty());
	SubscriptionRow cancelling = own;
	cancelling.cancel_at_period_end = true;
	TEST_EXPECT(ending({ cancelling, sub("sa2", ItemKind::StaticAddress, 0, "canceled"), address }) ==
			std::vector<std::string>{ "sa" });
	TEST_EXPECT(ending({ sub("x", ItemKind::Unknown, 0, "active"), own }).empty());
	TEST_EXPECT(ending({ sub("x", ItemKind::Unknown, 0, "canceled"), own }) == std::vector<std::string>{ "om" });
	TEST_EXPECT(ending({ tier_sub(3, "canceled") }).empty() && ending({}).empty());
	return 0;
}

int64_t make_player(Database &db, const std::string &name, int comp = 0, const char *kind = "player",
		const char *status = "active") {
	static int pcid = 0;
	char id[16];
	std::snprintf(id, sizeof(id), "0b%06x", ++pcid);
	const auto rows = db.query("INSERT INTO players (username, password_hash, pcid, nwh, nwhandle, kind, "
							   "account_status) VALUES ($1, 'x', $2, '1', $1, $3, $4) RETURNING id;",
			{ BindValue(name), BindValue(std::string(id)), BindValue(std::string(kind)),
					BindValue(std::string(status)) });
	const int64_t user = rows.empty() ? 0 : rows.front().as_int(0).value_or(0);
	// A grant as an admin writes it (a service account takes none: written by hand).
	if (user > 0 && comp > 0)
		db.exec("INSERT INTO comp_grants (user_id, item, quantity) VALUES ($1, 'servers', $2);",
				{ BindValue(user), BindValue(static_cast<int64_t>(comp)) });
	return user;
}

// The entitlement through the database: the account's facts and its rows.
int test_entitlement_of() {
	TestDatabase scratch;
	Database db = scratch.open();
	const int64_t ana = make_player(db, "ent_ana");
	const int64_t ben = make_player(db, "ent_ben", 2);
	const int64_t cy = make_player(db, "ent_cy", 4, "player", "suspended");
	const int64_t srv = make_player(db, "srv-1", 0, "service");
	TEST_EXPECT(ana > 0 && ben > 0 && cy > 0 && srv > 0);
	TEST_EXPECT(billing::entitlement_of(db, ana, kNow).max_servers == 0);
	TEST_EXPECT(billing::entitlement_of(db, 999999, kNow).max_servers == 0);
	Entitlement e = billing::entitlement_of(db, ben, kNow);
	TEST_EXPECT(e.max_servers == 2 && e.source == "comp" && e.can_start);

	SubscriptionRow row = tier_sub(3, "active");
	row.user_id = ana;
	billing::upsert_subscription(db, row);
	e = billing::entitlement_of(db, ana, kNow);
	TEST_EXPECT(e.max_servers == 3 && e.tier == 3 && e.current_period_end == kNow + 20 * kDay);
	TEST_EXPECT(billing::has_granting_subscription(db, ana, kNow));
	TEST_EXPECT(!billing::has_granting_subscription(db, ana, kNow + 30 * kDay));
	row.status = "past_due";
	billing::upsert_subscription(db, row);
	e = billing::entitlement_of(db, ana, kNow);
	TEST_EXPECT(e.max_servers == 3 && !e.can_start);
	TEST_EXPECT(billing::has_granting_subscription(db, ana, kNow));
	row.status = "canceled";
	billing::upsert_subscription(db, row);
	TEST_EXPECT(billing::entitlement_of(db, ana, kNow).max_servers == 0);
	TEST_EXPECT(!billing::has_granting_subscription(db, ana, kNow));
	// A suspended account and a service account get nothing.
	SubscriptionRow cy_row = sub("cy", ItemKind::Tier, 2, "active");
	cy_row.user_id = cy;
	billing::upsert_subscription(db, cy_row);
	TEST_EXPECT(billing::entitlement_of(db, cy, kNow).max_servers == 0);
	TEST_EXPECT(billing::entitlement_of(db, srv, kNow).max_servers == 0);
	// An admin's own machine beside the grant of servers.
	TEST_EXPECT(billing::set_comp_grants(db, ben, { 2, 1 }));
	e = billing::entitlement_of(db, ben, kNow);
	TEST_EXPECT(e.max_servers == 2 && e.own_machines == 1 && e.comp_own_machines == 1);
	TEST_EXPECT(!billing::set_comp_grants(db, srv, { 1, 0 }));
	// The grants' rules are the table's: 1..16 a row, of the two items alone.
	const auto refused = [&db](const std::string &sql) {
		try {
			db.exec(sql);
		} catch (const opennova::db::Error &) {
			return true;
		}
		return false;
	};
	const std::string id = std::to_string(ana);
	TEST_EXPECT(refused("INSERT INTO comp_grants (user_id, item, quantity) VALUES (" + id + ", 'servers', 17);"));
	TEST_EXPECT(refused("INSERT INTO comp_grants (user_id, item, quantity) VALUES (" + id + ", 'servers', 0);"));
	TEST_EXPECT(refused("INSERT INTO comp_grants (user_id, item, quantity) VALUES (" + id + ", 'tier', 1);"));
	return 0;
}

// The billing tables' own rules.
int test_tables() {
	TestDatabase scratch;
	Database db = scratch.open();
	// The instance id: minted once, the same at every call after.
	const std::string id = billing::instance_id(db);
	TEST_EXPECT(id.size() == 32 && id.find_first_not_of("0123456789abcdef") == std::string::npos);
	TEST_EXPECT(billing::instance_id(db) == id);
	TEST_EXPECT(db.query("SELECT 1 FROM service_meta;").size() == 1);

	// The customers: one per account, one account per customer.
	const int64_t ana = make_player(db, "tab_ana");
	const int64_t ben = make_player(db, "tab_ben");
	TEST_EXPECT(billing::insert_customer(db, ana, "cus_ana", false));
	TEST_EXPECT(!billing::insert_customer(db, ana, "cus_other", false));
	TEST_EXPECT(!billing::insert_customer(db, ben, "cus_ana", false));
	TEST_EXPECT(billing::user_of_customer(db, "cus_ana") == ana);
	TEST_EXPECT(!billing::user_of_customer(db, "cus_none"));
	billing::set_open_checkout_session(db, ana, "cs_1");
	auto customer = billing::customer_of(db, ana);
	TEST_EXPECT(customer && customer->open_checkout_session == "cs_1" && customer->synced_at.empty());
	billing::mark_customer_synced(db, "cus_ana");
	TEST_EXPECT(!billing::customer_of(db, ana)->synced_at.empty());

	// A subscription's status_since moves with its status alone.
	SubscriptionRow row = sub("sub_1", ItemKind::Tier, 1, "active");
	row.user_id = ana;
	row.stripe_customer_id = "cus_ana";
	billing::upsert_subscription(db, row);
	db.exec("UPDATE billing_subscriptions SET status_since = now() - interval '1 day';");
	const std::string since = billing::find_subscription(db, "sub_1")->status_since;
	row.quantity = 1;
	row.cancel_at_period_end = true;
	billing::upsert_subscription(db, row);
	auto stored = billing::find_subscription(db, "sub_1");
	TEST_EXPECT(stored && stored->status_since == since && stored->cancel_at_period_end);
	TEST_EXPECT(stored->current_period_end == kNow + 20 * kDay && stored->item == ItemKind::Tier && stored->tier == 1);
	row.status = "past_due";
	billing::upsert_subscription(db, row);
	stored = billing::find_subscription(db, "sub_1");
	TEST_EXPECT(stored && stored->status == "past_due" && stored->status_since != since);
	// The customer's end: its subscriptions still live go canceled, a kept one stays.
	SubscriptionRow other = sub("sub_2", ItemKind::OwnMachine, 0, "active");
	other.user_id = ana;
	other.stripe_customer_id = "cus_ana";
	billing::upsert_subscription(db, other);
	TEST_EXPECT(billing::end_subscriptions_of_customer(db, "cus_ana", { "sub_2" }, "canceled") == 1);
	TEST_EXPECT(billing::find_subscription(db, "sub_1")->status == "canceled");
	TEST_EXPECT(billing::find_subscription(db, "sub_2")->status == "active");
	TEST_EXPECT(billing::subscriptions_of(db, ana).size() == 2);
	// The account's delete keeps the rows, with no account.
	db.exec("DELETE FROM players WHERE id = $1;", { BindValue(ana) });
	TEST_EXPECT(!billing::customer_of(db, ana));
	stored = billing::find_subscription(db, "sub_2");
	TEST_EXPECT(stored && !stored->user_id);

	// The events: once each, pending by the customer that waited longest, the
	// events past the cutoff left pending, an event naming no customer ignored.
	TEST_EXPECT(billing::record_event(db, "evt_1", "invoice.paid", false, "cus_b", kNow));
	TEST_EXPECT(!billing::record_event(db, "evt_1", "invoice.paid", false, "cus_b", kNow));
	TEST_EXPECT(billing::record_event(db, "evt_2", "customer.subscription.updated", false, "cus_a", std::nullopt));
	TEST_EXPECT(billing::record_event(db, "evt_3", "invoice.paid", false, "cus_b", kNow));
	TEST_EXPECT(billing::record_event(db, "evt_4", "price.updated", false, "", kNow));
	db.exec("UPDATE billing_events SET received_at = now() - interval '10 minutes' WHERE event_id = 'evt_1';");
	db.exec("UPDATE billing_events SET received_at = now() - interval '5 minutes' WHERE event_id = 'evt_2';");
	TEST_EXPECT(billing::pending_customers(db, 10) == (std::vector<std::string>{ "cus_b", "cus_a" }));
	TEST_EXPECT(billing::pending_customers(db, 1) == std::vector<std::string>{ "cus_b" });
	TEST_EXPECT(billing::pending_event_count(db) == 3);
	TEST_EXPECT(billing::oldest_pending_age_s(db) >= 599);
	// A cutoff before evt_3 marks evt_1 alone.
	const std::string cutoff =
			db.query("SELECT (clock_timestamp() - interval '60 seconds')::text;").front().as_text(0).value_or("");
	TEST_EXPECT(!billing::database_clock(db).empty());
	TEST_EXPECT(billing::mark_processed(db, "cus_b", "synced", cutoff) == 1);
	TEST_EXPECT(billing::pending_customers(db, 10) == (std::vector<std::string>{ "cus_a", "cus_b" }));
	TEST_EXPECT(billing::mark_processed(db, "cus_b", "synced") == 1);
	TEST_EXPECT(billing::mark_processed(db, "cus_a", "foreign") == 1);
	TEST_EXPECT(billing::pending_customers(db, 10).empty() && billing::oldest_pending_age_s(db) == 0);
	const auto events = billing::recent_events(db, 10);
	TEST_EXPECT(events.size() == 4);
	bool ignored = false;
	for (const auto &e : events) ignored = ignored || (e.event_id == "evt_4" && e.processed && e.outcome == "ignored");
	TEST_EXPECT(ignored);
	// The prune: processed events past 30 days.
	db.exec("UPDATE billing_events SET processed_at = now() - interval '31 days' WHERE event_id IN ('evt_1', 'evt_2');");
	TEST_EXPECT(billing::prune_events(db) == 2);
	TEST_EXPECT(billing::recent_events(db, 10).size() == 2);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	struct Case {
		const char *name;
		int (*fn)();
	};
	const Case pure[] = {
		{ "status_truth_table", test_status_truth_table },
		{ "period_net", test_period_net },
		{ "entitlement_core", test_entitlement_core },
		{ "add_ons_to_end", test_add_ons_to_end },
	};
	for (const Case &c : pure) {
		std::printf("-- %s\n", c.name);
		if (c.fn() != 0) {
			std::fprintf(stderr, "FAIL: %s\n", c.name);
			return 1;
		}
	}
	test_db::init(argc, argv);
	const Case database[] = {
		{ "entitlement_of", test_entitlement_of },
		{ "tables", test_tables },
	};
	for (const Case &c : database) {
		std::printf("-- %s\n", c.name);
		if (c.fn() != 0) {
			std::fprintf(stderr, "FAIL: %s\n", c.name);
			return 1;
		}
	}
	std::printf("OK: the hosting entitlement and the billing tables\n");
	return 0;
}
