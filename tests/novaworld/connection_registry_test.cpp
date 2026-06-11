#include <novaworld/connection/registry.h>

#include "../common/test_expect.h"

#include <cstdio>

using opennova::Connection;
using opennova::ConnectionRegistry;
using opennova::ConnectionState;
using opennova::PeerAddr;

namespace {

Connection make(uint32_t id, uint32_t ip, uint16_t port, uint64_t now_ms = 1000) {
	Connection c;
	c.id = id;
	c.addr = PeerAddr{ip, port};
	c.state = ConnectionState::Handshaking;
	c.created_ms = now_ms;
	c.last_seen_ms = now_ms;
	c.pn = "NOVAWORLDUDP";
	return c;
}

int test_add_find() {
	ConnectionRegistry r;
	r.add(make(0xAABBCCDD, 0x7F000001u, 64206));

	auto found = r.find(0xAABBCCDD);
	TEST_EXPECT(found.has_value());
	TEST_EXPECT(found->addr.port == 64206);
	TEST_EXPECT(found->state == ConnectionState::Handshaking);

	auto by_addr = r.find_by_addr(PeerAddr{0x7F000001u, 64206});
	TEST_EXPECT(by_addr.has_value());
	TEST_EXPECT(by_addr->id == 0xAABBCCDD);

	TEST_EXPECT(r.size() == 1);
	return 0;
}

int test_touch_updates_last_seen() {
	ConnectionRegistry r;
	r.add(make(0x1, 0x7F000001u, 7000, 1000));

	r.touch(0x1, 5000);
	auto found = r.find(0x1);
	TEST_EXPECT(found.has_value());
	TEST_EXPECT(found->last_seen_ms == 5000);

	r.touch_addr(PeerAddr{0x7F000001u, 7000}, 8000);
	found = r.find(0x1);
	TEST_EXPECT(found->last_seen_ms == 8000);

	r.touch(0xDEAD, 9000); // missing id is a no-op
	TEST_EXPECT(r.size() == 1);
	return 0;
}

int test_drop_removes_both_indices() {
	ConnectionRegistry r;
	r.add(make(0x42, 0x0A000001u, 5000, 100));

	auto dropped = r.drop(0x42);
	TEST_EXPECT(dropped.has_value());
	TEST_EXPECT(dropped->id == 0x42);

	TEST_EXPECT(!r.find(0x42).has_value());
	TEST_EXPECT(!r.find_by_addr(PeerAddr{0x0A000001u, 5000}).has_value());
	TEST_EXPECT(r.size() == 0);

	auto missing = r.drop(0xFF);
	TEST_EXPECT(!missing.has_value());
	return 0;
}

int test_mark_active_promotes() {
	ConnectionRegistry r;
	r.add(make(0x99, 0x7F000001u, 7000));

	bool first = r.mark_active(0x99, "Taylor", "client_scrk_aaa", "server_scrk_bbb");
	TEST_EXPECT(first);
	auto a = r.find(0x99);
	TEST_EXPECT(a->state == ConnectionState::Active);
	TEST_EXPECT(a->identity == "Taylor");
	TEST_EXPECT(a->client_scrk == "client_scrk_aaa");
	TEST_EXPECT(a->server_scrk == "server_scrk_bbb");

	bool second = r.mark_active(0x99, "Other", "x", "y");
	TEST_EXPECT(!second); // already Active
	auto a2 = r.find(0x99);
	TEST_EXPECT(a2->identity == "Taylor"); // unchanged

	bool missing = r.mark_active(0xDEAD, "x", "y", "z");
	TEST_EXPECT(!missing);
	return 0;
}

int test_addr_collision_evicts_old_id() {
	// New HELLO with the same remote addr but a fresh session id should
	// transparently take over the addr binding so future packets for the
	// new session don't re-resolve to the stale entry.
	ConnectionRegistry r;
	r.add(make(0x1, 0x7F000001u, 9000, 100));
	r.add(make(0x2, 0x7F000001u, 9000, 200));

	auto by_addr = r.find_by_addr(PeerAddr{0x7F000001u, 9000});
	TEST_EXPECT(by_addr.has_value());
	TEST_EXPECT(by_addr->id == 0x2);

	auto old = r.find(0x1);
	TEST_EXPECT(!old.has_value()); // evicted with the addr binding
	TEST_EXPECT(r.size() == 1);
	return 0;
}

int test_iter_expired() {
	ConnectionRegistry r;
	r.add(make(0xA, 0x7F000001u, 1, 1000));
	r.add(make(0xB, 0x7F000001u, 2, 2000));
	r.add(make(0xC, 0x7F000001u, 3, 5000));

	// At now=10000, timeout=4999 → all three deadlines are strictly less.
	auto expired = r.iter_expired(10000, 4999);
	TEST_EXPECT(expired.size() == 3);

	// timeout=7999 → only the two oldest (deadlines 8999 and 9999 < 10000).
	expired = r.iter_expired(10000, 7999);
	TEST_EXPECT(expired.size() == 2);

	// timeout=8999 → only A (deadline 9999 < 10000); B and C survive.
	expired = r.iter_expired(10000, 8999);
	TEST_EXPECT(expired.size() == 1);
	TEST_EXPECT(expired[0] == 0xA);

	// timeout=9000 → A's deadline is exactly 10000, NOT strictly less; nothing expires.
	expired = r.iter_expired(10000, 9000);
	TEST_EXPECT(expired.empty());
	return 0;
}

int test_snapshot() {
	ConnectionRegistry r;
	r.add(make(0x1, 0x7F000001u, 1));
	r.add(make(0x2, 0x7F000001u, 2));
	auto snap = r.snapshot();
	TEST_EXPECT(snap.size() == 2);
	return 0;
}

} // namespace

int main() {
	if (test_add_find() != 0) return 1;
	if (test_touch_updates_last_seen() != 0) return 1;
	if (test_drop_removes_both_indices() != 0) return 1;
	if (test_mark_active_promotes() != 0) return 1;
	if (test_addr_collision_evicts_old_id() != 0) return 1;
	if (test_iter_expired() != 0) return 1;
	if (test_snapshot() != 0) return 1;
	std::printf("OK: ConnectionRegistry add/touch/drop/mark_active/iter_expired\n");
	return 0;
}
