// The sold capacity (apps/novaworld_common/hosted/sold_capacity.h) and the
// checkout reservations it counts (billing/billing_repository.h "A checkout's
// hold"). The pure core first: what an entitlement holds (the larger of a comp
// grant and a tier's, never the sum; an own machine no further than the
// servers), what the fleet sells, what one more subscription adds, the fit and
// the oversold test. Then through a migrated database of the test's own
// (common/test_database.h): every player account's holdings summed with its
// live reservations (a lapsed one, an inactive account and a service account
// counting nothing), a reservation dropped by its session or by its id, and two
// checkouts judged under the hosting-start lock one after the other, the second
// seeing the first's reservation.

#include "billing/billing_repository.h"
#include "billing/entitlement.h"
#include "hosted/placement.h"
#include "hosted/sold_capacity.h"

#include <net/novaworld/db/database.h>

#include "common/test_database.h"
#include "common/test_expect.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace billing = opennova::novaworld_server::billing;
namespace hosted = opennova::novaworld_server::hosted;
using billing::ItemKind;
using billing::SubscriptionRow;
using hosted::Capacity;
using opennova::db::BindValue;
using opennova::db::Database;
using test_db::TestDatabase;

namespace {

constexpr int64_t kNow = 1760000000;

SubscriptionRow granting(const std::string &id, ItemKind item, int tier, int64_t quantity = 1) {
	SubscriptionRow s;
	s.stripe_subscription_id = id;
	s.stripe_customer_id = "cus_" + id;
	s.status = "active";
	s.item = item;
	s.price_id = "price_" + id;
	s.tier = tier;
	s.quantity = quantity;
	s.current_period_end = kNow + 20 * 24 * 60 * 60;
	return s;
}

bool same(const Capacity &c, int64_t shared, int64_t own, int64_t addresses) {
	return c.shared == shared && c.own == own && c.addresses == addresses;
}

int test_core() {
	billing::Entitlement e;
	e.max_servers = 3;
	e.own_machines = 1;
	TEST_EXPECT(same(hosted::held_by(e), 2, 1, 0));
	// An own machine past the servers holds nothing more.
	e.own_machines = 5;
	TEST_EXPECT(same(hosted::held_by(e), 0, 3, 0));
	TEST_EXPECT(same(hosted::held_by(billing::Entitlement{}), 0, 0, 0));

	hosted::PlacementPolicy policy;
	policy.shared_slots = 6;
	policy.max_shared = 2;
	policy.max_own = 3;
	policy.max_addresses = 4;
	const Capacity sellable = hosted::sellable_of(policy);
	TEST_EXPECT(same(sellable, 12, 3, 4));

	Capacity sold;
	sold.shared = 11;
	sold.own = 3;
	TEST_EXPECT(!hosted::sold_exceeds_capacity(sold, sellable));
	Capacity one;
	one.shared = 1;
	TEST_EXPECT(hosted::fits(sold, one, sellable));
	one.shared = 2;
	TEST_EXPECT(!hosted::fits(sold, one, sellable));
	// A part that shrinks is no refusal; one that grows past its sellable is.
	Capacity machine;
	machine.shared = -1;
	machine.own = 1;
	TEST_EXPECT(!hosted::fits(sold, machine, sellable));
	sold.own = 2;
	TEST_EXPECT(hosted::fits(sold, machine, sellable));
	sold.shared = 13;
	TEST_EXPECT(hosted::sold_exceeds_capacity(sold, sellable));
	TEST_EXPECT(hosted::fits(sold, machine, sellable));

	// What one more subscription adds: a tier its servers past the comp grant
	// (the larger, never the sum); an own machine one of its servers moved.
	billing::AccountFacts comp2;
	comp2.comp_servers = 2;
	TEST_EXPECT(same(hosted::added_by(comp2, {}, ItemKind::Tier, 3, kNow), 1, 0, 0));
	TEST_EXPECT(same(hosted::added_by(comp2, {}, ItemKind::Tier, 1, kNow), 0, 0, 0));
	const std::vector<SubscriptionRow> tier2 = { granting("t2", ItemKind::Tier, 2) };
	TEST_EXPECT(same(hosted::added_by(billing::AccountFacts{}, tier2, ItemKind::OwnMachine, 0, kNow), -1, 1, 0));
	for (int tier = 1; tier <= 3; ++tier)
		TEST_EXPECT(same(hosted::added_by_new_account(ItemKind::Tier, tier, kNow), tier, 0, 0));
	TEST_EXPECT(same(hosted::added_by_new_account(ItemKind::OwnMachine, 0, kNow), -1, 1, 0));
	return 0;
}

int64_t make_player(Database &db, const std::string &name, const char *kind = "player", const char *status = "active") {
	static int pcid = 0;
	char id[16];
	std::snprintf(id, sizeof(id), "0c%06x", ++pcid);
	const auto rows = db.query("INSERT INTO players (username, password_hash, pcid, nwh, nwhandle, kind, "
							   "account_status) VALUES ($1, 'x', $2, '1', $1, $3, $4) RETURNING id;",
			{ BindValue(name), BindValue(std::string(id)), BindValue(std::string(kind)),
					BindValue(std::string(status)) });
	return rows.empty() ? 0 : rows.front().as_int(0).value_or(0);
}

void subscribe(Database &db, int64_t user, SubscriptionRow row) {
	row.user_id = user;
	billing::upsert_subscription(db, row);
}

int test_sold_now() {
	TestDatabase scratch;
	Database db = scratch.open();
	const int64_t now = billing::unix_now();
	TEST_EXPECT(same(hosted::sold_now(db, now), 0, 0, 0));
	// Ana: tier 2 and an own machine. Ben: a grant of 3 servers, 1 on an own
	// machine, under a tier 1 (the larger counts). Cy, banned, and a service
	// account: nothing, whatever they hold.
	const int64_t ana = make_player(db, "sold_ana");
	const int64_t ben = make_player(db, "sold_ben");
	const int64_t cy = make_player(db, "sold_cy", "player", "banned");
	const int64_t srv = make_player(db, "srv-77", "service");
	TEST_EXPECT(ana > 0 && ben > 0 && cy > 0 && srv > 0);
	SubscriptionRow tier = granting("ana_t2", ItemKind::Tier, 2);
	tier.current_period_end = now + 20 * 24 * 60 * 60;
	subscribe(db, ana, tier);
	SubscriptionRow own = granting("ana_om", ItemKind::OwnMachine, 0);
	own.current_period_end = tier.current_period_end;
	subscribe(db, ana, own);
	TEST_EXPECT(billing::set_comp_grants(db, ben, { 3, 1 }));
	SubscriptionRow ben_tier = granting("ben_t1", ItemKind::Tier, 1);
	ben_tier.current_period_end = tier.current_period_end;
	subscribe(db, ben, ben_tier);
	TEST_EXPECT(billing::set_comp_grants(db, cy, { 5, 0 }));
	SubscriptionRow srv_tier = granting("srv_t3", ItemKind::Tier, 3);
	srv_tier.current_period_end = tier.current_period_end;
	subscribe(db, srv, srv_tier);
	TEST_EXPECT(same(hosted::sold_now(db, now), 3, 2, 0));

	// A reservation counts as the subscription it sells, until it lapses or
	// goes; another account's id drops nothing.
	const int64_t dee = make_player(db, "sold_dee");
	const int64_t r1 = billing::reserve_checkout(db, dee, ItemKind::Tier, 3, 3600);
	TEST_EXPECT(r1 > 0 && same(hosted::sold_now(db, now), 6, 2, 0));
	const auto held = billing::live_reservations_of(db, dee);
	TEST_EXPECT(held.size() == 1 && held.front().item == ItemKind::Tier && held.front().tier == 3 &&
			held.front().checkout_session.empty());
	billing::stamp_reservation(db, r1, "cs_sold_1");
	TEST_EXPECT(billing::live_reservations_by_account(db).at(dee).front().checkout_session == "cs_sold_1");
	billing::drop_reservation(db, r1, ana);
	TEST_EXPECT(same(hosted::sold_now(db, now), 6, 2, 0));
	TEST_EXPECT(billing::drop_session_reservation(db, "") == 0);
	TEST_EXPECT(billing::drop_session_reservation(db, "cs_sold_1") == 1);
	TEST_EXPECT(same(hosted::sold_now(db, now), 3, 2, 0));
	// An own machine reserved beside Ana's tier moves one of her places.
	const int64_t r2 = billing::reserve_checkout(db, ana, ItemKind::OwnMachine, 0, 3600);
	TEST_EXPECT(r2 > 0 && same(hosted::sold_now(db, now), 2, 3, 0));
	const hosted::AccountHoldings ana_now = hosted::holdings_of(db, ana, now);
	TEST_EXPECT(billing::entitlement_from(ana_now.facts, ana_now.subscriptions, now).own_machines == 2);
	billing::drop_reservation(db, r2, ana);
	// A lapsed one counts nothing, and the next reservation deletes it.
	const int64_t lapsed = billing::reserve_checkout(db, dee, ItemKind::Tier, 1, 3600);
	db.exec("UPDATE billing_checkout_reservations SET expires_at = now() - interval '1 second' WHERE id = $1;",
			{ BindValue(lapsed) });
	TEST_EXPECT(same(hosted::sold_now(db, now), 3, 2, 0) && billing::live_reservations_of(db, dee).empty());
	billing::reserve_checkout(db, dee, ItemKind::Tier, 1, 3600);
	TEST_EXPECT(db.query("SELECT 1 FROM billing_checkout_reservations WHERE id = $1;", { BindValue(lapsed) }).empty());
	// No such account holds nothing.
	const hosted::AccountHoldings none = hosted::holdings_of(db, 999999, now);
	TEST_EXPECT(billing::entitlement_from(none.facts, none.subscriptions, now).max_servers == 0);
	return 0;
}

// Two checkouts for the fleet's last place, each judging and reserving in
// one transaction under the hosting-start lock: the second waits for the
// first's commit, then sees its reservation and finds no room.
int test_last_place_under_the_lock() {
	TestDatabase scratch;
	Database first = scratch.open();
	Database second = scratch.open();
	const int64_t now = billing::unix_now();
	const int64_t ana = make_player(first, "lock_ana");
	const int64_t ben = make_player(first, "lock_ben");
	Capacity sellable;
	sellable.shared = 1;
	const auto judge_and_reserve = [&](Database &db, int64_t user, bool &reserved) {
		const hosted::AccountHoldings h = hosted::holdings_of(db, user, now);
		const Capacity more = hosted::added_by(h.facts, h.subscriptions, ItemKind::Tier, 1, now);
		reserved = hosted::fits(hosted::sold_now(db, now), more, sellable) &&
				billing::reserve_checkout(db, user, ItemKind::Tier, 1, 3600) > 0;
	};

	opennova::db::Transaction a(first);
	first.exec("SELECT pg_advisory_xact_lock($1);", { BindValue(opennova::db::kHostedStartLock) });
	bool ana_reserved = false;
	judge_and_reserve(first, ana, ana_reserved);
	TEST_EXPECT(ana_reserved);
	std::atomic<bool> ben_locked{ false };
	bool ben_reserved = true;
	bool ben_failed = false;
	std::thread ben_checkout([&] {
		try {
			opennova::db::Transaction b(second);
			second.exec("SELECT pg_advisory_xact_lock($1);", { BindValue(opennova::db::kHostedStartLock) });
			ben_locked = true;
			judge_and_reserve(second, ben, ben_reserved);
			b.commit();
		} catch (const opennova::db::Error &e) {
			std::fprintf(stderr, "  ben's checkout: %s\n", e.what());
			ben_failed = true;
		}
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	const bool waited = !ben_locked;
	a.commit();
	ben_checkout.join();
	TEST_EXPECT(waited && !ben_failed && ben_locked && !ben_reserved);
	TEST_EXPECT(same(hosted::sold_now(first, now), 1, 0, 0));
	TEST_EXPECT(billing::live_reservations_of(first, ben).empty());
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	struct Case {
		const char *name;
		int (*fn)();
	};
	std::printf("-- core\n");
	if (test_core() != 0) {
		std::fprintf(stderr, "FAIL: core\n");
		return 1;
	}
	test_db::init(argc, argv);
	const Case database[] = {
		{ "sold_now", test_sold_now },
		{ "last_place_under_the_lock", test_last_place_under_the_lock },
	};
	for (const Case &c : database) {
		std::printf("-- %s\n", c.name);
		if (c.fn() != 0) {
			std::fprintf(stderr, "FAIL: %s\n", c.name);
			return 1;
		}
	}
	std::printf("OK: the sold capacity and the checkout reservations\n");
	return 0;
}
