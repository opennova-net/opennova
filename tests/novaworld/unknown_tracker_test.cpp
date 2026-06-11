// UnknownTracker unit tests: record/dedup semantics, sample cap, distinct
// keys, DB flush + upsert against the real 0004 migration, and snapshot.

#include <novaworld/db/sqlite.h>
#include <novaworld/unknown_tracker.h>

#include "../common/test_expect.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef OPENNOVA_SOURCE_DIR
#error "OPENNOVA_SOURCE_DIR must be set by CMake"
#endif

using opennova::UnknownSighting;
using opennova::UnknownTracker;
using opennova::db::Database;

namespace {

std::string read_file(const std::filesystem::path &p) {
	std::ifstream in(p, std::ios::binary);
	std::ostringstream os;
	os << in.rdbuf();
	return os.str();
}

// Apply the real 0004 migration onto a fresh in-memory DB so the test
// exercises the same schema the server ships.
void apply_unknown_messages_schema(Database &db) {
	const std::filesystem::path source_dir{OPENNOVA_SOURCE_DIR};
	const auto sql = source_dir / "backend" / "migrations" / "0004_unknown_messages.sql";
	db.exec_script(read_file(sql));
}

const UnknownSighting *find(const std::vector<UnknownSighting> &v,
                           const std::string &channel,
                           const std::string &signature) {
	for (const auto &s : v) {
		if (s.channel == channel && s.signature == signature) return &s;
	}
	return nullptr;
}

int test_record_and_dedup() {
	UnknownTracker t;
	const std::vector<uint8_t> first{0x41, 0x42, 0x43};
	const std::vector<uint8_t> second{0x99, 0x98};
	t.record("nwu", "0x47", first, "peer-a", 1000);
	t.record("nwu", "0x47", second, "peer-b", 2500);

	auto snap = t.snapshot();
	TEST_EXPECT(snap.size() == 1);
	const auto *e = find(snap, "nwu", "0x47");
	TEST_EXPECT(e != nullptr);
	TEST_EXPECT(e->count == 2);
	TEST_EXPECT(e->first_seen_ms == 1000);
	TEST_EXPECT(e->last_seen_ms == 2500);
	// First sample + meta are kept; the second sighting must not overwrite.
	TEST_EXPECT(e->sample == first);
	TEST_EXPECT(e->sample_meta == "peer-a");
	return 0;
}

int test_sample_cap() {
	UnknownTracker t;
	std::vector<uint8_t> big(1000, 0xAB);
	t.record("http", "GET /huge", big, "peer", 1);
	auto snap = t.snapshot();
	TEST_EXPECT(snap.size() == 1);
	TEST_EXPECT(snap[0].sample.size() == 512);
	// Truncated to the first 512 bytes (all 0xAB here).
	for (uint8_t b : snap[0].sample) TEST_EXPECT(b == 0xAB);
	return 0;
}

int test_distinct_keys() {
	UnknownTracker t;
	t.record("nwu", "0x47", nullptr, 0, "", 1);
	t.record("nwu", "0x48", nullptr, 0, "", 1);   // same channel, diff sig
	t.record("gate", "0x47", nullptr, 0, "", 1);  // diff channel, same sig
	t.record("pn", "JointOperations", nullptr, 0, "", 1);

	auto snap = t.snapshot();
	TEST_EXPECT(snap.size() == 4);
	TEST_EXPECT(find(snap, "nwu", "0x47") != nullptr);
	TEST_EXPECT(find(snap, "nwu", "0x48") != nullptr);
	TEST_EXPECT(find(snap, "gate", "0x47") != nullptr);
	TEST_EXPECT(find(snap, "pn", "JointOperations") != nullptr);
	return 0;
}

int test_flush_and_upsert() {
	Database db(":memory:");
	apply_unknown_messages_schema(db);

	UnknownTracker t;
	const std::vector<uint8_t> sample{0x10, 0x20, 0x30};
	t.record("container", "ClientFooRequest", sample, "1.2.3.4:64206", 500);
	t.record("container", "ClientFooRequest", {0xFF}, "later", 900); // count -> 2
	t.flush(db);

	auto rows = db.query(
		"SELECT channel, signature, count, first_seen_ms, last_seen_ms, "
		"       sample, sample_meta "
		"FROM unknown_messages;");
	TEST_EXPECT(rows.size() == 1);
	TEST_EXPECT(rows[0].as_text(0).value() == "container");
	TEST_EXPECT(rows[0].as_text(1).value() == "ClientFooRequest");
	TEST_EXPECT(rows[0].as_int(2).value() == 2);
	TEST_EXPECT(rows[0].as_int(3).value() == 500);
	TEST_EXPECT(rows[0].as_int(4).value() == 900);
	TEST_EXPECT(rows[0].as_text(6).value() == "1.2.3.4:64206");

	// A second flush after more records upserts: count grows, first sample +
	// first_seen unchanged.
	t.record("container", "ClientFooRequest", {0x00}, "even-later", 1500); // count -> 3
	t.flush(db);

	rows = db.query(
		"SELECT count, first_seen_ms, last_seen_ms, sample_meta "
		"FROM unknown_messages WHERE channel='container' AND signature='ClientFooRequest';");
	TEST_EXPECT(rows.size() == 1);
	TEST_EXPECT(rows[0].as_int(0).value() == 3);
	TEST_EXPECT(rows[0].as_int(1).value() == 500);   // first_seen unchanged
	TEST_EXPECT(rows[0].as_int(2).value() == 1500);  // last_seen advanced
	TEST_EXPECT(rows[0].as_text(3).value() == "1.2.3.4:64206"); // meta unchanged

	// An empty dirty set must be a no-op (no throw, row count stable).
	t.flush(db);
	auto all = db.query("SELECT COUNT(*) FROM unknown_messages;");
	TEST_EXPECT(all[0].as_int(0).value() == 1);
	return 0;
}

int test_flush_multiple_rows() {
	Database db(":memory:");
	apply_unknown_messages_schema(db);

	UnknownTracker t;
	t.record("nwu", "0x47", nullptr, 0, "a", 1);
	t.record("gate", "badtag", nullptr, 0, "b", 2);
	t.record("http", "GET /nope", nullptr, 0, "c", 3);
	t.flush(db);

	auto rows = db.query("SELECT COUNT(*) FROM unknown_messages;");
	TEST_EXPECT(rows[0].as_int(0).value() == 3);

	// NULL sample column survives a no-sample record.
	auto nullsample = db.query(
		"SELECT sample IS NULL FROM unknown_messages WHERE channel='nwu';");
	TEST_EXPECT(nullsample[0].as_int(0).value() == 1);
	return 0;
}

int test_snapshot_returns_recorded() {
	UnknownTracker t;
	t.record("ptype", "0x123", {0x01}, "meta-x", 42);
	auto snap = t.snapshot();
	TEST_EXPECT(snap.size() == 1);
	TEST_EXPECT(snap[0].channel == "ptype");
	TEST_EXPECT(snap[0].signature == "0x123");
	TEST_EXPECT(snap[0].count == 1);
	TEST_EXPECT(snap[0].first_seen_ms == 42);
	TEST_EXPECT(snap[0].last_seen_ms == 42);
	TEST_EXPECT(snap[0].sample_meta == "meta-x");
	TEST_EXPECT((snap[0].sample == std::vector<uint8_t>{0x01}));
	return 0;
}

} // namespace

int main() {
	if (test_record_and_dedup() != 0) return 1;
	if (test_sample_cap() != 0) return 1;
	if (test_distinct_keys() != 0) return 1;
	if (test_flush_and_upsert() != 0) return 1;
	if (test_flush_multiple_rows() != 0) return 1;
	if (test_snapshot_returns_recorded() != 0) return 1;
	std::printf("OK: unknown_tracker (record/dedup, sample cap, distinct, flush/upsert, snapshot)\n");
	return 0;
}
