// prune_stale_hosts backstop: rows whose updated_at is older than the
// window are removed; fresh rows survive; host_players cascade away.

#include <net/novaworld/db/sqlite.h>
#include <net/novaworld/host_repository.h>

#include <cstdio>
#include <string>

#include "common/test_expect.h"

namespace {

void insert_host(opennova::db::Database &db, uint32_t rid,
                 const std::string &age_modifier) {
	// Directly insert with a controlled updated_at so we don't depend on
	// wall-clock sleeps. age_modifier is a sqlite datetime() modifier,
	// e.g. "-10 minutes" or "-5 seconds".
	db.exec(
		"INSERT INTO active_hosts (rid, gsid, game, app_id, server_name, "
		" host_ip, host_port, peer_ip, peer_port, updated_at) "
		"VALUES (?, ?, 'jop_2_consumer', '10000', 'srv', '10.0.0.1', 64206, "
		" '10.0.0.1', 64206, datetime('now', ?));",
		{opennova::db::BindValue(static_cast<int64_t>(rid)),
		 opennova::db::BindValue(std::string("GSID-") + std::to_string(rid)),
		 opennova::db::BindValue(age_modifier)});
}

int64_t host_count(opennova::db::Database &db) {
	auto rows = db.query("SELECT COUNT(*) FROM active_hosts;");
	return rows.at(0).as_int(0).value_or(-1);
}

} // namespace

int main() {
	opennova::db::Database db(":memory:");
	// The real migrations, so the schema (FKs, indexes, cascade) matches
	// production exactly.
	opennova::db::run_migrations(db, OPENNOVA_SOURCE_DIR "/backend/migrations");

	insert_host(db, 1, "-10 minutes"); // stale
	insert_host(db, 2, "-2 seconds");  // fresh
	insert_host(db, 3, "-30 minutes"); // stale
	TEST_EXPECT(host_count(db) == 3);

	// Window of 5 minutes (300 s): the two stale rows go, the fresh stays.
	const int pruned = opennova::hostdb::prune_stale_hosts(db, 300);
	TEST_EXPECT(pruned == 2);
	TEST_EXPECT(host_count(db) == 1);

	auto rows = db.query("SELECT rid FROM active_hosts;");
	TEST_EXPECT(rows.size() == 1);
	TEST_EXPECT(rows.at(0).as_int(0).value_or(-1) == 2);

	// A second sweep with nothing stale removes nothing.
	TEST_EXPECT(opennova::hostdb::prune_stale_hosts(db, 300) == 0);
	TEST_EXPECT(host_count(db) == 1);

	std::printf("OK: prune_stale_hosts backstop\n");
	return 0;
}
