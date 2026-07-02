#include "session_store.h"

#include <chrono>
#include <cstdio>
#include <random>
#include <utility>

namespace opennova::server {

namespace {

uint64_t now_ms() {
	using namespace std::chrono;
	return static_cast<uint64_t>(
		duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

} // namespace

std::string SessionStore::generate_tag(const std::string &dll_name) {
	static thread_local std::mt19937_64 gen{std::random_device{}()};
	std::uniform_int_distribution<int> rand5(0, 99999);
	std::uniform_int_distribution<uint32_t> hex8;
	char buf[160];
	std::snprintf(buf, sizeof(buf), "NWServer:%s:SESSIONTAG:%d:%08x",
	              dll_name.c_str(), rand5(gen), hex8(gen));
	return buf;
}

void SessionStore::put_login(const std::string &tag, LoginSession session) {
	std::lock_guard<std::mutex> lk(mu_);
	login_[tag] = LoginEntry{std::move(session), now_ms()};
}

std::optional<LoginSession> SessionStore::get_login(const std::string &tag) const {
	std::lock_guard<std::mutex> lk(mu_);
	auto it = login_.find(tag);
	if (it == login_.end()) return std::nullopt;
	return it->second.s;
}

void SessionStore::erase_login(const std::string &tag) {
	std::lock_guard<std::mutex> lk(mu_);
	login_.erase(tag);
}

void SessionStore::put_join(const std::string &tag, JoinSession session) {
	std::lock_guard<std::mutex> lk(mu_);
	join_[tag] = JoinEntry{std::move(session), now_ms()};
}

std::optional<JoinSession> SessionStore::get_join(const std::string &tag) const {
	std::lock_guard<std::mutex> lk(mu_);
	auto it = join_.find(tag);
	if (it == join_.end()) return std::nullopt;
	return it->second.s;
}

void SessionStore::erase_join(const std::string &tag) {
	std::lock_guard<std::mutex> lk(mu_);
	join_.erase(tag);
}

void SessionStore::put_host(const std::string &tag, HostSession session) {
	std::lock_guard<std::mutex> lk(mu_);
	host_[tag] = HostEntry{std::move(session), now_ms()};
}

std::optional<HostSession> SessionStore::get_host(const std::string &tag) const {
	std::lock_guard<std::mutex> lk(mu_);
	auto it = host_.find(tag);
	if (it == host_.end()) return std::nullopt;
	return it->second.s;
}

void SessionStore::erase_host(const std::string &tag) {
	std::lock_guard<std::mutex> lk(mu_);
	host_.erase(tag);
}

std::size_t SessionStore::login_count() const {
	std::lock_guard<std::mutex> lk(mu_);
	return login_.size();
}

std::size_t SessionStore::join_count() const {
	std::lock_guard<std::mutex> lk(mu_);
	return join_.size();
}

std::size_t SessionStore::host_count() const {
	std::lock_guard<std::mutex> lk(mu_);
	return host_.size();
}

std::size_t SessionStore::evict_older_than(uint64_t max_age_ms) {
	const uint64_t now = now_ms();
	const uint64_t cutoff = (now > max_age_ms) ? (now - max_age_ms) : 0;
	std::lock_guard<std::mutex> lk(mu_);
	std::size_t dropped = 0;
	auto sweep = [&](auto &map) {
		for (auto it = map.begin(); it != map.end();) {
			if (it->second.created_ms < cutoff) {
				it = map.erase(it);
				++dropped;
			} else {
				++it;
			}
		}
	};
	sweep(login_);
	sweep(join_);
	sweep(host_);
	return dropped;
}

} // namespace opennova::server
