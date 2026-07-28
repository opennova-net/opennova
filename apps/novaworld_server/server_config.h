#pragma once

#include <novaworld/gate_probe.h>
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
	uint16_t gate_udp_port = opennova::GATE_DEFAULT_PORT;

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

	// Whether to apply the dev-only seed files (0002_dev_users.sql — the
	// `test`/`foo` accounts with publicly-documented passwords). OFF by
	// default so production never creates them; the dev compose sets
	// SEED_DEV_USERS=1 to keep the local multi-client test accounts.
	bool seed_dev_users = false;

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

	// Expansion-publish pipeline (ported from onnet). Both default empty:
	//   expansion_github_token  (EXPANSION_GITHUB_TOKEN) — a GitHub PAT with
	//     contents:write on the expansion repos. When empty, POST .../release
	//     still records the release row but reports the tag step as failed
	//     (onnet admin.py:71-74).
	//   expansion_publish_token (EXPANSION_PUBLISH_TOKEN) — bearer token the
	//     expansion repo's CI presents to /admin/internal/.../publish|fail.
	//     When empty those routes return 500 (onnet admin_internal.py:18-19).
	// These are distinct from admin_api_token.
	std::string expansion_github_token;
	std::string expansion_publish_token;

	// The slug -> "owner/repo" mapping for the tag push is no longer held here:
	// it lives in the expansions table's github_repo column, sourced from the
	// Terraform-managed catalogue seed and read per-release in http_listener.

	// Reflection override for the host/join flow (dev/NAT). NovaWorld tells a
	// hosting client its reachable endpoint and advertises it to joiners. On the
	// open internet the observed UDP source IS that endpoint, but behind a docker
	// bridge the observed source is the proxy gateway (172.x:ephemeral) —
	// unreachable by a joiner, the "stuck on Enumerating" symptom. When set,
	// these force a locally reachable value. Leave empty/0 in prod. Port 0 == unset.
	//
	// TWO ports, mirroring onnet's separate gate / nw_udp processes (and JO
	// game.cfg). The IP is shared by both. Each port is the retail client's LOCAL
	// UDP port for that socket (docker hides the real source, so we set them):
	//   client_reflect_gate_port      = game.cfg mpgateserverlocalport (49152);
	//     echoed back to the client as the gate's ReflectedPortNumber.
	//   client_reflect_novaworld_port = game.cfg mpnovaworldport (32768); the game
	//     host's session socket, advertised to joiners as the endpoint they dial
	//     to enumerate/join.
	std::string client_reflect_ip;                  // shared: gate ReflectedIpAddress + advertised host IP
	uint16_t    client_reflect_gate_port = 0;       // ONNET_CLIENT_REFLECT_GATE_PORT
	uint16_t    client_reflect_novaworld_port = 0;  // ONNET_CLIENT_REFLECT_NOVAWORLD_PORT

	// Build a config by reading env vars, falling back to the defaults
	// above for anything not set.
	static ServerConfig from_env();
};

} // namespace opennova::server
