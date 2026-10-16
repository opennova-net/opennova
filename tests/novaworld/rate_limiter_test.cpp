// The NovaWorld service's credential-route brake (apps/novaworld_web/
// rate_limiter.*): token buckets in Redis on a hand-stepped clock (the burst,
// the refill, the Retry-After, independent keys and limiters, a bucket's key
// expiring when it would be full again, the take/refund pair the failure-only
// buckets use, and two replicas drawing on one bucket exactly), the address
// keys (IPv6 grouped to a prefix), and the client address a request is keyed
// by (resolve_client_ip: a proxy header counts only from a trusted peer).
// Crow-free; the buckets on the Redis server -DOPENNOVA_TEST_REDIS names
// (common/test_redis.h), each case in a namespace of its own. The HTTP route
// harness drives the same limits over the wire.

#include "rate_limiter.h"
#include "redis_client.h"

#include "common/test_expect.h"
#include "common/test_redis.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace nws = opennova::novaworld_server;
using nws::RateLimiter;
using std::chrono::milliseconds;
using std::chrono::seconds;

namespace {

// A hand-stepped clock's origin. The buckets' TTLs run on the Redis server's
// own clock, so a case keeps each bucket's real time to full above the moments
// it runs for.
const RateLimiter::Clock::time_point kT0 = RateLimiter::Clock::time_point() + std::chrono::hours(1);

// `key`'s bucket's remaining TTL in ms; -2 when it has no key.
long long pttl(nws::RedisPool &redis, const RateLimiter &limiter, const std::string &key) {
	return redis.command({ "PTTL", limiter.bucket_key(key) }).integer;
}

// A burst of `capacity` from a fresh key, then a refusal whose Retry-After is
// the time one token takes; a token returns after that long, and only one.
int test_burst_refill_retry_after() {
	test_redis::TestRedis redis;
	RateLimiter limiter(redis.pool(), { "t", 3, 0.5 }); // 3 at once, then one every 2 s
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
	// A clock behind the bucket's last update (another replica's, a little
	// behind) refills nothing and never moves the bucket back.
	TEST_EXPECT(limiter.take("a", later - seconds(30)) > 0);
	TEST_EXPECT(limiter.take("a", later + seconds(2)) == 0);
	return 0;
}

// Keys hold their own buckets: one key's exhaustion leaves another whole. And
// limiters hold their own: two over the same key (the billing routes' two
// per-account limiters key "billing|<id>" alike) never share a bucket.
int test_keys_and_limiters_are_independent() {
	test_redis::TestRedis redis;
	RateLimiter limiter(redis.pool(), { "t", 2, 0.25 }); // one every 4 s
	TEST_EXPECT(limiter.take("10.0.0.1", kT0) == 0);
	TEST_EXPECT(limiter.take("10.0.0.1", kT0) == 0);
	TEST_EXPECT(limiter.take("10.0.0.1", kT0) == 4);
	TEST_EXPECT(limiter.take("10.0.0.2", kT0) == 0);
	TEST_EXPECT(limiter.take("10.0.0.2", kT0) == 0);
	RateLimiter other(redis.pool(), { "u", 2, 0.25 });
	TEST_EXPECT(other.take("10.0.0.1", kT0) == 0);
	TEST_EXPECT(other.take("10.0.0.1", kT0) == 0);
	TEST_EXPECT(other.take("10.0.0.1", kT0) == 4);
	TEST_EXPECT(limiter.bucket_key("k") != other.bucket_key("k"));
	TEST_EXPECT(limiter.bucket_key("k").rfind(redis.prefix(), 0) == 0);
	return 0;
}

// The bound: a bucket that refilled to capacity says nothing a missing one
// does not, so its key expires when it would be full again (its TTL the time to
// full) and a refund that fills it deletes it; a key past its TTL is gone and
// comes back a full bucket.
int test_bucket_key_expires_when_full() {
	test_redis::TestRedis redis;
	RateLimiter limiter(redis.pool(), { "t", 5, 1.0 }); // 5, then one a second
	TEST_EXPECT(limiter.take("k", kT0) == 0);
	long long ttl = pttl(redis.pool(), limiter, "k");
	TEST_EXPECT(ttl > 0 && ttl <= 1000);
	for (int i = 0; i < 4; ++i)
		TEST_EXPECT(limiter.take("k", kT0) == 0);
	ttl = pttl(redis.pool(), limiter, "k");
	TEST_EXPECT(ttl > 4000 && ttl <= 5000);
	// A refused take leaves it as it was.
	TEST_EXPECT(limiter.take("k", kT0) == 1);
	ttl = pttl(redis.pool(), limiter, "k");
	TEST_EXPECT(ttl > 4000 && ttl <= 5000);
	limiter.refund("k", kT0);
	ttl = pttl(redis.pool(), limiter, "k");
	TEST_EXPECT(ttl > 3000 && ttl <= 4000);
	for (int i = 0; i < 4; ++i)
		limiter.refund("k", kT0);
	TEST_EXPECT(pttl(redis.pool(), limiter, "k") == -2);

	// On the real clock: a fast bucket drained, then left past its time to full.
	RateLimiter fast(redis.pool(), { "fast", 2, 20.0 }); // full again in 100 ms
	const auto now = RateLimiter::Clock::now();
	TEST_EXPECT(fast.take("f", now) == 0);
	TEST_EXPECT(fast.take("f", now) == 0);
	TEST_EXPECT(fast.take("f", now) == 1);
	std::this_thread::sleep_for(milliseconds(250));
	TEST_EXPECT(pttl(redis.pool(), fast, "f") == -2);
	const auto after = RateLimiter::Clock::now();
	TEST_EXPECT(fast.take("f", after) == 0);
	TEST_EXPECT(fast.take("f", after) == 0);
	return 0;
}

// The failure-only brake's pair: take() before the attempt, refund() after a
// success. A run of successes never drains the bucket; failures drain it
// exactly, however many run at once (each took its token first); a refund
// never lifts the bucket past capacity.
int test_take_and_refund() {
	test_redis::TestRedis redis;
	RateLimiter limiter(redis.pool(), { "t", 3, 0.5 }); // 3 failures, then one every 2 s
	for (int i = 0; i < 10; ++i) {
		TEST_EXPECT(limiter.take("user", kT0) == 0);
		limiter.refund("user", kT0); // the password was right
	}
	// Three checks in flight at once, all wrong: the fourth is refused before
	// it runs.
	TEST_EXPECT(limiter.take("user", kT0) == 0);
	TEST_EXPECT(limiter.take("user", kT0) == 0);
	TEST_EXPECT(limiter.take("user", kT0) == 0);
	TEST_EXPECT(limiter.take("user", kT0) == 2);
	// One of them was right after all: its token comes back, and only one.
	limiter.refund("user", kT0);
	TEST_EXPECT(limiter.take("user", kT0) == 0);
	TEST_EXPECT(limiter.take("user", kT0) == 2);
	// Refunds past capacity are dropped.
	for (int i = 0; i < 10; ++i) limiter.refund("other", kT0);
	for (int i = 0; i < 3; ++i) TEST_EXPECT(limiter.take("other", kT0) == 0);
	TEST_EXPECT(limiter.take("other", kT0) == 2);
	return 0;
}

// Two web replicas (two pools over one service's namespace, each with its own
// limiter, as two processes hold them), eight threads on each drawing on one
// bucket at once: exactly `capacity` takes succeed between them, never one
// more, and every refusal names a wait.
int test_replicas_share_buckets_exactly() {
	test_redis::TestRedis replica_a;
	test_redis::TestRedis replica_b(replica_a.prefix(), /*owns=*/false);
	const RateLimiter::Params params{ "login_ip", 50, 1.0 / 3600 };
	RateLimiter limiter_a(replica_a.pool(), params);
	RateLimiter limiter_b(replica_b.pool(), params);
	std::atomic<int> granted{ 0 };
	std::atomic<int> refused_without_wait{ 0 };
	std::atomic<int> failed{ 0 };
	std::vector<std::thread> threads;
	for (int t = 0; t < 16; ++t) {
		RateLimiter &limiter = t % 2 == 0 ? limiter_a : limiter_b;
		threads.emplace_back([&limiter, &granted, &refused_without_wait, &failed] {
			for (int i = 0; i < 20; ++i) {
				try {
					const int64_t wait = limiter.take("198.51.100.7", kT0);
					if (wait == 0)
						++granted;
					else if (wait < 1)
						++refused_without_wait;
				} catch (const std::exception &e) {
					std::fprintf(stderr, "  take: %s\n", e.what());
					++failed;
				}
			}
		});
	}
	for (std::thread &t : threads)
		t.join();
	if (granted != 50)
		std::fprintf(stderr, "  %d granted of 320 against a burst of 50\n", granted.load());
	TEST_EXPECT(failed == 0);
	TEST_EXPECT(granted == 50);
	TEST_EXPECT(refused_without_wait == 0);
	// Either replica sees the bucket the other drew on.
	TEST_EXPECT(limiter_a.take("198.51.100.7", kT0) > 0);
	TEST_EXPECT(limiter_b.take("198.51.100.7", kT0) > 0);
	return 0;
}

// address_key: IPv4 as it is; IPv6 grouped to the prefix, whatever its
// spelling (case, "::", a %zone, a dotted tail); an IPv4-mapped address as
// its IPv4; anything else as it is, cut at 64 bytes.
int test_address_key() {
	TEST_EXPECT(nws::address_key("198.51.100.4", 64) == "198.51.100.4");
	TEST_EXPECT(nws::address_key("::ffff:198.51.100.4", 64) == "198.51.100.4");
	TEST_EXPECT(nws::address_key("::FFFF:c633:6404", 56) == "198.51.100.4");

	const std::string lan = nws::address_key("2001:db8:0:aa::1", 64);
	TEST_EXPECT(lan == "20010db8000000aa/64");
	TEST_EXPECT(nws::address_key("2001:0DB8:0000:00AA:ffff:1:2:3", 64) == lan);
	TEST_EXPECT(nws::address_key("2001:db8:0:aa:1::%eth0", 64) == lan);
	TEST_EXPECT(nws::address_key("2001:db8:0:aa::1.2.3.4", 64) == lan);
	TEST_EXPECT(nws::address_key("2001:db8:0:ab::1", 64) != lan);
	// A /56 holds 256 /64s.
	TEST_EXPECT(nws::address_key("2001:db8:0:aa::1", 56) == "20010db8000000/56");
	TEST_EXPECT(nws::address_key("2001:db8:0:ff::1", 56) == nws::address_key("2001:db8:0:aa::1", 56));
	TEST_EXPECT(nws::address_key("2001:db8:0:1aa::1", 56) != nws::address_key("2001:db8:0:aa::1", 56));
	// A prefix that splits a byte keeps only its bits.
	TEST_EXPECT(nws::address_key("2001:db8:0:aa::1", 60) == "20010db8000000a0/60");
	TEST_EXPECT(nws::address_key("::1", 128) == "00000000000000000000000000000001/128");
	TEST_EXPECT(nws::address_key("::", 64) == "0000000000000000/64");

	// Not addresses: kept as they are (cut at 64 bytes).
	for (const char *text : {"1:2:3:4:5:6:7:8:9", "2001:db8::1::2", "12345::1", "::g", ":1:2:3:4:5:6:7",
	                         "1.2.3.4:80"}) {
		TEST_EXPECT(nws::address_key(text, 64) == text);
	}
	TEST_EXPECT(nws::address_key(std::string(100, 'x'), 64) == std::string(64, 'x'));
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

int main(int argc, char **argv) {
	test_redis::init(argc, argv);
	struct Case { const char *name; int (*fn)(); };
	const Case cases[] = {
		{ "burst_refill_retry_after", test_burst_refill_retry_after },
		{ "keys_and_limiters_are_independent", test_keys_and_limiters_are_independent },
		{ "bucket_key_expires_when_full", test_bucket_key_expires_when_full },
		{ "take_and_refund", test_take_and_refund },
		{ "replicas_share_buckets_exactly", test_replicas_share_buckets_exactly },
		{ "address_key", test_address_key },
		{ "client_ip", test_client_ip },
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
