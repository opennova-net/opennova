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
	if (auto v = getenv_safe("WEB_DIST_DIR"))        c.web_dist_dir = v;
	if (auto v = getenv_safe("TEMPLATES_DIR"))       c.templates_dir = v;
	if (auto v = getenv_safe("STATIC_DIR"))          c.static_dir = v;

	c.heartbeat_timeout_ms = getenv_u64("HEARTBEAT_TIMEOUT_MS", c.heartbeat_timeout_ms);
	c.tick_interval_ms     = getenv_u64("TICK_INTERVAL_MS",     c.tick_interval_ms);
	c.host_sweep_interval_ms = getenv_u64("HOST_SWEEP_INTERVAL_MS", c.host_sweep_interval_ms);
	c.host_stale_window_ms   = getenv_u64("HOST_STALE_WINDOW_MS",   c.host_stale_window_ms);

	if (auto v = getenv_safe("ADMIN_API_TOKEN"))     c.admin_api_token = v;

	// Expansion-publish pipeline (onnet admin.py / admin_internal.py).
	if (auto v = getenv_safe("EXPANSION_GITHUB_TOKEN"))  c.expansion_github_token = v;
	if (auto v = getenv_safe("EXPANSION_PUBLISH_TOKEN")) c.expansion_publish_token = v;

	// Default slug -> repo mapping (onnet admin.py:19-23).
	c.expansion_repositories = {
		{"revx02", "opennova-net/revx02"},
		{"onjo01", "opennova-net/onjo01"},
		{"ondx01", "opennova-net/ondx01"},
	};
	// Optional override: "slug=owner/repo,slug2=owner/repo2".
	if (auto v = getenv_safe("EXPANSION_REPOSITORIES")) {
		c.expansion_repositories.clear();
		std::string spec = v;
		size_t pos = 0;
		while (pos < spec.size()) {
			size_t comma = spec.find(',', pos);
			std::string pair = spec.substr(pos, comma - pos);
			size_t eq = pair.find('=');
			if (eq != std::string::npos) {
				std::string slug = pair.substr(0, eq);
				std::string repo = pair.substr(eq + 1);
				if (!slug.empty() && !repo.empty())
					c.expansion_repositories[slug] = repo;
			}
			if (comma == std::string::npos) break;
			pos = comma + 1;
		}
	}

	if (auto v = getenv_safe("ONNET_CLIENT_REFLECT_IP")) c.client_reflect_ip = v;
	c.client_reflect_gate_port      = getenv_u16("ONNET_CLIENT_REFLECT_GATE_PORT",      c.client_reflect_gate_port);
	c.client_reflect_novaworld_port = getenv_u16("ONNET_CLIENT_REFLECT_NOVAWORLD_PORT", c.client_reflect_novaworld_port);

	return c;
}

} // namespace opennova::server
