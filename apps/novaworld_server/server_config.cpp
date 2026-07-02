#include "server_config.h"

#include <cstdlib>
#include <string>

namespace opennova::server {

namespace {

const char *getenv_safe(const char *name) {
	const char *v = std::getenv(name);
	return (v && *v) ? v : nullptr;
}

uint16_t getenv_u16(const char *name, uint16_t fallback) {
	const char *v = getenv_safe(name);
	if (!v) return fallback;
	try {
		return static_cast<uint16_t>(std::stoi(v));
	} catch (...) {
		return fallback;
	}
}

uint64_t getenv_u64(const char *name, uint64_t fallback) {
	const char *v = getenv_safe(name);
	if (!v) return fallback;
	try {
		return std::stoull(v);
	} catch (...) {
		return fallback;
	}
}

bool getenv_bool(const char *name, bool fallback) {
	const char *v = getenv_safe(name);
	if (!v) return fallback;
	return *v == '1' || *v == 't' || *v == 'T' || *v == 'y' || *v == 'Y';
}

} // namespace

ServerConfig ServerConfig::from_env() {
	ServerConfig c;

	if (auto v = getenv_safe("ONNET_PUBLIC_HOST"))   c.public_host = v;
	c.gate_udp_port = getenv_u16("ONNET_GATE_UDP_PORT", c.gate_udp_port);
	c.nw_udp_port   = getenv_u16("ONNET_NW_UDP_PORT",   c.nw_udp_port);
	c.http_port     = getenv_u16("ONNET_HTTP_PORT",     c.http_port);

	if (auto v = getenv_safe("DATABASE_PATH"))       c.database_path = v;
	if (auto v = getenv_safe("MIGRATIONS_DIR"))      c.migrations_dir = v;
	if (auto v = getenv_safe("SEED_DIR"))            c.seed_dir = v;
	c.seed_dev_users = getenv_bool("SEED_DEV_USERS", c.seed_dev_users);
	if (auto v = getenv_safe("WEB_DIST_DIR"))        c.web_dist_dir = v;
	if (auto v = getenv_safe("TEMPLATES_DIR"))       c.templates_dir = v;
	if (auto v = getenv_safe("STATIC_DIR"))          c.static_dir = v;

	c.heartbeat_timeout_ms = getenv_u64("HEARTBEAT_TIMEOUT_MS", c.heartbeat_timeout_ms);
	c.tick_interval_ms     = getenv_u64("TICK_INTERVAL_MS",     c.tick_interval_ms);
	c.host_sweep_interval_ms = getenv_u64("HOST_SWEEP_INTERVAL_MS", c.host_sweep_interval_ms);
	c.host_stale_window_ms   = getenv_u64("HOST_STALE_WINDOW_MS",   c.host_stale_window_ms);

	if (auto v = getenv_safe("ADMIN_API_TOKEN"))     c.admin_api_token = v;

	// Expansion-publish pipeline (onnet admin.py / admin_internal.py). The
	// slug -> repo mapping is no longer configured here; it's read per-release
	// from the expansions.github_repo column (Terraform-managed catalogue).
	if (auto v = getenv_safe("EXPANSION_GITHUB_TOKEN"))  c.expansion_github_token = v;
	if (auto v = getenv_safe("EXPANSION_PUBLISH_TOKEN")) c.expansion_publish_token = v;

	if (auto v = getenv_safe("ONNET_CLIENT_REFLECT_IP")) c.client_reflect_ip = v;
	c.client_reflect_gate_port      = getenv_u16("ONNET_CLIENT_REFLECT_GATE_PORT",      c.client_reflect_gate_port);
	c.client_reflect_novaworld_port = getenv_u16("ONNET_CLIENT_REFLECT_NOVAWORLD_PORT", c.client_reflect_novaworld_port);

	return c;
}

} // namespace opennova::server
