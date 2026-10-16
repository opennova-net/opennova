#pragma once

#include <net/novaworld/connection/manager.h>
#include <net/novaworld/gate_probe.h>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace opennova::novaworld_server {

// Standalone novaworld server configuration. All fields populate from env
// vars at startup (see ServerConfig::from_env), with sensible defaults so a
// fresh checkout boots cleanly with no setup.
//
// Env-var names mirror onnet's `ONNET_*` convention (see memory
// `project_local_dev_flow.md`) so existing dev flows keep working.
struct ServerConfig {
	// The gate's public address (ONNET_PUBLIC_HOST): its POSTIPADDRESS, where
	// a hosting client's status blobs land, the hosted servers' master host on
	// a cloud machine, and the switcher's address. Set to the LAN IP for
	// cross-machine testing; localhost is enough for single-host dev.
	std::string public_host = "127.0.0.1";
	// Whether ONNET_PUBLIC_HOST was set (non-empty) in the environment: the
	// gate requires it, since a gate left on the default would announce
	// loopback to every client; an explicit 127.0.0.1 is accepted.
	bool public_host_set = false;

	// The one load-balanced web endpoint every advertised URL names
	// (ONNET_WEB_HOST, a name or an address), with http_port: the gate's
	// startupurl, the 0x82 SessionInit's web domain (the lobby), and the menus'
	// HOST_URL and GSB_SERVER (web), so all three agree. Required by the gate,
	// the lobby and web.
	std::string web_host;

	// The lobby address the gates hand clients (ONNET_LOBBY_ENDPOINT,
	// <host>:<port>): their udpnovaworld. Every lobby announces it (its
	// instance row), the deployment's one lobby address: on ECS the load
	// balancer's, the gate's Elastic IP and the lobby port, which spreads the
	// clients across the lobby tasks. Required by the lobby; the gate's is
	// optional, the one it hands out while no lobby instance takes clients.
	// Never derived from the port a lobby bound.
	std::string lobby_endpoint;

	// Gate UDP — bootstrap probes from clients land here (port 7597 in
	// retail). Reply contains POSTIPADDRESS + UDPNOVAWORLD + STARTUPURL.
	uint16_t gate_udp_port = opennova::GATE_DEFAULT_PORT;

	// NW UDP — Layer 2-3 NAPI traffic (HELLO/JOIN/SESSION/GOODBYE) on
	// port 64206 in retail. Same socket serves both lobby and in-match
	// clients (PN dispatch — see README.md "Listeners").
	uint16_t nw_udp_port = 64206;

	// HTTP — web's Crow binds here. Serves /api/* + falls back to
	// web/dist/ for the static SPA. Also the port every advertised web URL
	// names beside web_host (the load balancer forwards it unchanged).
	uint16_t http_port = 8080;

	// The PostgreSQL database (db/database.h): DATABASE_URL, a libpq
	// connection URI (not secret; the boot line prints it, a password in it
	// masked), and DATABASE_PASSWORD, the database user's password, kept apart from it
	// (secret: registered as a log secret at boot). The default URL is
	// scripts/dev_postgres.sh's local server. DATABASE_POOL_MAX caps the
	// connections the service opens (db::ConnectionPool): the gate's, the NW
	// UDP listener's and main's for their lifetime, and one per HTTP request,
	// supervisor pass or admin-link call while it runs.
	std::string database_url = "postgresql://novaworld@127.0.0.1:5432/novaworld";
	std::string database_password;
	size_t database_pool_max = 16;

	// Web's Redis (redis_client.h; the HTTP listener requires it):
	// ONNET_REDIS_URL, redis://host[:port][/db] or rediss:// for TLS (not
	// secret; the boot line prints it, a password in it masked), and
	// ONNET_REDIS_PASSWORD, the AUTH password, kept apart from it (secret:
	// registered as a log secret at boot). The default URL is
	// scripts/dev_redis.sh's local server.
	std::string redis_url = "redis://127.0.0.1:6379";
	std::string redis_password;

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
	std::filesystem::path templates_dir = "apps/novaworld_web/templates";

	// Game-client static assets (.tga login backgrounds, etc.).
	std::filesystem::path static_dir = "apps/novaworld_web/static";

	// Receive-silence reap for NOVAWORLDUDP peers. Not configurable: it is
	// the cs[0] timeout the 0x82 SessionInit advertises to the peer (240 s),
	// so the two can never disagree (see
	// engine/net/novaworld/connection/manager.h).
	uint64_t heartbeat_timeout_ms = opennova::novaworldudp_session_timeout_ms();

	// Tick interval for the connection manager's expire pass.
	uint64_t tick_interval_ms = 500;

	// ONNET_HEALTH_PORT: the TCP port GET /livez answers on
	// (health_listener.h) in the gate, the lobby, the controller and the
	// worker; web answers it on http_port. 0: the OS picks (the tests).
	uint16_t health_port = 8081;

	// The controller's internal listener bind its boot found in its ECS task
	// (<private IPv4>:8070, ecs_metadata.h) with ONNET_INTERNAL_HTTP_BIND
	// unset; empty: the environment's (hosted/fleet_config.h reads it). Not an
	// environment variable of its own.
	std::string internal_bind;

	// MET (metrics) endpoint the gate response advertises
	// (METIPADDRESS / METIPPORT / METLABEL / METPING / METEXT, all read by
	// [orig: CNapiGateManager_ProcessResponse @0x4ced20]). Emitted only when
	// met_ip is set; the client's metrics leg needs the label too.
	std::string met_ip;                // ONNET_MET_IP
	uint16_t    met_port = 0;          // ONNET_MET_PORT
	std::string met_label;             // ONNET_MET_LABEL
	int         met_ping = 0;          // ONNET_MET_PING
	int         met_ext  = 0;          // ONNET_MET_EXT

	// GLSVSS (the gate's character-data refresh leg): the gate advertises
	// GLSVSSREQUEST / GLSVSSRIMS / GLSVSSAGRMS when glsvss_request is set, the
	// client then periodically sends ClientGLSVSSRequest
	// [orig: CNapiGameSession_ProcessPeriodicUpdate @0x4d44cb..0x4d4532] and
	// the lobby answers ServerGLSVSSResults with glsvss_results.
	std::string glsvss_request;        // ONNET_GLSVSS_REQUEST
	int         glsvss_rims  = 0;      // ONNET_GLSVSS_RIMS
	int         glsvss_agrms = 0;      // ONNET_GLSVSS_AGRMS
	std::string glsvss_results;        // ONNET_GLSVSS_RESULTS

	// policy: backstop sweep of crash-orphaned active_hosts rows (the
	// hosts.prune job, jobs/maintenance_jobs.h). Cadence = how often the
	// sweep's jobs run (the other prunes ride it); window = how stale (no
	// ClientHostUpdate heartbeat) a host row must be before it's pruned. The
	// normal teardown (GOODBYE / StopHosting / heartbeat-timeout) handles the
	// common case; this only catches rows that slipped through.
	uint64_t host_sweep_interval_ms = 30000;
	uint64_t host_stale_window_ms   = 300000;

	// ONNET_WORKER_THREADS: the job workers' threads (jobs/worker_pool.h),
	// 1..64.
	int worker_threads = 4;

	// Bearer token for /api/admin/* endpoints. When unset (default), the
	// admin endpoints are CLOSED — they always return 401. Set via
	// ADMIN_API_TOKEN env var to enable. Tokens are compared with a
	// constant-time string compare in `auth.cpp`.
	std::string admin_api_token;

	// The website session cookie's Secure flag (ONNET_COOKIE_SECURE=1). Set
	// it wherever browsers reach the site over https: prod, behind
	// Cloudflare's TLS (DEPLOY.md), where nginx and this server still speak
	// plain http, so no request header can tell. Off by default for the
	// http://localhost dev site.
	bool cookie_secure = false;

	// The reverse proxies trusted to name the client in X-Real-IP /
	// X-Forwarded-For (ONNET_TRUSTED_PROXIES, comma-separated exact addresses;
	// prod's nginx reaches this server from 127.0.0.1). Empty (the default):
	// the TCP peer is the client, and those headers are ignored.
	std::vector<std::string> trusted_proxies;

	// ONNET_API_PROXY_ONLY=1: the website's API (/api and everything under it)
	// answers 403 to any TCP peer not in trusted_proxies, so it is reached only
	// through the proxy that applies the site's edge checks (prod's nginx on
	// 127.0.0.1, DEPLOY.md). The retail routes a stock client reaches on this
	// port directly (NW*.dll, the .gsb feeds, templates, static files) stay
	// open. Off by default (the dev site's Vite proxy is in no trusted list).
	bool api_proxy_only = false;

	// ONNET_BOOTSTRAP_ADMIN: an existing account web gives the admin role
	// at boot, the way the first site admin is made. A name no account has is
	// a warning.
	std::string bootstrap_admin;

	// ONNET_REQUIRE_HOST_KEY=1: list no host whose HostKey names no account
	// (the key a logged-in client's /NWHost.dll fetch minted). A request
	// carrying such a key gets ServerHostResult Success=0, MsgCode 200 (a stock
	// client's NWEC58); a keyless request (a stock host's fresh one) is held out
	// of the browser until an update's key names the owner, and ended with
	// ServerStopHosting (MsgCode 200) when it names none. Off by default: such a
	// host is listed with no owner, as opennova-serve without --credentials and
	// a game host on no account list. Service policy
	// (LobbySession::set_require_host_key).
	bool require_host_key = false;

	// The site's origin (ONNET_SITE_URL, its trailing '/' dropped): the links
	// the welcome mail and the password reset mail carry (mailer.h). Billing
	// reads the same variable for its return URLs (billing/billing_config.h).
	std::string site_url;

	// Outbound mail through Amazon SES (mailer.h, README "Account email and
	// password resets"): ONNET_MAIL_FROM, the sender's address
	// (support@<domain>), and ONNET_MAIL_REGION, SES's region. Mail is on only
	// with both and ONNET_SITE_URL set; web and the worker log why it
	// is off otherwise.
	std::string mail_from;
	std::string mail_region;

	// ONNET_HOSTED_AWS_CLI: the AWS CLI v2 every call of it runs (an absolute
	// path, run as given, no PATH search): the lobby's ECS discovery
	// (ecs_metadata.h), the mailer's SES calls (mailer.h); the fleet's EC2
	// driver reads it with its settings (hosted/fleet_config.h).
	std::string aws_cli = "/usr/local/bin/aws";

	// ONNET_CD_KEY_AUTO_MINT (on unless set to 0): a DFX2 account that has never
	// held a CD key gets one minted onto it at its first activation, so a player
	// need not wait on an admin (cd_keys.h, cd_key_routes.h). Off, only a key an
	// admin assigned activates. Service policy: NovaLogic's issuer is unwitnessed.
	bool cd_key_auto_mint = true;

	// ONNET_EMBEDDED_JO_HOST=1: run an in-match JO host in this process on the
	// NovaWorld UDP port, so a Hello carrying JO's complete in-match identity
	// there is served as a game session (NwUdpListener::initialize_jo_host).
	// The nw_host_register_e2e harness turns it on to drive the listener's
	// game-session responder; off by default, so the service runs no game host
	// beside its lobby, HTTP and billing.
	bool embedded_jo_host = false;

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

	// Hosted servers (apps/novaworld_common/hosted, ADR 0053 proposed): the
	// machine driver the supervisor runs their machines on. "none" (the
	// default) runs none and the start route answers that hosting is off;
	// "ec2" launches EC2 machines from the shared and own launch templates
	// (Ec2MachineDriver); "docker" launches each machine as a container of the
	// serve image on the local stack's network (DockerMachineDriver,
	// deploy/compose). ONNET_HOSTED_PROVIDER; any other value is "none", with a
	// warning, as is a driver with no object store (below). The fleet's own
	// settings (the internal listener, the machines' knobs, each driver's
	// values, the serve and hostd images) are hosted/fleet_config.h's.
	std::string hosted_provider = "none";
	// ONNET_HOSTED_CAP: the most hosted servers live at once, across every
	// account, a backstop beside the fleet's own room.
	int hosted_cap = 24;
	// The object store the hosted game data lives in (object_store.h, ADR 0064):
	// ONNET_OBJECT_STORE_URL, `s3://<bucket>/data` (the deployment's own ops
	// bucket) with ONNET_OBJECT_STORE_REGION, the bucket's region, or
	// `file:///<dir>` for the local stack and the tests (its machines copy trees
	// from that directory). Hosting is on only with a store that parses, on
	// either driver: the data sets' trees live in it, hostd fetches them from it,
	// and web's admin page uploads into it.
	std::string object_store_url;
	std::string object_store_region;

	// Build a config by reading env vars, falling back to the defaults
	// above for anything not set.
	static ServerConfig from_env();
};

} // namespace opennova::novaworld_server
