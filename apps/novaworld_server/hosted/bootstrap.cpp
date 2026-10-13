#include "bootstrap.h"

#include "hosted_settings.h"

#include <base/io/base64.h>
#include <base/io/json.h>
#include <base/io/sha256.h>

#include <chrono>
#include <iterator>
#include <utility>

namespace opennova::novaworld_server::hosted {

namespace {

uint64_t steady_now_ms() {
	using namespace std::chrono;
	return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

bool is_token_text(std::string_view token) {
	if (token.size() != BootstrapStore::kTokenChars) return false;
	for (const char c : token) {
		const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
		if (!ok) return false;
	}
	return true;
}

std::string token_key(std::string_view token) { return io::sha256_hex(token.data(), token.size()); }

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

std::string bootstrap_bundle_json(const BootstrapBundle &bundle) {
	io::JsonValue out = io::JsonValue::make_object();
	out.set("server_id", io::json_number(static_cast<double>(bundle.server_id)));
	out.set("image", io::json_string(bundle.image));
	out.set("args", io::json_string_array(bundle.spec.serve_args));
	io::JsonValue files = io::JsonValue::make_array();
	for (const LaunchFile &file : bundle.spec.files) {
		io::JsonValue entry = io::JsonValue::make_object();
		entry.set("name", io::json_string(file.name));
		entry.set("secret", io::JsonValue::make_bool(file.secret));
		entry.set("content_base64",
				io::json_string(io::base64_encode(reinterpret_cast<const uint8_t *>(file.content.data()),
						file.content.size())));
		files.push(std::move(entry));
	}
	out.set("files", std::move(files));
	return io::json_write(out);
}

BootstrapStore::BootstrapStore(Clock now_ms) : now_(now_ms ? std::move(now_ms) : Clock(steady_now_ms)) {}

void BootstrapStore::prune(uint64_t now_ms) {
	for (auto it = entries_.begin(); it != entries_.end();) {
		it = now_ms >= it->second.expires_ms ? entries_.erase(it) : std::next(it);
	}
}

std::string BootstrapStore::issue(BootstrapBundle bundle) {
	const std::string token = random_secret(kTokenChars);
	std::lock_guard<std::mutex> lock(mu_);
	const uint64_t now = now_();
	prune(now);
	Entry entry;
	entry.bundle = std::move(bundle);
	entry.expires_ms = now + kTokenLifetimeMs;
	entries_[token_key(token)] = std::move(entry);
	return token;
}

bool BootstrapStore::bind(const std::string &token, const std::string &machine_id, const std::string &address) {
	std::lock_guard<std::mutex> lock(mu_);
	prune(now_());
	const auto it = entries_.find(token_key(token));
	if (it == entries_.end()) return false;
	it->second.machine_id = machine_id;
	it->second.address = address;
	machines_[machine_id] = false;
	return true;
}

void BootstrapStore::revoke(const std::string &token) {
	std::lock_guard<std::mutex> lock(mu_);
	entries_.erase(token_key(token));
}

std::optional<BootstrapStore::Redeemed> BootstrapStore::redeem(int64_t server_id, std::string_view token,
		const std::string &peer, std::string &why) {
	if (!is_token_text(token)) {
		why = "no token, or not one this service mints";
		return std::nullopt;
	}
	std::lock_guard<std::mutex> lock(mu_);
	prune(now_());
	const auto it = entries_.find(token_key(token));
	if (it == entries_.end()) {
		why = "an unknown, spent or lapsed token";
		return std::nullopt;
	}
	Entry &entry = it->second;
	if (entry.bundle.server_id != server_id) {
		why = "a token issued for another server";
		return std::nullopt;
	}
	if (entry.machine_id.empty() || entry.address.empty()) {
		why = "a token whose launch has not named its machine";
		return std::nullopt;
	}
	const auto bound = parse_ipv4(entry.address);
	if (!bound || !ipv4_admitted(peer, { Ipv4Cidr{ *bound, ~uint32_t{ 0 } } })) {
		why = "a token presented from an address that is not its machine's";
		return std::nullopt;
	}
	Redeemed out;
	out.bundle = std::move(entry.bundle);
	out.machine_id = entry.machine_id;
	machines_[entry.machine_id] = true;
	entries_.erase(it);
	return out;
}

bool BootstrapStore::awaiting_fetch(const std::string &machine_id) const {
	std::lock_guard<std::mutex> lock(mu_);
	const auto it = machines_.find(machine_id);
	return it != machines_.end() && !it->second;
}

void BootstrapStore::forget_machine(const std::string &machine_id) {
	std::lock_guard<std::mutex> lock(mu_);
	machines_.erase(machine_id);
	for (auto it = entries_.begin(); it != entries_.end();) {
		it = it->second.machine_id == machine_id ? entries_.erase(it) : std::next(it);
	}
}

size_t BootstrapStore::live_tokens() const {
	std::lock_guard<std::mutex> lock(mu_);
	size_t live = 0;
	const uint64_t now = now_();
	for (const auto &entry : entries_) {
		if (now < entry.second.expires_ms) ++live;
	}
	return live;
}

} // namespace opennova::novaworld_server::hosted
