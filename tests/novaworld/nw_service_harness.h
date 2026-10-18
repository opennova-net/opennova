#pragma once

// One NovaWorld service in-process for the end-to-end tests that need all of
// it (nw_host_ownership_e2e, nw_owner_admin_e2e): the Crow HttpListener (the
// retail NW*.dll pages from apps/novaworld_web/templates and the /api
// routes), the NwUdpListener and the GateListener over one pooled database of
// the service's own (common/test_database.h: backend/migrations plus the games
// seed) with one account and a Redis namespace of its own
// (common/test_redis.h), on loopback ports the OS picks. Built only with
// BUILD_NOVAWORLD_HTTP; main() calls test_db::init and test_redis::init first.

#include "auth.h"
#include "gate_listener.h"
#include "http_listener.h"
#include "nw_udp_listener.h"
#include "program/server_config.h"

#include <net/novaworld/connection/manager.h>
#include <net/novaworld/db/database.h>
#include <net/novaworld/host_repository.h>
#include <net_sockets.h>

#include "common/test_database.h"
#include "common/test_redis.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace nw_service_test {

// The account Service::start makes.
inline constexpr const char *kOwner = "owner";
inline constexpr const char *kOwnerPassword = "pw-owner";

// Polls `done` every 20 ms, `tries` times at most; its last answer.
inline bool wait_for(const std::function<bool()> &done, int tries = 100) {
	for (int i = 0; i < tries && !done(); ++i)
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	return done();
}

// The service's three listeners in one process, as the web, lobby and gate
// programs run them over one database: the HTTP listener first (its pushes and sessions
// view through the lobby's tables), then the NW UDP listener, then the gate.
// The config names what each advertises as a deployment's does (ONNET_WEB_HOST
// and ONNET_HTTP_PORT, ONNET_LOBBY_ENDPOINT): here the ports the OS picked (the
// lobby's picked up front, since its endpoint is the config's, never the port
// it bound).
struct Service {
	test_db::TestDatabase database{ test_db::Options{ /*migrate=*/true, /*seed_games=*/true } };
	opennova::db::ConnectionPool pool;
	opennova::ConnectionManager manager;
	test_redis::TestRedis redis;
	opennova::novaworld_server::HttpListener http;
	opennova::novaworld_server::NwUdpListener nwudp;
	opennova::novaworld_server::GateListener gate;
	int64_t owner_id = 0;

	Service() : pool(database.options()), http(pool, redis.pool()), nwudp(manager) {}

	// `tweak`, when set, edits the config the listeners start with (the hosted
	// servers' settings, say) after the harness has set its own.
	bool start(const std::filesystem::path &source_dir, const std::filesystem::path &scratch,
			bool require_host_key,
			const std::function<void(opennova::novaworld_server::ServerConfig &)> &tweak = {}) {
		{
			auto boot = pool.acquire();
			opennova::novaworld_server::CreateUserParams p;
			p.username = kOwner;
			p.password = kOwnerPassword;
			p.pcid = "0e2e0001";
			p.nwh = "1";
			p.nwhandle = "OwnerHandle";
			const auto created = opennova::novaworld_server::create_user(*boot, p);
			if (!created.ok)
				return false;
			owner_id = created.id;
		}
		manager.on_lost([this](const opennova::Connection &c, opennova::DropReason r) {
			nwudp.erase_lobby_state(c.addr, opennova::drop_reason_name(r));
		});
		opennova::novaworld_server::ServerConfig config;
		config.public_host = "127.0.0.1";
		config.web_host = "127.0.0.1";
		config.http_port = 0;
		// The lobby's port, free a moment ago (bound and let go), which its
		// instance announces and the gate hands out.
		{
			uint16_t nw_port = 0;
			opennova::net::ScopedSocket probe(opennova::net::udp_bind(0, &nw_port));
			config.nw_udp_port = nw_port;
		}
		config.lobby_endpoint = "127.0.0.1:" + std::to_string(config.nw_udp_port);
		config.gate_udp_port = 0;
		config.templates_dir = source_dir / "apps" / "novaworld_web" / "templates";
		config.web_dist_dir = scratch / "web_dist";
		config.static_dir = scratch / "static";
		config.require_host_key = require_host_key;
		if (tweak) tweak(config);
		nwudp.set_db_pool(&pool);
		gate.set_db_pool(&pool);
		if (!http.start(config))
			return false;
		config.http_port = http.bound_port();
		if (!nwudp.start(config))
			return false;
		return gate.start(config);
	}

	void stop() {
		http.stop();
		gate.stop();
		nwudp.stop();
	}

	// The hosting connection named `name`, from the listener's memory.
	std::optional<opennova::LobbyState> lobby(const std::string &name) const {
		for (const auto &h : nwudp.snapshot_hosted()) {
			if (h.lobby.hosting && h.lobby.server_name == name)
				return h.lobby;
		}
		return std::nullopt;
	}

	// The browser's row (active_hosts) of the connection named `name`.
	std::optional<opennova::hostdb::HostRow> row(const std::string &name) {
		for (const auto &h : opennova::hostdb::list_hosts(*pool.acquire())) {
			if (h.server_name == name)
				return h;
		}
		return std::nullopt;
	}

	// The row named `name` once it names `owner` (nullopt: no owner), within a
	// couple of seconds.
	bool row_owned_by(const std::string &name, std::optional<int64_t> owner) {
		return wait_for([&] {
			const auto r = row(name);
			return r && r->host_user_id == owner;
		});
	}

	// The key's host_keys row: {user_id, claimed_rid}; nullopt when none.
	std::optional<std::pair<int64_t, std::optional<int64_t>>> key_row(const std::string &key) {
		const auto rows = pool.acquire()->query(
				"SELECT user_id, claimed_rid FROM host_keys WHERE key_hash = $1;",
				{ opennova::db::BindValue(opennova::hostdb::host_key_hash(key)) });
		if (rows.empty())
			return std::nullopt;
		return std::make_pair(rows[0].as_int(0).value_or(0), rows[0].as_int(1));
	}
};

} // namespace nw_service_test
