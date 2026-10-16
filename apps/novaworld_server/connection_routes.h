#pragma once

// The admin's NovaWorld sessions view: GET /api/admin/connections, every
// connection the ConnectionManager's registry holds (a game client's or a
// host's NovaWorld UDP session, from its ClientHello), with who it is and the
// server it lists from the NW UDP listener's lobby state, and POST
// /api/admin/connections/<id>/end, which ends one (NwUdpListener::end_session).
// The HttpListener owns one ConnectionRoutes over its Crow app, beside its
// HostRoutes (README "Connection lifecycle").

#include "web_access.h"

#include <net/novaworld/db/database.h>

#include <crow.h>

#include <cstdint>

namespace opennova {
class ConnectionManager;
}

namespace opennova::novaworld_server {

class NwUdpListener;

// A connection's clock stamp (Connection::created_ms / last_seen_ms) as Unix
// epoch milliseconds. The stamps are the steady clock in ms, the clock the NW
// UDP listener stamps and the reap reads, which counts from an unspecified
// start (on Linux the machine's boot), so a stamp read as an epoch time lands
// in January 1970. This is the wall time `unix_now_ms` less the stamp's age on
// the steady clock at `steady_now_ms`; a stamp ahead of `steady_now_ms` (none
// is) reads as now.
int64_t unix_ms_of_steady(uint64_t steady_ms, uint64_t steady_now_ms, int64_t unix_now_ms);

class ConnectionRoutes {
public:
	ConnectionRoutes(WebAccess &access, db::ConnectionPool &pool) : access_(access), pool_(pool) {}

	ConnectionRoutes(const ConnectionRoutes &) = delete;
	ConnectionRoutes &operator=(const ConnectionRoutes &) = delete;

	// Registers the two routes on `app`. `manager` holds the connections and
	// `nw_udp` (null: none, and every end is a 503) their lobby state; both must
	// outlive the routes.
	void register_routes(WebApp &app, ConnectionManager &manager, NwUdpListener *nw_udp);

private:
	crow::response list(const crow::request &req);
	crow::response end(const crow::request &req, uint64_t id);

	WebAccess &access_;
	db::ConnectionPool &pool_;
	ConnectionManager *manager_ = nullptr;
	NwUdpListener *nw_udp_ = nullptr;
};

} // namespace opennova::novaworld_server
