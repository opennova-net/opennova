#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace opennova::novaworld_server {

// In-memory token buckets keyed by a string (a client address, a username):
// the brake on POST /api/login and POST /api/register. Each key holds up to
// `capacity` tokens and regains `refill_per_second`; a request takes one.
// Crow-free and clocked by its caller, so a unit test steps time by hand.
//
// Bounded: a bucket that has refilled to capacity says nothing a missing one
// does not, so it is dropped (every kSweepInterval, and whenever the table is
// full); a full table of live buckets then evicts the one touched longest ago.
// Thread-safe (one mutex), for Crow's worker threads.
class RateLimiter {
public:
	using Clock = std::chrono::steady_clock;

	struct Params {
		double capacity = 1;          // the burst
		double refill_per_second = 1; // the sustained rate
		size_t max_keys = 10000;      // the table bound
	};

	static constexpr std::chrono::seconds kSweepInterval{60};

	explicit RateLimiter(Params params) : params_(params) {}

	RateLimiter(const RateLimiter &) = delete;
	RateLimiter &operator=(const RateLimiter &) = delete;

	// Takes one token from `key`'s bucket. Returns 0 when it had one, else the
	// whole seconds until it will (at least 1): the Retry-After a refused
	// request is answered with.
	int64_t take(const std::string &key, Clock::time_point now);

	// Hands back a token take() drew, for a bucket only failed attempts should
	// drain: the caller takes before the attempt (so concurrent attempts can
	// never draw more than the bucket holds) and refunds the ones that
	// succeeded. Never past capacity.
	void refund(const std::string &key, Clock::time_point now);

	size_t size() const;

private:
	struct Bucket {
		double tokens = 0;
		Clock::time_point updated;
	};

	double refilled(const Bucket &bucket, Clock::time_point now) const;
	void drop_full_locked(Clock::time_point now);
	// `key`'s bucket refilled to `now`, made (full) when absent, the table
	// bound kept.
	Bucket &bucket_locked(const std::string &key, Clock::time_point now);
	int64_t retry_after(const Bucket &bucket) const;

	const Params params_;
	mutable std::mutex mu_;
	std::unordered_map<std::string, Bucket> buckets_;
	Clock::time_point last_sweep_{};
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
