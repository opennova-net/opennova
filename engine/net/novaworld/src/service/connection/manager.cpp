#include <novaworld/connection/manager.h>

#include <utility>

namespace opennova {

const char *drop_reason_name(DropReason r) {
	switch (r) {
	case DropReason::Logout:   return "logout";
	case DropReason::Timeout:  return "timeout";
	case DropReason::Replaced: return "replaced";
	case DropReason::Shutdown: return "shutdown";
	}
	return "unknown";
}

void ConnectionManager::notify_handshake(Connection conn) {
	if (conn.reported_id == 0) conn.reported_id = conn.id;
	const auto existing_for_addr = registry_.find_by_addr(conn.addr);
	if (existing_for_addr &&
	    existing_for_addr->reported_id == conn.reported_id &&
	    existing_for_addr->pn == conn.pn) {
		// ClientHello is retransmitted before ClientAuth (and may be repeated
		// later when ServerHello was delayed). Preserve the registry's
		// synthetic id, Active state, SCRKs, and the listener's cached auth.
		return;
	}
	if (existing_for_addr && lost_handler_) {
		lost_handler_(*existing_for_addr, DropReason::Replaced);
	}
	registry_.add(conn);
	if (added_handler_) {
		added_handler_(conn);
	}
}

void ConnectionManager::notify_active(uint32_t id, std::string identity,
                                       std::string client_scrk, std::string server_scrk) {
	registry_.mark_active(id, std::move(identity),
	                      std::move(client_scrk), std::move(server_scrk));
}

void ConnectionManager::notify_active_addr(const PeerAddr &addr,
                                            std::string identity,
                                            std::string client_scrk,
                                            std::string server_scrk) {
	registry_.mark_active_by_addr(addr, std::move(identity),
	                              std::move(client_scrk), std::move(server_scrk));
}

void ConnectionManager::notify_seen(uint32_t id, uint64_t now_ms) {
	registry_.touch(id, now_ms);
}

void ConnectionManager::notify_seen_addr(const PeerAddr &addr, uint64_t now_ms) {
	registry_.touch_addr(addr, now_ms);
}

void ConnectionManager::notify_logout(uint32_t id) {
	auto dropped = registry_.drop(id);
	if (dropped && lost_handler_) {
		lost_handler_(*dropped, DropReason::Logout);
	}
}

void ConnectionManager::notify_logout_addr(const PeerAddr &addr) {
	auto dropped = registry_.drop_by_addr(addr);
	if (dropped && lost_handler_) {
		lost_handler_(*dropped, DropReason::Logout);
	}
}

std::size_t ConnectionManager::tick(uint64_t now_ms) {
	const auto expired = registry_.iter_expired(now_ms, heartbeat_timeout_ms_);
	std::size_t dropped_count = 0;
	for (uint32_t id : expired) {
		auto dropped = registry_.drop(id);
		if (dropped) {
			++dropped_count;
			if (lost_handler_) {
				lost_handler_(*dropped, DropReason::Timeout);
			}
		}
	}
	return dropped_count;
}

void ConnectionManager::shutdown() {
	auto remaining = registry_.snapshot();
	for (const auto &conn : remaining) {
		registry_.drop(conn.id);
		if (lost_handler_) {
			lost_handler_(conn, DropReason::Shutdown);
		}
	}
}

} // namespace opennova
