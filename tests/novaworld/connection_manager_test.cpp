#include <novaworld/connection/manager.h>

#include "../common/test_expect.h"

#include <cstdio>
#include <string>
#include <vector>

using opennova::Connection;
using opennova::ConnectionManager;
using opennova::ConnectionState;
using opennova::DropReason;
using opennova::PeerAddr;

namespace {

Connection handshake(uint32_t id, uint32_t ip, uint16_t port, uint64_t now_ms) {
	Connection c;
	c.id = id;
	c.addr = PeerAddr{ip, port};
	c.state = ConnectionState::Handshaking;
	c.created_ms = now_ms;
	c.last_seen_ms = now_ms;
	c.pn = "NOVAWORLDUDP";
	return c;
}

int test_added_callback_fires_on_handshake() {
	ConnectionManager mgr(/*timeout_ms*/ 5000);
	std::vector<uint32_t> added;
	mgr.on_added([&](const Connection &c) { added.push_back(c.id); });

	mgr.notify_handshake(handshake(0xCAFE, 0x7F000001u, 64206, 1000));
	TEST_EXPECT(added.size() == 1);
	TEST_EXPECT(added[0] == 0xCAFE);
	TEST_EXPECT(mgr.registry().size() == 1);
	return 0;
}

int test_logout_fires_lost_with_logout_reason() {
	ConnectionManager mgr(5000);
	std::vector<std::pair<uint32_t, DropReason>> lost;
	mgr.on_lost([&](const Connection &c, DropReason r) { lost.push_back({c.id, r}); });

	mgr.notify_handshake(handshake(0x1, 0x7F000001u, 1, 1000));
	mgr.notify_logout(0x1);

	TEST_EXPECT(lost.size() == 1);
	TEST_EXPECT(lost[0].first == 0x1);
	TEST_EXPECT(lost[0].second == DropReason::Logout);
	TEST_EXPECT(mgr.registry().size() == 0);
	return 0;
}

int test_tick_drops_expired_with_timeout_reason() {
	ConnectionManager mgr(/*timeout_ms*/ 1000);
	std::vector<std::pair<uint32_t, DropReason>> lost;
	mgr.on_lost([&](const Connection &c, DropReason r) { lost.push_back({c.id, r}); });

	mgr.notify_handshake(handshake(0xAAA, 0x7F000001u, 1, 100));
	mgr.notify_handshake(handshake(0xBBB, 0x7F000001u, 2, 100));

	// At t=200ms nothing should expire (200 < 100+1000).
	std::size_t dropped = mgr.tick(200);
	TEST_EXPECT(dropped == 0);
	TEST_EXPECT(mgr.registry().size() == 2);

	// Touch one peer at t=500 to keep it alive.
	mgr.notify_seen(0xAAA, 500);

	// At t=1500, BBB (last_seen=100, timeout=1000) expires; AAA (last_seen=500) survives.
	dropped = mgr.tick(1500);
	TEST_EXPECT(dropped == 1);
	TEST_EXPECT(lost.size() == 1);
	TEST_EXPECT(lost[0].first == 0xBBB);
	TEST_EXPECT(lost[0].second == DropReason::Timeout);
	TEST_EXPECT(mgr.registry().size() == 1);
	return 0;
}

int test_handshake_replacing_existing_addr_fires_replaced() {
	ConnectionManager mgr(5000);
	std::vector<std::pair<uint32_t, DropReason>> lost;
	mgr.on_lost([&](const Connection &c, DropReason r) { lost.push_back({c.id, r}); });

	mgr.notify_handshake(handshake(0x1, 0x7F000001u, 9000, 100));
	mgr.notify_handshake(handshake(0x2, 0x7F000001u, 9000, 200));

	TEST_EXPECT(lost.size() == 1);
	TEST_EXPECT(lost[0].first == 0x1);
	TEST_EXPECT(lost[0].second == DropReason::Replaced);
	TEST_EXPECT(mgr.registry().size() == 1);

	auto remaining = mgr.registry().snapshot();
	TEST_EXPECT(remaining[0].id == 0x2);
	return 0;
}

int test_repeated_hello_preserves_synthetic_identity_and_active_state() {
	ConnectionManager mgr(5000);
	std::vector<DropReason> lost;
	int added = 0;
	mgr.on_lost([&](const Connection &, DropReason reason) {
		lost.push_back(reason);
	});
	mgr.on_added([&](const Connection &) { ++added; });

	const PeerAddr first_addr{0x7F000001u, 9001};
	const PeerAddr second_addr{0x7F000001u, 9002};
	mgr.notify_handshake(handshake(1, first_addr.ip, first_addr.port, 100));
	mgr.notify_handshake(handshake(1, second_addr.ip, second_addr.port, 100));
	mgr.notify_active_addr(
			second_addr, "second", "client-scrk", "server-scrk");
	const auto before = mgr.registry().find_by_addr(second_addr);
	TEST_EXPECT(before.has_value());
	TEST_EXPECT(before->id != 1);
	TEST_EXPECT(before->reported_id == 1);
	TEST_EXPECT(before->state == ConnectionState::Active);

	mgr.notify_handshake(handshake(1, second_addr.ip, second_addr.port, 200));
	const auto after = mgr.registry().find_by_addr(second_addr);
	TEST_EXPECT(after.has_value());
	TEST_EXPECT(after->id == before->id);
	TEST_EXPECT(after->reported_id == before->reported_id);
	TEST_EXPECT(after->state == ConnectionState::Active);
	TEST_EXPECT(after->client_scrk == "client-scrk");
	TEST_EXPECT(after->server_scrk == "server-scrk");
	TEST_EXPECT(lost.empty());
	TEST_EXPECT(added == 2);
	return 0;
}

int test_shutdown_evicts_all_with_shutdown_reason() {
	ConnectionManager mgr(5000);
	std::vector<DropReason> reasons;
	mgr.on_lost([&](const Connection &, DropReason r) { reasons.push_back(r); });

	mgr.notify_handshake(handshake(0x1, 0x7F000001u, 1, 100));
	mgr.notify_handshake(handshake(0x2, 0x7F000001u, 2, 100));
	mgr.notify_handshake(handshake(0x3, 0x7F000001u, 3, 100));

	mgr.shutdown();

	TEST_EXPECT(reasons.size() == 3);
	for (auto r : reasons) {
		TEST_EXPECT(r == DropReason::Shutdown);
	}
	TEST_EXPECT(mgr.registry().size() == 0);
	return 0;
}

int test_notify_active_promotes_state() {
	ConnectionManager mgr;
	mgr.notify_handshake(handshake(0x77, 0x7F000001u, 1, 100));
	mgr.notify_active(0x77, "Taylor", "client_scrk_xxx", "server_scrk_yyy");

	auto c = mgr.registry().find(0x77);
	TEST_EXPECT(c.has_value());
	TEST_EXPECT(c->state == ConnectionState::Active);
	TEST_EXPECT(c->identity == "Taylor");
	return 0;
}

int test_drop_reason_name_is_stable() {
	TEST_EXPECT(std::string(opennova::drop_reason_name(DropReason::Logout)) == "logout");
	TEST_EXPECT(std::string(opennova::drop_reason_name(DropReason::Timeout)) == "timeout");
	TEST_EXPECT(std::string(opennova::drop_reason_name(DropReason::Replaced)) == "replaced");
	TEST_EXPECT(std::string(opennova::drop_reason_name(DropReason::Shutdown)) == "shutdown");
	return 0;
}

} // namespace

int main() {
	if (test_added_callback_fires_on_handshake() != 0) return 1;
	if (test_logout_fires_lost_with_logout_reason() != 0) return 1;
	if (test_tick_drops_expired_with_timeout_reason() != 0) return 1;
	if (test_handshake_replacing_existing_addr_fires_replaced() != 0) return 1;
	if (test_repeated_hello_preserves_synthetic_identity_and_active_state() != 0) return 1;
	if (test_shutdown_evicts_all_with_shutdown_reason() != 0) return 1;
	if (test_notify_active_promotes_state() != 0) return 1;
	if (test_drop_reason_name_is_stable() != 0) return 1;
	std::printf("OK: ConnectionManager handshake/logout/tick/replaced/shutdown\n");
	return 0;
}
