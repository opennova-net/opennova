#include "server_config.h"
#include "core/loggable.h"
#include "core/text_util.h"

#include <base/io/strutil.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

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

// A comma-separated list, each entry trimmed, empty entries dropped.
} // namespace

ServerConfig ServerConfig::from_env() {
	ServerConfig c;

	if (auto v = getenv_safe("ONNET_PUBLIC_HOST")) {
		c.public_host = v;
		c.public_host_set = true;
	}
	if (auto v = getenv_safe("ONNET_WEB_HOST"))
		c.web_host = strutil::trim(v);
	if (auto v = getenv_safe("ONNET_LOBBY_ENDPOINT"))
		c.lobby_endpoint = strutil::trim(v);
	c.gate_udp_port = getenv_u16("ONNET_GATE_UDP_PORT", c.gate_udp_port);
	c.nw_udp_port   = getenv_u16("ONNET_NW_UDP_PORT",   c.nw_udp_port);
	c.http_port     = getenv_u16("ONNET_HTTP_PORT",     c.http_port);

	if (auto v = getenv_safe("DATABASE_URL"))        c.database_url = v;
	if (auto v = getenv_safe("DATABASE_PASSWORD"))   c.database_password = v;
	c.database_pool_max = static_cast<size_t>(
			std::max<uint64_t>(1, getenv_u64("DATABASE_POOL_MAX", c.database_pool_max)));
	if (auto v = getenv_safe("ONNET_REDIS_URL"))      c.redis_url = v;
	if (auto v = getenv_safe("ONNET_REDIS_PASSWORD")) c.redis_password = v;
	if (auto v = getenv_safe("MIGRATIONS_DIR"))      c.migrations_dir = v;
	if (auto v = getenv_safe("SEED_DIR"))            c.seed_dir = v;
	c.seed_dev_users = getenv_bool("SEED_DEV_USERS", c.seed_dev_users);
	if (auto v = getenv_safe("WEB_DIST_DIR"))        c.web_dist_dir = v;
	if (auto v = getenv_safe("TEMPLATES_DIR"))       c.templates_dir = v;
	if (auto v = getenv_safe("STATIC_DIR"))          c.static_dir = v;

	// heartbeat_timeout_ms has no env knob: it is the advertised cs[0].
	c.tick_interval_ms     = getenv_u64("TICK_INTERVAL_MS",     c.tick_interval_ms);
	c.health_port = getenv_u16("ONNET_HEALTH_PORT", c.health_port);
	c.host_sweep_interval_ms = getenv_u64("HOST_SWEEP_INTERVAL_MS", c.host_sweep_interval_ms);
	c.host_stale_window_ms   = getenv_u64("HOST_STALE_WINDOW_MS",   c.host_stale_window_ms);
	c.worker_threads = std::clamp(getenv_int("ONNET_WORKER_THREADS", c.worker_threads), 1, 64);

	if (auto v = getenv_safe("ADMIN_API_TOKEN"))     c.admin_api_token = v;
	c.cookie_secure = getenv_bool("ONNET_COOKIE_SECURE", c.cookie_secure);
	if (auto v = getenv_safe("ONNET_TRUSTED_PROXIES")) c.trusted_proxies = split_list(v);
	c.api_proxy_only = getenv_bool("ONNET_API_PROXY_ONLY", c.api_proxy_only);
	if (auto v = getenv_safe("ONNET_RETAIL_PROXY")) c.retail_proxy = v;
	if (auto v = getenv_safe("ONNET_BOOTSTRAP_ADMIN")) c.bootstrap_admin = v;
	c.require_host_key = getenv_bool("ONNET_REQUIRE_HOST_KEY", c.require_host_key);
	c.cd_key_auto_mint = getenv_bool("ONNET_CD_KEY_AUTO_MINT", c.cd_key_auto_mint);
	c.embedded_jo_host = getenv_bool("ONNET_EMBEDDED_JO_HOST", c.embedded_jo_host);
	if (auto v = getenv_safe("ONNET_SITE_URL")) {
		c.site_url = strutil::trim(v);
		while (!c.site_url.empty() && c.site_url.back() == '/') c.site_url.pop_back();
	}
	if (auto v = getenv_safe("ONNET_MAIL_FROM")) c.mail_from = strutil::trim(v);
	if (auto v = getenv_safe("ONNET_MAIL_REGION")) c.mail_region = strutil::trim(v);
	if (auto v = getenv_safe("ONNET_HOSTED_AWS_CLI"))
		c.aws_cli = v;

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

	if (auto v = getenv_safe("ONNET_HOSTED_PROVIDER")) c.hosted_provider = v;
	c.hosted_cap = getenv_int("ONNET_HOSTED_CAP", c.hosted_cap);
	if (auto v = getenv_safe("ONNET_OBJECT_STORE_URL")) c.object_store_url = strutil::trim(v);
	if (auto v = getenv_safe("ONNET_OBJECT_STORE_REGION")) c.object_store_region = strutil::trim(v);
	if (auto v = getenv_safe("ONNET_UPDATE_HOST")) c.update_host = strutil::trim(v);
	if (auto v = getenv_safe("ONNET_UPDATE_PORTS")) c.update_ports = strutil::trim(v);
	if (auto v = getenv_safe("ONNET_UPDATE_CACHE_DIR")) c.update_cache_dir = strutil::trim(v);

	return c;
}

std::optional<ObjectStoreConfig> object_store_of(const ServerConfig &config) {
	if (config.object_store_url.empty())
		return std::nullopt;
	ObjectStoreConfig store;
	std::string error;
	if (!parse_object_store_url(config.object_store_url, config.object_store_region, store, error)) {
		std::fprintf(stderr, "[boot] WARN the object store: ONNET_OBJECT_STORE_URL %s\n",
				loggable(error, 300).c_str());
		return std::nullopt;
	}
	return store;
}

} // namespace opennova::novaworld_server
