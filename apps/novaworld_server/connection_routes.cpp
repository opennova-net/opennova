#include "connection_routes.h"

#include "http_json.h"

#include <net/novaworld/connection/manager.h>

#include <chrono>
#include <cstdint>
#include <vector>

namespace opennova::novaworld_server {

namespace {

const char *connection_state_name(ConnectionState s) {
	switch (s) {
		case ConnectionState::Handshaking:
			return "handshaking";
		case ConnectionState::Active:
			return "active";
		case ConnectionState::Closing:
			return "closing";
	}
	return "unknown";
}

} // namespace

int64_t unix_ms_of_steady(uint64_t steady_ms, uint64_t steady_now_ms, int64_t unix_now_ms) {
	const uint64_t age = steady_now_ms > steady_ms ? steady_now_ms - steady_ms : 0;
	return unix_now_ms - static_cast<int64_t>(age);
}

void ConnectionRoutes::register_routes(
		WebApp &app, ConnectionManager &manager, NwUdpListener *nw_udp) {
	manager_ = &manager;
	nw_udp_ = nw_udp;
	CROW_ROUTE(app, "/api/admin/connections")
			.methods("GET"_method)([this](const crow::request &req) { return list(req); });
}

// GET /api/admin/connections — {"count", "heartbeat_timeout_ms", "connections"}:
// each connection's registry id, state, the protocol name (pn) and NA its
// hello and auth carried (identity), its address, and when it connected and
// was last heard from (created_ms, last_seen_ms) as Unix epoch ms.
crow::response ConnectionRoutes::list(const crow::request &req) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	using namespace std::chrono;
	// The registry's clock and the wall clock, read together.
	const uint64_t steady_now = static_cast<uint64_t>(
			duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
	const int64_t unix_now = static_cast<int64_t>(
			duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
	const auto snapshot = manager_->registry().snapshot();
	std::vector<crow::json::wvalue> entries;
	entries.reserve(snapshot.size());
	for (const auto &c : snapshot) {
		crow::json::wvalue e;
		e["id"] = c.id;
		e["state"] = connection_state_name(c.state);
		e["pn"] = c.pn;
		e["identity"] = c.identity;
		e["created_ms"] = unix_ms_of_steady(c.created_ms, steady_now, unix_now);
		e["last_seen_ms"] = unix_ms_of_steady(c.last_seen_ms, steady_now, unix_now);
		e["addr"] = peer_addr_to_string(c.addr);
		entries.push_back(std::move(e));
	}
	crow::json::wvalue out;
	out["count"] = snapshot.size();
	out["heartbeat_timeout_ms"] = manager_->heartbeat_timeout_ms();
	out["connections"] = std::move(entries);
	return json_reply(200, out);
}

} // namespace opennova::novaworld_server
