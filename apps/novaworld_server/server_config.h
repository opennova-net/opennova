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
	// Public host the server advertises in gate responses + lobby URLs.
	// Set to the LAN IP for cross-machine testing; localhost is enough
	// for single-host dev.
	std::string public_host = "127.0.0.1";

	// Gate UDP — bootstrap probes from clients land here (port 7597 in
	// retail). Reply contains POSTIPADDRESS + UDPNOVAWORLD + STARTUPURL.
	uint16_t gate_udp_port = opennova::GATE_DEFAULT_PORT;

	// NW UDP — Layer 2-3 NAPI traffic (HELLO/JOIN/SESSION/GOODBYE) on
	// port 64206 in retail. Same socket serves both lobby and in-match
	// clients (PN dispatch — see README.md "Listeners").
	uint16_t nw_udp_port = 64206;

	// HTTP — Crow binds here. Serves /api/* + falls back to web/dist/
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

	// Receive-silence reap for NOVAWORLDUDP peers. Not configurable: it is
	// the cs[0] timeout the 0x82 SessionInit advertises to the peer (240 s),
	// so the two can never disagree (see
	// engine/net/novaworld/connection/manager.h).
	uint64_t heartbeat_timeout_ms = opennova::novaworldudp_session_timeout_ms();

	// Tick interval for the connection manager's expire pass.
	uint64_t tick_interval_ms = 500;

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

	// ONNET_BOOTSTRAP_ADMIN: an existing account main() gives the admin role
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
	// opennova-nw-lister without an account list. Service policy
	// (LobbySession::set_require_host_key).
	bool require_host_key = false;

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

	// Hosted servers (apps/novaworld_server/hosted, ADR 0053 proposed): where the
	// supervisor runs them. "none" (the default) runs none and the start route
	// answers that hosting is off; "local" runs each as an opennova-serve child
	// process on this machine (LocalProcessProvider), for development and the
	// tests; "ec2" runs each on an EC2 machine of its own (Ec2Provider, the
	// settings below). ONNET_HOSTED_PROVIDER; any other value is "none", with a
	// warning.
	std::string hosted_provider = "none";
	// ONNET_HOSTED_CAP: the most hosted servers live at once, across every
	// account (each account has one at most).
	int hosted_cap = 5;
	// The local provider's opennova-serve binary (ONNET_HOSTED_SERVE_BIN), the
	// game data it serves (ONNET_HOSTED_RESOURCE_DIR, its --resource-dir;
	// ONNET_HOSTED_LOOSE_ROOT=1 adds --loose-root for a directory with no
	// archives) and the root of the servers' working directories
	// (ONNET_HOSTED_WORK_DIR). The first two are required for "local".
	std::string hosted_serve_bin;
	std::string hosted_resource_dir;
	bool hosted_loose_root = false;
	std::string hosted_work_dir = "backend/data/hosted";
	// ONNET_HOSTED_MISSIONS: the rotation a start takes when its request names
	// none (comma-separated .bms names, the first the starting map). Empty: a
	// start must name its missions.
	std::vector<std::string> hosted_missions;

	// The ec2 provider's (hosted/ec2_provider.h), each set by the deploy
	// toolbox's `app deploy` from the Terraform outputs (DEPLOY.md, "Hosted game
	// servers"); every one is required for "ec2". The region
	// (ONNET_HOSTED_EC2_REGION), the launch template
	// (ONNET_HOSTED_EC2_LAUNCH_TEMPLATE) and the subnet (ONNET_HOSTED_EC2_SUBNET)
	// a machine launches from; the deployment tag every call is scoped to
	// (ONNET_HOSTED_DEPLOYMENT); the serve image, pinned
	// (ONNET_HOSTED_SERVE_IMAGE); this service's PRIVATE address
	// (ONNET_HOSTED_SERVICE_ADDRESS: where the machines fetch their bootstrap
	// bundle and the address their admin.cfg admits); the private networks the
	// bootstrap route answers (ONNET_HOSTED_BOOTSTRAP_CIDRS, comma-separated
	// IPv4 CIDRs: the VPC's subnet); the AWS CLI (ONNET_HOSTED_AWS_CLI, an
	// absolute path); and the machine script (ONNET_HOSTED_MACHINE_SCRIPT, the
	// user-data template, hosted/machine/user_data.sh, which the server image
	// ships at the default path; a run outside the image names its own).
	std::string hosted_ec2_region;
	std::string hosted_ec2_launch_template;
	std::string hosted_ec2_subnet;
	std::string hosted_deployment;
	std::string hosted_serve_image;
	std::string hosted_service_address;
	std::vector<std::string> hosted_bootstrap_cidrs;
	std::string hosted_aws_cli = "/usr/local/bin/aws";
	std::string hosted_machine_script = "/app/hosted/machine_user_data.sh";

	// Build a config by reading env vars, falling back to the defaults
	// above for anything not set.
	static ServerConfig from_env();
};

} // namespace opennova::novaworld_server
