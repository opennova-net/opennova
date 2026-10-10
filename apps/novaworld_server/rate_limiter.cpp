#include "rate_limiter.h"

#include <base/io/strutil.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <vector>

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

void RateLimiter::refund(const std::string &key, Clock::time_point now) {
	std::lock_guard<std::mutex> lock(mu_);
	Bucket &bucket = bucket_locked(key, now);
	bucket.tokens = std::min(params_.capacity, bucket.tokens + 1);
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

namespace {

int hex_digit(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

// Colon-separated groups of 1..4 hex digits ("" is no group).
bool parse_hex_groups(std::string_view text, std::vector<uint16_t> &groups) {
	if (text.empty()) return true;
	size_t pos = 0;
	for (;;) {
		const size_t colon = text.find(':', pos);
		const std::string_view group =
				text.substr(pos, colon == std::string_view::npos ? std::string_view::npos : colon - pos);
		if (group.empty() || group.size() > 4) return false;
		unsigned value = 0;
		for (const char c : group) {
			const int d = hex_digit(c);
			if (d < 0) return false;
			value = value * 16 + static_cast<unsigned>(d);
		}
		groups.push_back(static_cast<uint16_t>(value));
		if (colon == std::string_view::npos) return true;
		pos = colon + 1;
	}
}

// Dotted-decimal IPv4, four parts of 1..3 digits, each 0..255.
bool parse_ipv4(std::string_view text, uint8_t out[4]) {
	size_t pos = 0;
	for (int part = 0; part < 4; ++part) {
		const size_t end = part < 3 ? text.find('.', pos) : text.size();
		if (end == std::string_view::npos || end == pos || end - pos > 3) return false;
		unsigned value = 0;
		for (size_t i = pos; i < end; ++i) {
			if (text[i] < '0' || text[i] > '9') return false;
			value = value * 10 + static_cast<unsigned>(text[i] - '0');
		}
		if (value > 255) return false;
		out[part] = static_cast<uint8_t>(value);
		pos = end + 1;
	}
	return true;
}

// An IPv6 address in text (RFC 4291 2.2: "::" for zero groups, a dotted IPv4
// tail; a %zone suffix is dropped) as its 16 bytes.
bool parse_ipv6(std::string_view text, uint8_t out[16]) {
	if (const auto zone = text.find('%'); zone != std::string_view::npos) text = text.substr(0, zone);
	uint8_t v4_tail[4] = {};
	const bool has_v4 = text.find('.') != std::string_view::npos;
	if (has_v4) {
		const auto last_colon = text.rfind(':');
		if (last_colon == std::string_view::npos ||
		    !parse_ipv4(text.substr(last_colon + 1), v4_tail)) {
			return false;
		}
		// Keep a "::" that ends right before the tail; drop a lone separator.
		text = text.substr(0, last_colon + 1);
		if (!(text.size() >= 2 && text.substr(text.size() - 2) == "::")) text.remove_suffix(1);
	}
	std::vector<uint16_t> head;
	std::vector<uint16_t> tail;
	const auto gap = text.find("::");
	if (gap != std::string_view::npos) {
		if (text.find("::", gap + 1) != std::string_view::npos) return false;
		if (!parse_hex_groups(text.substr(0, gap), head) ||
		    !parse_hex_groups(text.substr(gap + 2), tail)) {
			return false;
		}
	} else if (!parse_hex_groups(text, head)) {
		return false;
	}
	const size_t want = has_v4 ? 6 : 8;
	const size_t have = head.size() + tail.size();
	if (gap == std::string_view::npos ? have != want : have >= want) return false;
	std::vector<uint16_t> groups = head;
	groups.resize(want - tail.size(), 0);
	groups.insert(groups.end(), tail.begin(), tail.end());
	for (size_t i = 0; i < want; ++i) {
		out[i * 2] = static_cast<uint8_t>(groups[i] >> 8);
		out[i * 2 + 1] = static_cast<uint8_t>(groups[i] & 0xFF);
	}
	if (has_v4) std::copy(v4_tail, v4_tail + 4, out + 12);
	return true;
}

} // namespace

std::string address_key(std::string_view ip, int v6_prefix_bits) {
	uint8_t bytes[16] = {};
	if (ip.find(':') == std::string_view::npos || !parse_ipv6(ip, bytes)) {
		return std::string(ip.substr(0, 64));
	}
	const bool v4_mapped = std::all_of(bytes, bytes + 10, [](uint8_t b) { return b == 0; }) &&
	                       bytes[10] == 0xFF && bytes[11] == 0xFF;
	if (v4_mapped) {
		return std::to_string(bytes[12]) + "." + std::to_string(bytes[13]) + "." +
		       std::to_string(bytes[14]) + "." + std::to_string(bytes[15]);
	}
	const int bits = std::max(0, std::min(128, v6_prefix_bits));
	const size_t whole = static_cast<size_t>(bits / 8);
	std::string key = strutil::bytes_to_hex(bytes, whole);
	if (bits % 8 != 0) {
		const uint8_t mask = static_cast<uint8_t>(0xFF << (8 - bits % 8));
		const uint8_t partial = static_cast<uint8_t>(bytes[whole] & mask);
		key += strutil::bytes_to_hex(&partial, 1);
	}
	return key + "/" + std::to_string(bits);
}

} // namespace opennova::novaworld_server
