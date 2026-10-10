// The NovaWorld service's credential-route brake (apps/novaworld_server/
// rate_limiter.*): token buckets on a hand-stepped clock (the burst, the
// refill, the Retry-After, independent keys, the table bound and its
// eviction), and the client address a request is keyed by (resolve_client_ip:
// a proxy header counts only from a trusted peer). Crow-free, so it runs on
// every build; the HTTP route harness drives the same limits over the wire.

#include "rate_limiter.h"

#include "common/test_expect.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace nws = opennova::novaworld_server;
using nws::RateLimiter;
using std::chrono::milliseconds;
using std::chrono::seconds;

namespace {

// A clock origin well past the limiter's zero last-sweep time.
const RateLimiter::Clock::time_point kT0 = RateLimiter::Clock::time_point() + std::chrono::hours(1);

// A burst of `capacity` from a fresh key, then a refusal whose Retry-After is
// the time one token takes; a token returns after that long, and only one.
int test_burst_refill_retry_after() {
	RateLimiter limiter({3, 0.5, 100}); // 3 at once, then one every 2 s
	for (int i = 0; i < 3; ++i) TEST_EXPECT(limiter.take("a", kT0) == 0);
	TEST_EXPECT(limiter.take("a", kT0) == 2);
	// 1.5 s later: three quarters of a token, so still 1 s to go (rounded up).
	TEST_EXPECT(limiter.take("a", kT0 + milliseconds(1500)) == 1);
	TEST_EXPECT(limiter.take("a", kT0 + seconds(2)) == 0);
	TEST_EXPECT(limiter.take("a", kT0 + seconds(2)) > 0);
	// A long rest refills to the burst, never past it.
	const auto later = kT0 + seconds(600);
	for (int i = 0; i < 3; ++i) TEST_EXPECT(limiter.take("a", later) == 0);
	TEST_EXPECT(limiter.take("a", later) > 0);
	return 0;
}

// Keys hold their own buckets: one key's exhaustion leaves another whole.
int test_keys_are_independent() {
	RateLimiter limiter({2, 0.25, 100}); // one every 4 s
	TEST_EXPECT(limiter.take("10.0.0.1", kT0) == 0);
	TEST_EXPECT(limiter.take("10.0.0.1", kT0) == 0);
	TEST_EXPECT(limiter.take("10.0.0.1", kT0) == 4);
	TEST_EXPECT(limiter.take("10.0.0.2", kT0) == 0);
	TEST_EXPECT(limiter.take("10.0.0.2", kT0) == 0);
	TEST_EXPECT(limiter.size() == 2);
	return 0;
}

// The table never holds more than max_keys: a new key first drops the
// buckets that refilled to capacity, then evicts the one touched longest ago.
int test_table_is_bounded() {
	RateLimiter limiter({2, 1.0, 3});
	TEST_EXPECT(limiter.take("a", kT0) == 0);
	TEST_EXPECT(limiter.take("b", kT0 + milliseconds(100)) == 0);
	TEST_EXPECT(limiter.take("c", kT0 + milliseconds(200)) == 0);
	TEST_EXPECT(limiter.size() == 3);
	// All three are still short of capacity at +300 ms: "a", the oldest, goes.
	TEST_EXPECT(limiter.take("d", kT0 + milliseconds(300)) == 0);
	TEST_EXPECT(limiter.size() == 3);
	// "a" comes back as a fresh bucket (the eviction forgot its draw).
	TEST_EXPECT(limiter.take("a", kT0 + milliseconds(400)) == 0);
	TEST_EXPECT(limiter.take("a", kT0 + milliseconds(400)) == 0);
	TEST_EXPECT(limiter.size() == 3);
	for (int i = 0; i < 1000; ++i) {
		limiter.take("k" + std::to_string(i), kT0 + seconds(1) + milliseconds(i));
	}
	TEST_EXPECT(limiter.size() == 3);
	return 0;
}

// Buckets that refilled to capacity say nothing a missing one does not: the
// periodic sweep drops them.
int test_sweep_drops_full_buckets() {
	RateLimiter limiter({5, 1.0, 100});
	for (int i = 0; i < 10; ++i) TEST_EXPECT(limiter.take("ip" + std::to_string(i), kT0) == 0);
	TEST_EXPECT(limiter.size() == 10);
	// Past the sweep interval every bucket is full again; only the new key stays.
	TEST_EXPECT(limiter.take("late", kT0 + RateLimiter::kSweepInterval + seconds(1)) == 0);
	TEST_EXPECT(limiter.size() == 1);
	return 0;
}

// The client address: the peer, unless the peer is a trusted proxy, which
// then names it in X-Real-IP or, failing that, the last X-Forwarded-For hop.
int test_client_ip() {
	const std::vector<std::string> nginx = {"127.0.0.1", "::1"};
	// An untrusted peer's headers are its own claim.
	TEST_EXPECT(nws::resolve_client_ip("203.0.113.9", "10.0.0.1", "10.0.0.2", nginx) ==
	            "203.0.113.9");
	TEST_EXPECT(nws::resolve_client_ip("127.0.0.1", "10.0.0.1", "", {}) == "127.0.0.1");
	// The trusted proxy's X-Real-IP wins, trimmed.
	TEST_EXPECT(nws::resolve_client_ip("127.0.0.1", " 198.51.100.4 ", "1.1.1.1, 2.2.2.2", nginx) ==
	            "198.51.100.4");
	TEST_EXPECT(nws::resolve_client_ip("::1", "2001:db8::7", "", nginx) == "2001:db8::7");
	// Without one, the last X-Forwarded-For hop (the one the proxy appended):
	// a client's own leading entries are not believed.
	TEST_EXPECT(nws::resolve_client_ip("127.0.0.1", "", "6.6.6.6, 198.51.100.4", nginx) ==
	            "198.51.100.4");
	TEST_EXPECT(nws::resolve_client_ip("127.0.0.1", "", "198.51.100.5", nginx) == "198.51.100.5");
	// A trusted peer that names nobody is the client itself.
	TEST_EXPECT(nws::resolve_client_ip("127.0.0.1", "", "", nginx) == "127.0.0.1");
	TEST_EXPECT(nws::resolve_client_ip("127.0.0.1", "  ", " , ", nginx) == "127.0.0.1");
	return 0;
}

} // namespace

int main() {
	struct Case { const char *name; int (*fn)(); };
	const Case cases[] = {
		{"burst_refill_retry_after", test_burst_refill_retry_after},
		{"keys_are_independent", test_keys_are_independent},
		{"table_is_bounded", test_table_is_bounded},
		{"sweep_drops_full_buckets", test_sweep_drops_full_buckets},
		{"client_ip", test_client_ip},
	};
	for (const Case &c : cases) {
		std::printf("-- %s\n", c.name);
		if (c.fn() != 0) {
			std::fprintf(stderr, "FAIL: %s\n", c.name);
			return 1;
		}
	}
	std::printf("OK: rate limiter\n");
	return 0;
}
