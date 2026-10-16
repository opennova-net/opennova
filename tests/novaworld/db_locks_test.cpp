// The service's invariants under concurrent writers, now that a db::Transaction
// takes no database-wide lock (database.h): each check and the write it decides
// holds on the narrow lock its invariant takes, a row's or an advisory one, or
// on a unique index's 23505 mapped to the caller's answer. Each case holds one
// writer's transaction open on one connection (the writer's own function run
// inside a Transaction of the test's, so it nests as a savepoint and its locks
// and rows stay uncommitted), starts the racing writer on a second connection,
// waits until the server reports it waiting on a lock (pg_locks), then commits
// the first and checks what the second answered and what the rows hold:
//   * hosted::request_server: two starts of one account, and two accounts'
//     starts against the global cap, never both pass a count;
//   * a website session and a hosting key minted at a credentials generation a
//     password reset is moving (either one first) never outlive the reset;
//   * hostdb::apply_status_blob folds a status blob into the row as the lobby
//     last wrote it, never into the one it read before that write;
//   * two registrations whose usernames and handles cross never both pass, and
//     a PCID taken by a racing registration answers pcid_exists;
//   * two first-admin promotions promote one account;
//   * the CD keys: a key two admins assign goes to one account, an account two
//     admins give a key holds one, two activations of one account mint one key,
//     one machine activated twice at once has one NWACT, a key revoked twice
//     reports the second AlreadyRevoked;
// and the gate's and the lobby's writes (the gate's status blob, the lobby's
// host row) never wait on an unrelated web transaction's.
// Compiles the service's auth, web_session, cd_keys and hosted sources directly,
// as hosted_supervisor_test does, so it runs on every service build.

#include "auth.h"
#include "cd_keys.h"
#include "hosted_fleet_rig.h"
#include "web_session.h"

#include <net/novaworld/db/database.h>
#include <net/novaworld/host_repository.h>
#include <net/novaworld/lobby_update.h>

#include "common/test_database.h"
#include "common/test_expect.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <thread>

namespace cdkeys = opennova::novaworld_server::cdkeys;
using hosted_rig::kWall;
using hosted_rig::make_player;
using hosted_rig::valid_settings;
using opennova::db::BindValue;
using opennova::db::Database;
using opennova::db::Transaction;

namespace nws = opennova::novaworld_server;
namespace hosted = opennova::novaworld_server::hosted;
namespace hostdb = opennova::hostdb;

namespace {

using Clock = std::chrono::steady_clock;

constexpr const char *kMachineA = "ABCDEFGHIJKLMNOPQRSTUVW";
constexpr const char *kMachineB = "TTTTTTTTTTTTTTTTTTTTTTT";

int64_t backend_pid(Database &db) {
	return db.query("SELECT pg_backend_pid();")[0].as_int(0).value_or(0);
}

// Waits until server process `pid` waits on a lock (a pg_locks row of its not
// granted), `done` is set (it finished without one), or 4 s pass (below the
// 5 s lock timeout): whether it waited.
bool waits_on_lock(Database &observer, int64_t pid, const std::atomic<bool> &done) {
	const auto started = Clock::now();
	while (!done.load() && Clock::now() - started < std::chrono::seconds(4)) {
		if (!observer.query("SELECT 1 FROM pg_locks WHERE pid = $1::integer AND NOT granted;",
							 { BindValue(pid) })
						.empty())
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	return false;
}

// One race on `scratch`: `hold` runs on the holder's connection inside an open
// Transaction, `racer` on the racer's in a thread; once the racer waits on a
// lock (or finished first) the holder commits, and the racer is joined.
// Whether the racer waited; `failure` names an exception either threw.
struct Race {
	bool waited = false;
	std::string failure;
};

Race race(const test_db::TestDatabase &scratch, const std::function<void(Database &)> &hold,
		const std::function<void(Database &)> &racer) {
	Race out;
	Database holder = scratch.open();
	Database other = scratch.open();
	Database observer = scratch.open();
	const int64_t pid = backend_pid(other);
	try {
		Transaction tx(holder);
		hold(holder);
		std::atomic<bool> done{ false };
		std::string racer_failure;
		std::thread thread([&] {
			try {
				racer(other);
			} catch (const std::exception &e) {
				racer_failure = std::string("racer: ") + e.what();
			}
			done.store(true);
		});
		out.waited = waits_on_lock(observer, pid, done);
		try {
			tx.commit();
		} catch (const std::exception &e) {
			out.failure = std::string("holder commit: ") + e.what();
		}
		thread.join();
		if (out.failure.empty())
			out.failure = racer_failure;
	} catch (const std::exception &e) {
		out.failure = std::string("holder: ") + e.what();
	}
	if (!out.failure.empty())
		std::fprintf(stderr, "  %s\n", out.failure.c_str());
	return out;
}

int64_t count_of(Database &db, const std::string &sql, const std::vector<BindValue> &binds = {}) {
	const auto rows = db.query(sql, binds);
	return rows.empty() ? -1 : rows.front().as_int(0).value_or(-1);
}

int64_t generation_of(Database &db, int64_t id) {
	return count_of(db, "SELECT credentials_gen FROM players WHERE id = $1;", { BindValue(id) });
}

nws::UpdateUserParams password_reset() {
	nws::UpdateUserParams p;
	p.password_plaintext = "pw-reset";
	return p;
}

// ---- hosted servers ---------------------------------------------------------

int test_request_server_one_count_at_a_time() {
	test_db::TestDatabase scratch;
	Database db = scratch.open();
	const int64_t olive = make_player(db, "olive", 1);
	const int64_t pia = make_player(db, "pia", 1);
	const int64_t quin = make_player(db, "quin", 1);
	TEST_EXPECT(olive > 0 && pia > 0 && quin > 0);
	const hosted_rig::SeededData data = hosted_rig::seed_data(db);
	const auto start = [&data](Database &conn, int64_t owner, int cap) {
		return hosted::request_server(conn, owner, valid_settings(data.jo),
				hosted::offered_data_sets(conn), cap, kWall)
				.outcome;
	};
	using Outcome = hosted::RequestOutcome;

	// One account entitled to one server: the second start counts the first.
	Outcome first = Outcome::InvalidSettings;
	Outcome second = Outcome::InvalidSettings;
	Race r = race(
			scratch, [&](Database &h) { first = start(h, olive, 10); },
			[&](Database &o) { second = start(o, olive, 10); });
	TEST_EXPECT(r.failure.empty());
	TEST_EXPECT(first == Outcome::Created);
	TEST_EXPECT(second == Outcome::AtLimit);
	TEST_EXPECT(hosted::count_live_of(db, olive) == 1);
	TEST_EXPECT(r.waited);

	// Two accounts against a global cap of two with one server live: one fits.
	r = race(
			scratch, [&](Database &h) { first = start(h, pia, 2); },
			[&](Database &o) { second = start(o, quin, 2); });
	TEST_EXPECT(r.failure.empty());
	TEST_EXPECT(first == Outcome::Created);
	TEST_EXPECT(second == Outcome::AtCapacity);
	TEST_EXPECT(hosted::count_live(db) == 2);
	TEST_EXPECT(r.waited);
	return 0;
}

// ---- credentials minted against a revocation ---------------------------------

int test_mints_never_outlive_a_reset() {
	test_db::TestDatabase scratch;
	Database db = scratch.open();
	const int64_t rex = make_player(db, "rex", 0);
	TEST_EXPECT(rex > 0);
	const std::string key(hostdb::kHostKeyChars, 'K');
	TEST_EXPECT(hostdb::is_host_key(key));
	const auto sessions = [&] {
		return count_of(
				db, "SELECT COUNT(*) FROM web_sessions WHERE user_id = $1;", { BindValue(rex) });
	};
	const auto keys = [&] {
		return count_of(
				db, "SELECT COUNT(*) FROM host_keys WHERE user_id = $1;", { BindValue(rex) });
	};

	// The reset first: the mints wait on the account's row, then find the
	// generation moved and mint nothing.
	int64_t gen = generation_of(db, rex);
	std::optional<std::string> token;
	bool recorded = true;
	bool reset_ok = false;
	Race r = race(
			scratch, [&](Database &h) { reset_ok = nws::update_user(h, rex, password_reset()).ok; },
			[&](Database &o) { token = nws::create_web_session(o, rex, gen, "", ""); });
	TEST_EXPECT(r.failure.empty() && reset_ok);
	TEST_EXPECT(!token.has_value());
	TEST_EXPECT(sessions() == 0);
	TEST_EXPECT(r.waited);

	gen = generation_of(db, rex);
	r = race(
			scratch, [&](Database &h) { reset_ok = nws::update_user(h, rex, password_reset()).ok; },
			[&](Database &o) { recorded = hostdb::record_host_key(o, key, rex, gen); });
	TEST_EXPECT(r.failure.empty() && reset_ok);
	TEST_EXPECT(!recorded);
	TEST_EXPECT(keys() == 0);
	TEST_EXPECT(r.waited);

	// The mints first: the reset waits on the account's row, then deletes what
	// they minted.
	gen = generation_of(db, rex);
	r = race(
			scratch, [&](Database &h) { token = nws::create_web_session(h, rex, gen, "", ""); },
			[&](Database &o) { reset_ok = nws::update_user(o, rex, password_reset()).ok; });
	TEST_EXPECT(r.failure.empty() && reset_ok);
	TEST_EXPECT(token.has_value());
	TEST_EXPECT(sessions() == 0);
	TEST_EXPECT(r.waited);

	gen = generation_of(db, rex);
	r = race(
			scratch, [&](Database &h) { recorded = hostdb::record_host_key(h, key, rex, gen); },
			[&](Database &o) { reset_ok = nws::update_user(o, rex, password_reset()).ok; });
	TEST_EXPECT(r.failure.empty() && reset_ok);
	TEST_EXPECT(recorded);
	TEST_EXPECT(keys() == 0);
	TEST_EXPECT(r.waited);
	return 0;
}

// ---- the status blob's read-modify-write --------------------------------------

hostdb::HostRow listed_host(const std::string &server_name, const std::string &mission) {
	hostdb::HostRow row;
	row.rid = 9100;
	row.game = "jop_2_consumer";
	row.server_name = server_name;
	row.mission_name = mission;
	row.host_ip = "10.0.0.7";
	row.host_port = 32768;
	row.host_key = "HK-9100";
	row.player_count = 1;
	row.max_players = 16;
	return row;
}

int test_status_blob_folds_into_the_newest_row() {
	test_db::TestDatabase scratch;
	Database db = scratch.open();
	hostdb::upsert_host(db, listed_host("S-1", "m1"));

	// The lobby's newer write is open when the blob comes: the fold waits for
	// it, then keeps its columns and adds the blob's.
	opennova::LobbyStatusBlob blob;
	blob.lobby_name = "jop_2_consumer";
	blob.host_key = "HK-9100";
	blob.host_vars = { { "Players", "5" } };
	bool applied = false;
	Race r = race(
			scratch, [&](Database &h) { hostdb::update_host(h, listed_host("S-2", "m2")); },
			[&](Database &o) { applied = hostdb::apply_status_blob(o, blob); });
	TEST_EXPECT(r.failure.empty() && applied);
	TEST_EXPECT(r.waited);
	const auto host = hostdb::find_host_by_rid(db, 9100);
	TEST_EXPECT(host.has_value());
	TEST_EXPECT(host->server_name == "S-2" && host->mission_name == "m2");
	TEST_EXPECT(host->player_count == 5);
	return 0;
}

// The gate's and the lobby's own writes never queue behind a web transaction they share no row
// with: the gate's blob fold and the lobby's host row commit while a
// registration and a start request stand open.
int test_edge_writes_do_not_wait_on_web_writes() {
	test_db::TestDatabase scratch;
	Database db = scratch.open();
	hostdb::upsert_host(db, listed_host("S-1", "m1"));
	const int64_t sam = make_player(db, "sam", 1);
	const hosted_rig::SeededData seeded = hosted_rig::seed_data(db);
	TEST_EXPECT(sam > 0);
	opennova::LobbyStatusBlob blob;
	blob.lobby_name = "jop_2_consumer";
	blob.host_key = "HK-9100";
	blob.host_vars = { { "Players", "3" } };
	bool applied = false;
	bool persisted = false;
	Race r = race(
			scratch,
			[&](Database &h) {
				nws::CreateUserParams p;
				p.username = "tess";
				p.password = "pw-tess";
				p.pcid = "0000e55e";
				p.nwhandle = "tess";
				if (!nws::create_user(h, p).ok)
					throw std::runtime_error("create_user");
				const auto started = hosted::request_server(
						h, sam, valid_settings(seeded.jo), hosted::offered_data_sets(h), 10, kWall);
				if (started.outcome != hosted::RequestOutcome::Created)
					throw std::runtime_error("request_server");
			},
			[&](Database &o) {
				applied = hostdb::apply_status_blob(o, blob);
				Transaction tx(o);
				hostdb::update_host(o, listed_host("S-3", "m3"));
				hostdb::replace_roster(o, 9100, {});
				tx.commit();
				persisted = true;
			});
	TEST_EXPECT(r.failure.empty());
	TEST_EXPECT(!r.waited);
	TEST_EXPECT(applied && persisted);
	return 0;
}

// ---- account names ------------------------------------------------------------

nws::CreateUserParams registration(
		const std::string &username, const std::string &nwhandle, const std::string &pcid) {
	nws::CreateUserParams p;
	p.username = username;
	p.password = "pw-" + username;
	p.pcid = pcid;
	p.nwhandle = nwhandle;
	return p;
}

int test_crossing_names_never_both_pass() {
	test_db::TestDatabase scratch;
	Database db = scratch.open();

	// Each one's username is the other's handle: whichever commits first, the
	// other's check finds its username taken.
	nws::MutationResult first;
	nws::MutationResult second;
	Race r = race(
			scratch,
			[&](Database &h) {
				first = nws::create_user(h, registration("xavier", "pat", "0000a001"));
			},
			[&](Database &o) {
				second = nws::create_user(o, registration("pat", "xavier", "0000a002"));
			});
	TEST_EXPECT(r.failure.empty());
	TEST_EXPECT(first.ok);
	TEST_EXPECT(!second.ok && second.error_code == "username_exists");
	TEST_EXPECT(count_of(db, "SELECT COUNT(*) FROM players;") == 1);
	TEST_EXPECT(r.waited);

	// A rename onto a name a registration is taking waits for it, then finds
	// it taken.
	const int64_t uma = nws::create_user(db, registration("uma", "uma", "0000a003")).id;
	TEST_EXPECT(uma > 0);
	nws::UpdateUserParams rename;
	rename.username = "vera";
	r = race(
			scratch,
			[&](Database &h) {
				first = nws::create_user(h, registration("wes", "vera", "0000a004"));
			},
			[&](Database &o) { second = nws::update_user(o, uma, rename); });
	TEST_EXPECT(r.failure.empty());
	TEST_EXPECT(first.ok);
	TEST_EXPECT(!second.ok && second.error_code == "username_exists");
	TEST_EXPECT(r.waited);

	// A PCID no lock covers: the unique index waits for the first registration
	// and refuses the second's row, which is told pcid_exists, never db_error.
	r = race(
			scratch,
			[&](Database &h) {
				first = nws::create_user(h, registration("yara", "yara", "0000a005"));
			},
			[&](Database &o) {
				second = nws::create_user(o, registration("zeke", "zeke", "0000a005"));
			});
	TEST_EXPECT(r.failure.empty());
	TEST_EXPECT(first.ok);
	TEST_EXPECT(!second.ok && second.error_code == "pcid_exists");
	TEST_EXPECT(r.waited);
	return 0;
}

// ---- the first admin ----------------------------------------------------------

int test_bootstrap_promotes_one_admin() {
	test_db::TestDatabase scratch;
	Database db = scratch.open();
	TEST_EXPECT(make_player(db, "ann", 0) > 0 && make_player(db, "bea", 0) > 0);
	using Outcome = nws::BootstrapAdminResult::Outcome;
	Outcome first = Outcome::NoSuchAccount;
	Outcome second = Outcome::NoSuchAccount;
	Race r = race(
			scratch, [&](Database &h) { first = nws::bootstrap_admin(h, "ann").outcome; },
			[&](Database &o) { second = nws::bootstrap_admin(o, "bea").outcome; });
	TEST_EXPECT(r.failure.empty());
	TEST_EXPECT(first == Outcome::Promoted);
	TEST_EXPECT(second == Outcome::AdminExists);
	TEST_EXPECT(count_of(db, "SELECT COUNT(*) FROM players WHERE role = 'admin';") == 1);
	TEST_EXPECT(r.waited);
	return 0;
}

// ---- CD keys --------------------------------------------------------------------

int test_cd_keys() {
	test_db::TestDatabase scratch;
	Database db = scratch.open();
	const int64_t a1 = make_player(db, "cara", 0);
	const int64_t a2 = make_player(db, "dina", 0);
	const int64_t a3 = make_player(db, "edna", 0);
	TEST_EXPECT(a1 > 0 && a2 > 0 && a3 > 0);
	const auto keys = cdkeys::generate_keys(db, "dfx2", 3, "admin");
	TEST_EXPECT(keys.size() == 3);

	// One key, two admins assigning it to two accounts: the second is told
	// AlreadyAssigned, and the key is the first's.
	cdkeys::AssignOutcome first = cdkeys::AssignOutcome::NoSuchKey;
	cdkeys::AssignOutcome second = cdkeys::AssignOutcome::NoSuchKey;
	Race r = race(
			scratch, [&](Database &h) { first = cdkeys::assign_key(h, keys[0].id, a1); },
			[&](Database &o) { second = cdkeys::assign_key(o, keys[0].id, a2); });
	TEST_EXPECT(r.failure.empty());
	TEST_EXPECT(first == cdkeys::AssignOutcome::Assigned);
	TEST_EXPECT(second == cdkeys::AssignOutcome::AlreadyAssigned);
	TEST_EXPECT(cdkeys::find_key(db, keys[0].id)->account->id == a1);
	TEST_EXPECT(r.waited);

	// One account, two admins giving it two keys of the title: the second is
	// told AccountHasKey (the live-per-account index), never db_error.
	r = race(
			scratch, [&](Database &h) { first = cdkeys::assign_key(h, keys[1].id, a2); },
			[&](Database &o) { second = cdkeys::assign_key(o, keys[2].id, a2); });
	TEST_EXPECT(r.failure.empty());
	TEST_EXPECT(first == cdkeys::AssignOutcome::Assigned);
	TEST_EXPECT(second == cdkeys::AssignOutcome::AccountHasKey);
	TEST_EXPECT(cdkeys::keys_of_account(db, a2).size() == 1);
	TEST_EXPECT(r.waited);

	// One account's first two activations at once, minting on: one key is
	// minted, and the second activation activates its machine under it.
	cdkeys::ActivateResult on_first;
	cdkeys::ActivateResult on_second;
	r = race(
			scratch,
			[&](Database &h) {
				on_first = cdkeys::activate_account(h, a3, "dfx2", kMachineA, true);
			},
			[&](Database &o) {
				on_second = cdkeys::activate_account(o, a3, "dfx2", kMachineB, true);
			});
	TEST_EXPECT(r.failure.empty());
	TEST_EXPECT(on_first.outcome == cdkeys::ActivateOutcome::Activated && on_first.minted &&
			on_first.key);
	TEST_EXPECT(on_second.outcome == cdkeys::ActivateOutcome::Activated && !on_second.minted &&
			on_second.key);
	TEST_EXPECT(on_second.key->id == on_first.key->id);
	TEST_EXPECT(cdkeys::keys_of_account(db, a3).size() == 1);
	TEST_EXPECT(cdkeys::activations_of_key(db, on_first.key->id).size() == 2);
	TEST_EXPECT(r.waited);

	// One machine activated twice at once under one key: one activation, the
	// second answered Reactivated with the first's NWACT.
	r = race(
			scratch,
			[&](Database &h) { on_first = cdkeys::activate_key(h, keys[0].id, kMachineA); },
			[&](Database &o) { on_second = cdkeys::activate_key(o, keys[0].id, kMachineA); });
	TEST_EXPECT(r.failure.empty());
	TEST_EXPECT(on_first.outcome == cdkeys::ActivateOutcome::Activated);
	TEST_EXPECT(on_second.outcome == cdkeys::ActivateOutcome::Reactivated);
	TEST_EXPECT(!on_first.nwact.empty() && on_second.nwact == on_first.nwact);
	TEST_EXPECT(cdkeys::activations_of_key(db, keys[0].id).size() == 1);
	TEST_EXPECT(r.waited);

	// One key revoked twice at once: the second is told AlreadyRevoked.
	cdkeys::RevokeOutcome revoked_first = cdkeys::RevokeOutcome::NoSuchKey;
	cdkeys::RevokeOutcome revoked_second = cdkeys::RevokeOutcome::NoSuchKey;
	r = race(
			scratch, [&](Database &h) { revoked_first = cdkeys::revoke_key(h, keys[0].id); },
			[&](Database &o) { revoked_second = cdkeys::revoke_key(o, keys[0].id); });
	TEST_EXPECT(r.failure.empty());
	TEST_EXPECT(revoked_first == cdkeys::RevokeOutcome::Revoked);
	TEST_EXPECT(revoked_second == cdkeys::RevokeOutcome::AlreadyRevoked);
	TEST_EXPECT(r.waited);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	test_db::init(argc, argv);
	struct Case {
		const char *name;
		int (*fn)();
	};
	const Case cases[] = {
		{ "request_server_one_count_at_a_time", test_request_server_one_count_at_a_time },
		{ "mints_never_outlive_a_reset", test_mints_never_outlive_a_reset },
		{ "status_blob_folds_into_the_newest_row", test_status_blob_folds_into_the_newest_row },
		{ "edge_writes_do_not_wait_on_web_writes", test_edge_writes_do_not_wait_on_web_writes },
		{ "crossing_names_never_both_pass", test_crossing_names_never_both_pass },
		{ "bootstrap_promotes_one_admin", test_bootstrap_promotes_one_admin },
		{ "cd_keys", test_cd_keys },
	};
	for (const Case &c : cases) {
		std::printf("-- %s\n", c.name);
		if (c.fn() != 0) {
			std::fprintf(stderr, "FAIL: %s\n", c.name);
			return 1;
		}
	}
	std::printf("OK: narrow locks hold every invariant with no database-wide lock\n");
	return 0;
}
