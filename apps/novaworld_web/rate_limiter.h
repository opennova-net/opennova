#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::novaworld_server {

class RedisPool;

// Token buckets keyed by a string (a client address, a username, an account):
// the brakes on the credential routes and the account's costly actions. Each
// key holds up to `capacity` tokens and regains `refill_per_second`; a request
// takes one. The buckets live in Redis (redis_client.h), one hash per key
// (<prefix>rl:<name>:<key>), and each take or refund is one script that reads,
// refills and writes the bucket atomically, so every web replica draws on the
// same buckets and a limit holds exactly across them. Clocked by its caller, so
// a unit test steps time by hand; the replicas' clocks are the system clock,
// and a bucket written by a replica ahead of this one's clock refills from
// that later time, never backwards.
//
// Bounded: a bucket that has refilled to capacity says nothing a missing one
// does not, so its key expires at the moment it would be full again (and one
// a refund fills is deleted). What bounds the keys a flood of distinct keys
// leaves is the Redis server's memory limit, whose eviction (volatile-lru:
// every key here has a TTL) drops the key touched longest ago.
class RateLimiter {
public:
	using Clock = std::chrono::system_clock;

	struct Params {
		const char *name = ""; // the buckets' namespace, one per limiter
		double capacity = 1; // the burst
		double refill_per_second = 1; // the sustained rate
	};

	RateLimiter(RedisPool &redis, Params params) : redis_(redis), params_(params) {}

	RateLimiter(const RateLimiter &) = delete;
	RateLimiter &operator=(const RateLimiter &) = delete;

	// Takes one token from `key`'s bucket. Returns 0 when it had one, else the
	// whole seconds until it will (at least 1): the Retry-After a refused
	// request is answered with. Throws RedisError when Redis fails.
	int64_t take(const std::string &key, Clock::time_point now);

	// Hands back a token take() drew, for a bucket only failed attempts should
	// drain: the caller takes before the attempt (so concurrent attempts can
	// never draw more than the bucket holds) and refunds the ones that
	// succeeded. Never past capacity.
	void refund(const std::string &key, Clock::time_point now);

	// `key`'s bucket's Redis key.
	std::string bucket_key(const std::string &key) const;

private:
	int64_t run(const std::string &key, Clock::time_point now, bool take);

	RedisPool &redis_;
	const Params params_;
};

// The client address a request is rate-limited and recorded by. The TCP peer,
// unless the peer is one of `trusted_proxies` (ONNET_TRUSTED_PROXIES, exact
// addresses): then the address that proxy reports, its X-Real-IP, or failing
// that the last X-Forwarded-For entry (the hop the proxy itself appended).
// web/nginx.conf sets both from $remote_addr, replacing any X-Real-IP the
// client sent. From any other peer those headers are the client's own claim
// and are ignored.
std::string resolve_client_ip(std::string_view peer, std::string_view x_real_ip,
                              std::string_view x_forwarded_for,
                              const std::vector<std::string> &trusted_proxies);

// The key a client address is braked by: an IPv4 address as it is, and an IPv6
// one grouped to its first `v6_prefix_bits` bits (a /64 is one subscriber's
// LAN, so a /128 key would hand every host 2^64 fresh buckets), spelled as the
// prefix's hex and its length ("20010db8000000aa/64"). An IPv4-mapped IPv6
// address is its IPv4 address. Text that parses as neither is kept as it is,
// cut at 64 bytes.
std::string address_key(std::string_view ip, int v6_prefix_bits);

} // namespace opennova::novaworld_server
