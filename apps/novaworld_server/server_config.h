#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace opennova::server {

// Standalone novaworld server configuration. All fields populate from env
// vars at startup (see ServerConfig::from_env), with sensible defaults so a
// fresh checkout boots cleanly with no setup.
//
// Env-var names mirror onnet's `ONNET_*` convention (see memory
// `project_local_dev_flow.md`) so existing dev flows keep working.
struct ServerConfig {
	// Public host the server advertises in gate responses + lobby URLs.
	// Set to the LAN IP for cross-machine testing; localhost is enough
	// for single-host dev.
	std::string public_host = "127.0.0.1";

	// Gate UDP — bootstrap probes from clients land here (port 7597 in
	// retail). Reply contains POSTIPADDRESS + UDPNOVAWORLD + STARTUPURL.
	uint16_t gate_udp_port = 7597;

	// NW UDP — Layer 2-3 NAPI traffic (HELLO/JOIN/SESSION/GOODBYE) on
	// port 64206 in retail. Same socket serves both lobby and in-match
	// clients (PN dispatch — see notes/architecture.md).
	uint16_t nw_udp_port = 64206;

	// HTTP — Drogon binds here. Serves /api/* + falls back to web/dist/
	// for the static SPA.
	uint16_t http_port = 8080;

	// SQLite path. Created on first boot if missing.
	std::filesystem::path database_path = "backend/data/state.db";

	// Migrations + seed dirs. Read at startup.
	std::filesystem::path migrations_dir = "backend/migrations";
	std::filesystem::path seed_dir       = "backend/seed";

	// Vue static assets (the `npm run build` output). Served by the HTTP
	// fallback route. If missing, /api/* still works but the SPA route
	// returns 404.
	std::filesystem::path web_dist_dir   = "web/dist";

	// Game-client templates (.htm / .mnx / .joi files retail's NW*.dll
	// HTTP flow downloads). Copied from onnet/onnw/templates/.
	std::filesystem::path templates_dir  = "apps/novaworld_server/templates";

	// Game-client static assets (.tga login backgrounds, etc.).
	std::filesystem::path static_dir     = "apps/novaworld_server/static";

	// Heartbeat timeout — clients last seen longer than this get dropped
	// from the connection registry. Default lines up with the upper end
	// of the witnessed RandomizeTimeout window plus slack (see
	// libs/novaworld/include/novaworld/connection/manager.h).
	uint64_t heartbeat_timeout_ms = 120000;

	// Tick interval for the connection manager's expire pass.
	uint64_t tick_interval_ms = 500;

	// policy: backstop sweep of crash-orphaned active_hosts rows. Cadence
	// = how often the sweep runs; window = how stale (no ClientHostUpdate
	// heartbeat) a host row must be before it's pruned. The normal teardown
	// (GOODBYE / StopHosting / heartbeat-timeout) handles the common case;
	// this only catches rows that slipped through.
	uint64_t host_sweep_interval_ms = 30000;
	uint64_t host_stale_window_ms   = 300000;

	// Bearer token for /api/admin/* endpoints. When unset (default), the
	// admin endpoints are CLOSED — they always return 401. Set via
	// ADMIN_API_TOKEN env var to enable. Tokens are compared with a
	// constant-time string compare in `auth.cpp`.
	std::string admin_api_token;

	// Build a config by reading env vars, falling back to the defaults
	// above for anything not set.
	static ServerConfig from_env();
};

} // namespace opennova::server
