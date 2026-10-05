#include "server_config.h"

#include <base/io/strutil.h>

#include <cstdlib>
#include <string>

namespace opennova::novaworld_server {

namespace {

const char *getenv_safe(const char *name) {
	const char *v = std::getenv(name);
	return (v && *v) ? v : nullptr;
}

// A value that is no number (or out of range) keeps the fallback; the parse is
// std::stoi / stoull's without the throw (strutil, ADR 0049 d5).
uint16_t getenv_u16(const char *name, uint16_t fallback) {
	const char *v = getenv_safe(name);
	if (!v) return fallback;
	const auto n = strutil::parse_int(v);
	return n ? static_cast<uint16_t>(*n) : fallback;
}

uint64_t getenv_u64(const char *name, uint64_t fallback) {
	const char *v = getenv_safe(name);
	if (!v) return fallback;
	return strutil::parse_ullong(v).value_or(fallback);
}

int getenv_int(const char *name, int fallback) {
	const char *v = getenv_safe(name);
	if (!v) return fallback;
	return strutil::parse_int(v).value_or(fallback);
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

	// heartbeat_timeout_ms has no env knob: it is the advertised cs[0].
	c.tick_interval_ms     = getenv_u64("TICK_INTERVAL_MS",     c.tick_interval_ms);
	c.host_sweep_interval_ms = getenv_u64("HOST_SWEEP_INTERVAL_MS", c.host_sweep_interval_ms);
	c.host_stale_window_ms   = getenv_u64("HOST_STALE_WINDOW_MS",   c.host_stale_window_ms);

	if (auto v = getenv_safe("ADMIN_API_TOKEN"))     c.admin_api_token = v;

	if (auto v = getenv_safe("ONNET_MET_IP"))    c.met_ip = v;
	c.met_port = getenv_u16("ONNET_MET_PORT", c.met_port);
	if (auto v = getenv_safe("ONNET_MET_LABEL")) c.met_label = v;
	c.met_ping = getenv_int("ONNET_MET_PING", c.met_ping);
	c.met_ext  = getenv_int("ONNET_MET_EXT",  c.met_ext);

	if (auto v = getenv_safe("ONNET_GLSVSS_REQUEST")) c.glsvss_request = v;
	c.glsvss_rims  = getenv_int("ONNET_GLSVSS_RIMS",  c.glsvss_rims);
	c.glsvss_agrms = getenv_int("ONNET_GLSVSS_AGRMS", c.glsvss_agrms);
	if (auto v = getenv_safe("ONNET_GLSVSS_RESULTS")) c.glsvss_results = v;

	if (auto v = getenv_safe("ONNET_CLIENT_REFLECT_IP")) c.client_reflect_ip = v;
	c.client_reflect_gate_port      = getenv_u16("ONNET_CLIENT_REFLECT_GATE_PORT",      c.client_reflect_gate_port);
	c.client_reflect_novaworld_port = getenv_u16("ONNET_CLIENT_REFLECT_NOVAWORLD_PORT", c.client_reflect_novaworld_port);

	return c;
}

} // namespace opennova::novaworld_server
