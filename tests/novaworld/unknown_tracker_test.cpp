// UnknownTracker unit tests: record/dedup semantics, sample cap, distinct
// keys, DB flush + upsert against the migrated unknown_messages table (two
// processes' trackers adding up in one row, and list()), snapshot, the
// signature's printable bounded form, the per-channel key bound, and the
// age prune.

#include <net/novaworld/db/database.h>
#include <net/novaworld/unknown_tracker.h>

#include "../common/test_database.h"
#include "../common/test_expect.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#ifndef OPENNOVA_SOURCE_DIR
#error "OPENNOVA_SOURCE_DIR must be set by CMake"
#endif

using opennova::UnknownSighting;
using opennova::UnknownTracker;
using opennova::db::Database;

namespace {

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
	// The migrated schema, so the test exercises the table the server ships.
	test_db::TestDatabase scratch;
	Database db = scratch.open();

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
	TEST_EXPECT(std::get<std::vector<uint8_t>>(rows[0].values[5]) == sample);
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
	test_db::TestDatabase scratch;
	Database db = scratch.open();

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

// Two processes' trackers (two edges, an edge and a web replica) flushing one
// signature: each flush adds its sightings since its last to the one row, so
// the count is their sum, never the last writer's; the first sample is the
// first writer's, last_seen the latest of either. list() reads the rows, the
// latest seen first, one channel's when asked.
int test_two_trackers_add_up() {
	test_db::TestDatabase scratch;
	Database db = scratch.open();

	UnknownTracker edge_a;
	UnknownTracker edge_b;
	edge_a.record("gate", "badtag", { 0x0A }, "a", 100);
	edge_a.record("gate", "badtag", { 0x0B }, "a", 200); // a: 2
	edge_b.record("gate", "badtag", { 0x0C }, "b", 150); // b: 1
	edge_a.flush(db);
	edge_b.flush(db);
	auto rows = db.query(
			"SELECT count, first_seen_ms, last_seen_ms, sample_meta FROM unknown_messages;");
	TEST_EXPECT(rows.size() == 1);
	TEST_EXPECT(rows[0].as_int(0).value() == 3);
	TEST_EXPECT(rows[0].as_int(1).value() == 100);
	TEST_EXPECT(rows[0].as_int(2).value() == 200); // b's older sighting moves it not back
	TEST_EXPECT(rows[0].as_text(3).value() == "a");
	// More of each: only what came since each one's last flush is added.
	edge_b.record("gate", "badtag", { 0x0D }, "b", 300);
	edge_b.flush(db);
	edge_a.flush(db); // nothing new: a no-op
	rows = db.query("SELECT count, last_seen_ms FROM unknown_messages;");
	TEST_EXPECT(rows[0].as_int(0).value() == 4 && rows[0].as_int(1).value() == 300);

	edge_a.record("http", "GET /nope", nullptr, 0, "c", 50);
	edge_a.flush(db);
	const auto listed = UnknownTracker::list(db, "");
	TEST_EXPECT(listed.size() == 2 && listed[0].signature == "badtag" && listed[0].count == 4 &&
			(listed[0].sample == std::vector<uint8_t>{ 0x0A }) && listed[1].channel == "http");
	const auto gate_only = UnknownTracker::list(db, "gate");
	TEST_EXPECT(gate_only.size() == 1 && gate_only[0].sample_meta == "a");
	return 0;
}

// A signature is kept as its printable form: printable ASCII as it is, every
// other byte and the backslash as \xNN; past kUnknownSignatureCap bytes it is
// cut, ".." and the whole raw signature's FNV-1a 64 after it.
int test_signature_form() {
	TEST_EXPECT(opennova::unknown_signature("0x47") == "0x47");
	TEST_EXPECT(opennova::unknown_signature("GET /a b") == "GET /a b");
	TEST_EXPECT(opennova::unknown_signature(std::string("a\0\xFF\\", 4)) == "a\\x00\\xFF\\x5C");
	// The escape keeps two raws apart: the text "\x01" is not the byte 0x01.
	TEST_EXPECT(opennova::unknown_signature("\\x01") != opennova::unknown_signature("\x01"));
	const std::string fits(opennova::kUnknownSignatureCap, 'x');
	TEST_EXPECT(opennova::unknown_signature(fits) == fits);
	// Two long signatures one byte apart at their end still key apart, each
	// its prefix and a hash.
	const std::string a = std::string(200, 'y') + "a";
	const std::string b = std::string(200, 'y') + "b";
	const std::string sa = opennova::unknown_signature(a);
	const std::string sb = opennova::unknown_signature(b);
	TEST_EXPECT(sa != sb);
	TEST_EXPECT(sa.size() == opennova::kUnknownSignatureCap + 2 + 16);
	TEST_EXPECT(sa.compare(0, opennova::kUnknownSignatureCap,
						std::string(opennova::kUnknownSignatureCap, 'y')) == 0);
	TEST_EXPECT(sa.compare(opennova::kUnknownSignatureCap, 2, "..") == 0);
	// An escape is never split by the cut.
	const std::string escapes(100, '\x01');
	const std::string se = opennova::unknown_signature(escapes);
	TEST_EXPECT(se.size() == opennova::kUnknownSignatureCap + 2 + 16);
	TEST_EXPECT(se.compare(0, 4, "\\x01") == 0 && se.compare(124, 4, "\\x01") == 0);
	return 0;
}

// What one process keeps is bounded however many distinct signatures arrive:
// each kept to its printable form's cap, a channel to kUnknownKeysPerChannel
// of them, every later new one counted in the channel's overflow sighting; a
// signature already kept goes on counting in its own, and another channel
// keeps its own room.
int test_bounded_keys() {
	UnknownTracker t;
	const std::size_t extra = 500;
	const std::size_t n = opennova::kUnknownKeysPerChannel + extra;
	const std::vector<uint8_t> sample(60000, 0x41);
	for (std::size_t i = 0; i < n; ++i) {
		t.record("gate", std::to_string(i) + std::string(60000, 'x'), sample, "peer", 1000 + i);
	}
	t.record("gate", "0" + std::string(60000, 'x'), nullptr, 0, "peer", 9000);
	t.record("nwu", "0x47", nullptr, 0, "peer", 9001);

	const auto snap = t.snapshot();
	TEST_EXPECT(snap.size() == opennova::kUnknownKeysPerChannel + 2);
	std::size_t key_bytes = 0;
	std::size_t sample_bytes = 0;
	uint64_t gate_count = 0;
	for (const auto &s : snap) {
		TEST_EXPECT(s.signature.size() <= opennova::kUnknownSignatureCap + 2 + 16);
		key_bytes += s.signature.size();
		sample_bytes += s.sample.size();
		if (s.channel == "gate")
			gate_count += s.count;
	}
	TEST_EXPECT(key_bytes <= snap.size() * (opennova::kUnknownSignatureCap + 2 + 16));
	TEST_EXPECT(sample_bytes <= snap.size() * opennova::kUnknownSampleCap);
	TEST_EXPECT(gate_count == n + 1);
	const auto *other = find(snap, "gate", opennova::kUnknownOverflowSignature);
	TEST_EXPECT(other != nullptr);
	TEST_EXPECT(other->count == extra);
	TEST_EXPECT(other->first_seen_ms == 1000 + opennova::kUnknownKeysPerChannel);
	TEST_EXPECT(other->last_seen_ms == 1000 + n - 1);
	const auto *first =
			find(snap, "gate", opennova::unknown_signature("0" + std::string(60000, 'x')));
	TEST_EXPECT(first != nullptr && first->count == 2 && first->last_seen_ms == 9000);
	const auto *nwu = find(snap, "nwu", "0x47");
	TEST_EXPECT(nwu != nullptr && nwu->count == 1);
	return 0;
}

// A signature with bytes a text column refuses (a NUL, a byte past 0x7F)
// flushes as its printable form, so one such sighting never fails a flush.
int test_flush_unprintable_signature() {
	test_db::TestDatabase scratch;
	Database db = scratch.open();

	UnknownTracker t;
	t.record("gate", std::string("ab\0\xFF", 4), nullptr, 0, "peer", 1);
	t.record("http", "GET /ok", nullptr, 0, "peer", 2);
	t.flush(db);
	const auto rows =
			db.query("SELECT signature FROM unknown_messages WHERE channel = 'gate';");
	TEST_EXPECT(rows.size() == 1);
	TEST_EXPECT(rows[0].as_text(0).value() == "ab\\x00\\xFF");
	TEST_EXPECT(db.query("SELECT 1 FROM unknown_messages;").size() == 2);
	return 0;
}

// prune() deletes the rows no process has seen for its keep, by the
// database's clock against the rows' wall-clock last_seen_ms, and keeps the rest.
int test_prune_by_age() {
	test_db::TestDatabase scratch;
	Database db = scratch.open();

	const auto rows = db.query("SELECT (EXTRACT(EPOCH FROM now()) * 1000)::bigint;");
	const uint64_t now = static_cast<uint64_t>(rows[0].as_int(0).value());
	const uint64_t day = 24ull * 60 * 60 * 1000;
	UnknownTracker t;
	t.record("gate", "old", nullptr, 0, "peer", now - 31 * day);
	t.record("gate", "recent", nullptr, 0, "peer", now - 29 * day);
	t.record("http", "GET /now", nullptr, 0, "peer", now);
	t.flush(db);
	TEST_EXPECT(UnknownTracker::prune(db) == 1);
	TEST_EXPECT(db.query("SELECT 1 FROM unknown_messages WHERE signature = 'old';").empty());
	TEST_EXPECT(db.query("SELECT 1 FROM unknown_messages;").size() == 2);
	// A shorter keep takes the next one; nothing is left to take twice.
	TEST_EXPECT(UnknownTracker::prune(db, static_cast<int64_t>(day)) == 1);
	TEST_EXPECT(UnknownTracker::prune(db, static_cast<int64_t>(day)) == 0);
	const auto left = UnknownTracker::list(db, "");
	TEST_EXPECT(left.size() == 1 && left[0].signature == "GET /now");
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

int main(int argc, char **argv) {
	test_db::init(argc, argv);
	if (test_record_and_dedup() != 0) return 1;
	if (test_sample_cap() != 0) return 1;
	if (test_distinct_keys() != 0) return 1;
	if (test_flush_and_upsert() != 0) return 1;
	if (test_flush_multiple_rows() != 0) return 1;
	if (test_two_trackers_add_up() != 0)
		return 1;
	if (test_snapshot_returns_recorded() != 0) return 1;
	if (test_signature_form() != 0) return 1;
	if (test_bounded_keys() != 0) return 1;
	if (test_flush_unprintable_signature() != 0) return 1;
	if (test_prune_by_age() != 0) return 1;
	std::printf("OK: unknown_tracker (record/dedup, sample cap, distinct, flush/upsert, snapshot, "
				"bounded keys, prune)\n");
	return 0;
}
