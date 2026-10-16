#pragma once

// The admin's NovaWorld sessions view: GET /api/admin/connections, every
// session the lobby instances hold (a game client's or a host's NovaWorld UDP
// session, from its ClientHello), with who it is, the server it lists and the
// instance holding it, read from the lobbies' session rows
// (lobbydb::list_sessions, lobby_repository.h), and POST
// /api/admin/connections/<session id>/end, which ends one through a command for
// the lobby holding it (lobbydb::queue_end_session); and the lobby instances:
// GET /api/admin/lobbies, and POST /api/admin/lobbies/<id>/drain, after which no
// gate hands that instance a new client. None reaches a lobby's memory. The
// HttpListener owns one ConnectionRoutes over its Crow app, beside its
// HostRoutes (README "Connection lifecycle", "Lobby instances").

#include "web_access.h"

#include <net/novaworld/db/database.h>

#include <crow.h>

#include <cstdint>

namespace opennova::novaworld_server {

class ConnectionRoutes {
public:
	ConnectionRoutes(WebAccess &access, db::ConnectionPool &pool) : access_(access), pool_(pool) {}

	ConnectionRoutes(const ConnectionRoutes &) = delete;
	ConnectionRoutes &operator=(const ConnectionRoutes &) = delete;

	// Registers the four routes on `app`. `heartbeat_timeout_ms` is the receive
	// silence the lobby reaps a session after (ServerConfig::heartbeat_timeout_ms),
	// which the list reports.
	void register_routes(WebApp &app, uint64_t heartbeat_timeout_ms);

private:
	crow::response list(const crow::request &req);
	crow::response end(const crow::request &req, uint64_t session_id);
	crow::response lobbies(const crow::request &req);
	crow::response drain(const crow::request &req, uint64_t id);

	WebAccess &access_;
	db::ConnectionPool &pool_;
	uint64_t heartbeat_timeout_ms_ = 0;
};

} // namespace opennova::novaworld_server
