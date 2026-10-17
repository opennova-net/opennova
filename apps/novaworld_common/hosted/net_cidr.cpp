#include "net_cidr.h"

#include "health.h" // Watchdog::steady_now_ms

#include <chrono>
#include <iterator>
#include <utility>

namespace opennova::novaworld_server::hosted {

namespace {

} // namespace

std::optional<uint32_t> parse_ipv4(std::string_view text) {
	uint32_t address = 0;
	size_t pos = 0;
	for (int part = 0; part < 4; ++part) {
		if (part != 0) {
			if (pos >= text.size() || text[pos] != '.') return std::nullopt;
			++pos;
		}
		const size_t start = pos;
		uint32_t value = 0;
		while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9' && pos - start < 3) {
			value = value * 10 + static_cast<uint32_t>(text[pos] - '0');
			++pos;
		}
		const size_t digits = pos - start;
		if (digits == 0 || value > 255 || (digits > 1 && text[start] == '0')) return std::nullopt;
		address = (address << 8) | value;
	}
	if (pos != text.size()) return std::nullopt;
	return address;
}

std::optional<Ipv4Cidr> parse_ipv4_cidr(std::string_view text) {
	const size_t slash = text.find('/');
	const auto address = parse_ipv4(text.substr(0, slash));
	if (!address) return std::nullopt;
	int prefix = 32;
	if (slash != std::string_view::npos) {
		const std::string_view bits = text.substr(slash + 1);
		if (bits.empty() || bits.size() > 2) return std::nullopt;
		prefix = 0;
		for (const char c : bits) {
			if (c < '0' || c > '9') return std::nullopt;
			prefix = prefix * 10 + (c - '0');
		}
		if (prefix > 32 || (bits.size() > 1 && bits[0] == '0')) return std::nullopt;
	}
	Ipv4Cidr cidr;
	cidr.mask = prefix == 0 ? 0u : ~uint32_t{ 0 } << (32 - prefix);
	cidr.network = *address;
	if ((cidr.network & ~cidr.mask) != 0) return std::nullopt;
	return cidr;
}

bool ipv4_admitted(std::string_view peer, const std::vector<Ipv4Cidr> &cidrs) {
	static constexpr std::string_view kMapped = "::ffff:";
	if (peer.size() > kMapped.size()) {
		bool mapped = true;
		for (size_t i = 0; i < kMapped.size(); ++i) {
			char c = peer[i];
			if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
			mapped = mapped && c == kMapped[i];
		}
		if (mapped) peer.remove_prefix(kMapped.size());
	}
	const auto address = parse_ipv4(peer);
	if (!address) return false;
	for (const Ipv4Cidr &cidr : cidrs) {
		if ((*address & cidr.mask) == cidr.network) return true;
	}
	return false;
}

bool ipv4_cidr_private(const Ipv4Cidr &cidr) {
	static constexpr Ipv4Cidr kPrivate[] = {
		{ 0x0A000000u, 0xFF000000u }, // 10.0.0.0/8
		{ 0xAC100000u, 0xFFF00000u }, // 172.16.0.0/12
		{ 0xC0A80000u, 0xFFFF0000u }, // 192.168.0.0/16
	};
	for (const Ipv4Cidr &range : kPrivate) {
		// At least the range's prefix, and its network inside the range.
		if ((cidr.mask & range.mask) == range.mask && (cidr.network & range.mask) == range.network) return true;
	}
	return false;
}

std::string ipv4_cidr_text(const Ipv4Cidr &cidr) {
	int prefix = 0;
	for (uint32_t bit = 0x80000000u; bit != 0 && (cidr.mask & bit) != 0; bit >>= 1) ++prefix;
	std::string out;
	for (int shift = 24; shift >= 0; shift -= 8) {
		out += std::to_string((cidr.network >> shift) & 0xFFu);
		out += shift != 0 ? "." : "/";
	}
	return out + std::to_string(prefix);
}

LogThrottle::LogThrottle(Clock now_ms) : now_(now_ms ? std::move(now_ms) : Clock(Watchdog::steady_now_ms)) {}

bool LogThrottle::admit(const std::string &key) {
	std::lock_guard<std::mutex> lock(mu_);
	const uint64_t now = now_();
	for (auto it = last_ms_.begin(); it != last_ms_.end();) {
		it = now - it->second >= kWindowMs ? last_ms_.erase(it) : std::next(it);
	}
	if (last_ms_.count(key) != 0 || last_ms_.size() >= kMaxKeys) return false;
	last_ms_[key] = now;
	return true;
}

} // namespace opennova::novaworld_server::hosted
