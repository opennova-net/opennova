#include <novaworld/connection/registry.h>

#include <utility>

namespace opennova {

void ConnectionRegistry::add(Connection conn) {
	std::lock_guard<std::mutex> lock(mu_);

	// If the same addr already has an entry, evict it (the new HELLO
	// replaces it).
	auto addr_it = by_addr_.find(conn.addr);
	if (addr_it != by_addr_.end()) {
		by_id_.erase(addr_it->second);
		by_addr_.erase(addr_it);
	}

	// G.7: if conn.id is already taken by a *different* addr, synthesize
	// a unique id. Two retail processes both send ci=0x00000001, so
	// trusting the client-supplied id verbatim made the second AUTH
	// alias the first connection's slot — every find_by_addr for the
	// first addr then returned the second's data, and we encrypted the
	// first peer's outbound SESSION with the second peer's scrk
	// (witnessed as "INCOMING PACKET ERROR" in retail's _connectlog.txt
	// 2026-04-28). Synthetic ids live in the 0x80000000-0xFFFFFFFF range
	// so they can't collide with retail's first-connection CIs (0x0001..).
	if (by_id_.find(conn.id) != by_id_.end()) {
		while (by_id_.find(next_synth_id_) != by_id_.end()) ++next_synth_id_;
		conn.id = next_synth_id_++;
	}

	const uint32_t id = conn.id;
	const PeerAddr addr = conn.addr;
	by_id_[id] = std::move(conn);
	by_addr_[addr] = id;
}

void ConnectionRegistry::touch(uint32_t id, uint64_t now_ms) {
	std::lock_guard<std::mutex> lock(mu_);
	auto it = by_id_.find(id);
	if (it != by_id_.end()) {
		it->second.last_seen_ms = now_ms;
	}
}

void ConnectionRegistry::touch_addr(const PeerAddr &addr, uint64_t now_ms) {
	std::lock_guard<std::mutex> lock(mu_);
	auto addr_it = by_addr_.find(addr);
	if (addr_it == by_addr_.end()) {
		return;
	}
	auto id_it = by_id_.find(addr_it->second);
	if (id_it != by_id_.end()) {
		id_it->second.last_seen_ms = now_ms;
	}
}

std::optional<Connection> ConnectionRegistry::drop(uint32_t id) {
	std::lock_guard<std::mutex> lock(mu_);
	auto it = by_id_.find(id);
	if (it == by_id_.end()) {
		return std::nullopt;
	}
	Connection out = std::move(it->second);
	by_addr_.erase(out.addr);
	by_id_.erase(it);
	return out;
}

std::optional<Connection> ConnectionRegistry::drop_by_addr(const PeerAddr &addr) {
	std::lock_guard<std::mutex> lock(mu_);
	auto addr_it = by_addr_.find(addr);
	if (addr_it == by_addr_.end()) return std::nullopt;
	const uint32_t id = addr_it->second;
	by_addr_.erase(addr_it);
	auto id_it = by_id_.find(id);
	if (id_it == by_id_.end()) return std::nullopt;
	Connection out = std::move(id_it->second);
	by_id_.erase(id_it);
	return out;
}

bool ConnectionRegistry::mark_active(uint32_t id, std::string identity,
                                     std::string client_scrk, std::string server_scrk) {
	std::lock_guard<std::mutex> lock(mu_);
	auto it = by_id_.find(id);
	if (it == by_id_.end()) {
		return false;
	}
	if (it->second.state == ConnectionState::Active) {
		return false;
	}
	it->second.state = ConnectionState::Active;
	it->second.identity = std::move(identity);
	it->second.client_scrk = std::move(client_scrk);
	it->second.server_scrk = std::move(server_scrk);
	return true;
}

bool ConnectionRegistry::mark_active_by_addr(const PeerAddr &addr,
                                             std::string identity,
                                             std::string client_scrk,
                                             std::string server_scrk) {
	std::lock_guard<std::mutex> lock(mu_);
	auto addr_it = by_addr_.find(addr);
	if (addr_it == by_addr_.end()) return false;
	auto it = by_id_.find(addr_it->second);
	if (it == by_id_.end()) return false;
	// Re-promote even if already Active so a fresh AUTH (e.g. after a
	// dropped reply) refreshes the SCRKs.
	it->second.state = ConnectionState::Active;
	it->second.identity = std::move(identity);
	it->second.client_scrk = std::move(client_scrk);
	it->second.server_scrk = std::move(server_scrk);
	return true;
}

std::optional<Connection> ConnectionRegistry::find(uint32_t id) const {
	std::lock_guard<std::mutex> lock(mu_);
	auto it = by_id_.find(id);
	if (it == by_id_.end()) {
		return std::nullopt;
	}
	return it->second;
}

std::optional<Connection> ConnectionRegistry::find_by_addr(const PeerAddr &addr) const {
	std::lock_guard<std::mutex> lock(mu_);
	auto addr_it = by_addr_.find(addr);
	if (addr_it == by_addr_.end()) {
		return std::nullopt;
	}
	auto id_it = by_id_.find(addr_it->second);
	if (id_it == by_id_.end()) {
		return std::nullopt;
	}
	return id_it->second;
}

std::vector<Connection> ConnectionRegistry::snapshot() const {
	std::lock_guard<std::mutex> lock(mu_);
	std::vector<Connection> out;
	out.reserve(by_id_.size());
	for (const auto &[_, conn] : by_id_) {
		out.push_back(conn);
	}
	return out;
}

std::size_t ConnectionRegistry::size() const {
	std::lock_guard<std::mutex> lock(mu_);
	return by_id_.size();
}

std::vector<uint32_t> ConnectionRegistry::iter_expired(uint64_t now_ms, uint64_t timeout_ms) const {
	std::lock_guard<std::mutex> lock(mu_);
	std::vector<uint32_t> out;
	for (const auto &[id, conn] : by_id_) {
		if (conn.last_seen_ms + timeout_ms < now_ms) {
			out.push_back(id);
		}
	}
	return out;
}

} // namespace opennova
