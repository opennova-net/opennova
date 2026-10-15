// The fleet's IPv4 networks (apps/novaworld_server/hosted/net_cidr.h): dotted
// quads and CIDRs read strictly, the peers a list of networks admits (an
// IPv4-mapped IPv6 peer among them, never an IPv6 one), the private ranges the
// agent routes' networks (ONNET_HOSTED_BOOTSTRAP_CIDRS) must lie in, the log
// text, and the refusal log's throttle (one line per peer per minute, at most
// kMaxKeys peers at once). Pure functions.

#include "hosted/net_cidr.h"

#include "common/test_expect.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace hosted = opennova::novaworld_server::hosted;

namespace {

int test_log_throttle() {
	uint64_t now = 1000;
	hosted::LogThrottle throttle([&now] { return now; });
	TEST_EXPECT(throttle.admit("10.44.0.21") && !throttle.admit("10.44.0.21"));
	TEST_EXPECT(throttle.admit("10.44.0.22"));
	now += hosted::LogThrottle::kWindowMs - 1;
	TEST_EXPECT(!throttle.admit("10.44.0.21"));
	now += 1;
	TEST_EXPECT(throttle.admit("10.44.0.21"));
	// At most kMaxKeys peers at once: one more is not logged until one lapses.
	now += hosted::LogThrottle::kWindowMs;
	for (size_t i = 0; i < hosted::LogThrottle::kMaxKeys; ++i)
		TEST_EXPECT(throttle.admit("10.45.0." + std::to_string(i)));
	TEST_EXPECT(!throttle.admit("10.46.0.1"));
	now += hosted::LogThrottle::kWindowMs;
	TEST_EXPECT(throttle.admit("10.46.0.1"));
	return 0;
}

int test_cidrs() {
	using hosted::parse_ipv4;
	using hosted::parse_ipv4_cidr;
	TEST_EXPECT(parse_ipv4("10.44.0.5") == uint32_t{ 0x0A2C0005 });
	TEST_EXPECT(parse_ipv4("0.0.0.0") == uint32_t{ 0 } && parse_ipv4("255.255.255.255") == uint32_t{ 0xFFFFFFFF });
	for (const char *bad : { "", "10.44.0", "10.44.0.5.1", "256.0.0.1", "01.2.3.4", " 1.2.3.4", "1.2.3.4 ", "1..2.3",
				 "a.b.c.d", "1.2.3.-4", "1.2.3.4/32", "1000.2.3.4" })
		TEST_EXPECT(!parse_ipv4(bad));
	const auto vpc = parse_ipv4_cidr("10.44.0.0/16");
	TEST_EXPECT(vpc && vpc->network == 0x0A2C0000 && vpc->mask == 0xFFFF0000);
	TEST_EXPECT(parse_ipv4_cidr("0.0.0.0/0") && parse_ipv4_cidr("0.0.0.0/0")->mask == 0);
	const auto host = parse_ipv4_cidr("10.44.0.5");
	TEST_EXPECT(host && host->mask == 0xFFFFFFFF);
	for (const char *bad : { "10.44.0.1/16", "10.44.0.0/33", "10.44.0.0/", "10.44.0.0/x", "10.44.0.0/016",
				 "10.44.0.0/16/1", "/16" })
		TEST_EXPECT(!parse_ipv4_cidr(bad));
	const std::vector<hosted::Ipv4Cidr> admitted = { *vpc, *parse_ipv4_cidr("192.168.7.0/24") };
	TEST_EXPECT(hosted::ipv4_admitted("10.44.3.4", admitted));
	TEST_EXPECT(hosted::ipv4_admitted("192.168.7.200", admitted));
	TEST_EXPECT(hosted::ipv4_admitted("::ffff:10.44.3.4", admitted));
	TEST_EXPECT(hosted::ipv4_admitted("::FFFF:10.44.3.4", admitted));
	for (const char *outside : { "10.45.0.1", "127.0.0.1", "192.168.8.1", "54.162.79.147", "::1", "", "::ffff:" })
		TEST_EXPECT(!hosted::ipv4_admitted(outside, admitted));
	TEST_EXPECT(!hosted::ipv4_admitted("10.44.3.4", {}));

	// ONNET_HOSTED_BOOTSTRAP_CIDRS: the private networks alone, no prefix under /8.
	const auto is_private = [](const char *text) {
		const auto cidr = parse_ipv4_cidr(text);
		return cidr && hosted::ipv4_cidr_private(*cidr);
	};
	for (const char *inside : { "10.0.0.0/8", "10.44.0.0/16", "10.44.0.5", "172.16.0.0/12", "172.31.32.0/20",
				 "192.168.0.0/16", "192.168.7.0/24" })
		TEST_EXPECT(is_private(inside));
	for (const char *outside : { "0.0.0.0/0", "8.0.0.0/7", "10.0.0.0/7", "172.0.0.0/8", "172.32.0.0/16",
				 "192.168.0.0/15", "54.162.0.0/16", "100.64.0.0/10", "127.0.0.0/8" }) {
		TEST_EXPECT(parse_ipv4_cidr(outside));
		TEST_EXPECT(!is_private(outside));
	}
	TEST_EXPECT(hosted::ipv4_cidr_text(*vpc) == "10.44.0.0/16");
	TEST_EXPECT(hosted::ipv4_cidr_text(*host) == "10.44.0.5/32");
	TEST_EXPECT(hosted::ipv4_cidr_text(*parse_ipv4_cidr("0.0.0.0/0")) == "0.0.0.0/0");
	return 0;
}

} // namespace

int main() {
	struct Case {
		const char *name;
		int (*fn)();
	};
	const Case cases[] = {
		{ "cidrs", test_cidrs },
		{ "log_throttle", test_log_throttle },
	};
	for (const Case &c : cases) {
		std::printf("-- %s\n", c.name);
		if (c.fn() != 0) {
			std::fprintf(stderr, "FAIL: %s\n", c.name);
			return 1;
		}
	}
	std::printf("OK: the fleet's IPv4 networks\n");
	return 0;
}
