#include "rate_limiter.h"

#include <base/io/strutil.h>

#include <algorithm>
#include <cmath>

namespace opennova::novaworld_server {

double RateLimiter::refilled(const Bucket &bucket, Clock::time_point now) const {
	const double elapsed = std::chrono::duration<double>(now - bucket.updated).count();
	if (elapsed <= 0) return bucket.tokens;
	return std::min(params_.capacity, bucket.tokens + elapsed * params_.refill_per_second);
}

void RateLimiter::drop_full_locked(Clock::time_point now) {
	for (auto it = buckets_.begin(); it != buckets_.end();) {
		if (refilled(it->second, now) >= params_.capacity) {
			it = buckets_.erase(it);
		} else {
			++it;
		}
	}
	last_sweep_ = now;
}

RateLimiter::Bucket &RateLimiter::bucket_locked(const std::string &key, Clock::time_point now) {
	if (now - last_sweep_ >= kSweepInterval) drop_full_locked(now);

	auto it = buckets_.find(key);
	if (it == buckets_.end()) {
		if (buckets_.size() >= params_.max_keys) drop_full_locked(now);
		if (!buckets_.empty() && buckets_.size() >= params_.max_keys) {
			const auto oldest = std::min_element(
					buckets_.begin(), buckets_.end(), [](const auto &a, const auto &b) {
						return a.second.updated < b.second.updated;
					});
			buckets_.erase(oldest);
		}
		it = buckets_.emplace(key, Bucket{params_.capacity, now}).first;
	} else {
		it->second.tokens = refilled(it->second, now);
		it->second.updated = now;
	}
	return it->second;
}

int64_t RateLimiter::retry_after(const Bucket &bucket) const {
	const double wait = (1 - bucket.tokens) / params_.refill_per_second;
	return std::max<int64_t>(1, static_cast<int64_t>(std::ceil(wait)));
}

int64_t RateLimiter::take(const std::string &key, Clock::time_point now) {
	std::lock_guard<std::mutex> lock(mu_);
	Bucket &bucket = bucket_locked(key, now);
	if (bucket.tokens >= 1) {
		bucket.tokens -= 1;
		return 0;
	}
	return retry_after(bucket);
}

int64_t RateLimiter::wait(const std::string &key, Clock::time_point now) {
	std::lock_guard<std::mutex> lock(mu_);
	// An absent key is a full bucket: reading it adds no entry.
	const auto it = buckets_.find(key);
	if (it == buckets_.end()) return 0;
	Bucket probe = it->second;
	probe.tokens = refilled(probe, now);
	return probe.tokens >= 1 ? 0 : retry_after(probe);
}

void RateLimiter::charge(const std::string &key, Clock::time_point now) {
	std::lock_guard<std::mutex> lock(mu_);
	Bucket &bucket = bucket_locked(key, now);
	bucket.tokens = std::max(0.0, bucket.tokens - 1);
}

size_t RateLimiter::size() const {
	std::lock_guard<std::mutex> lock(mu_);
	return buckets_.size();
}

std::string resolve_client_ip(std::string_view peer, std::string_view x_real_ip,
                              std::string_view x_forwarded_for,
                              const std::vector<std::string> &trusted_proxies) {
	const bool trusted = std::find(trusted_proxies.begin(), trusted_proxies.end(), peer) !=
	                     trusted_proxies.end();
	if (!trusted) return std::string(peer);
	if (const auto real = strutil::trim_view(x_real_ip); !real.empty()) {
		return std::string(real);
	}
	const auto comma = x_forwarded_for.rfind(',');
	const auto last = strutil::trim_view(
			comma == std::string_view::npos ? x_forwarded_for : x_forwarded_for.substr(comma + 1));
	if (!last.empty()) return std::string(last);
	return std::string(peer);
}

} // namespace opennova::novaworld_server
