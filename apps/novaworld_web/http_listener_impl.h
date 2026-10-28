#pragma once

// The HTTP listener's own state (http_listener.h's Impl): the Crow app, the start handshake, the
// route families it registers and the games catalogue. Private to the listener's translation
// units: http_listener.cpp (start, stop, the admin and public API, the static routes),
// retail_login_routes.cpp (the retail login chain) and retail_host_join_routes.cpp (the retail
// host and join flow).

#include "admin_user_routes.h"
#include "billing/billing_routes.h"
#include "cd_key_routes.h"
#include "connection_routes.h"
#include "host_routes.h"
#include "hosted/address_routes.h"
#include "hosted/console_routes.h"
#include "hosted/fleet_routes.h"
#include "hosted/hosted_routes.h"
#include "hosted/stats_routes.h"
#include "http_listener.h"
#include "infra/attention_routes.h"
#include "infra/cost_routes.h"
#include "infra/infra_routes.h"
#include "infra/log_routes.h"
#include "infra/usage_routes.h"
#include "jobs/job_routes.h"
#include "lobby_routes.h"
#include "password_reset_routes.h"
#include "products/product_routes.h"
#include "products/project_routes.h"
#include "products/release_routes.h"
#include "program/catalog_repository.h"
#include "redis_client.h"
#include "settings_routes.h"
#include "web_access.h"

#include <net/novaworld/db/database.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>

namespace opennova::novaworld_server {

struct HttpListener::Impl {
	Impl(db::ConnectionPool &pool, RedisPool &redis) :
			access(app, pool, redis),
			hosts(access, pool, redis),
			connections(access, pool),
			hosted_servers(access, pool, redis),
			addresses(access, pool),
			fleet(access, pool),
			stats(access, pool, redis),
			console(access, pool, redis),
			lobbies(pool),
			cd_keys(access, pool),
			users(access, pool),
			settings(access, pool),
			billing(access, pool, redis),
			resets(access, pool, redis),
			jobs(access, pool),
			infra(access, pool),
			logs(access, pool, redis),
			usage(access, pool, redis),
			attention(access, pool, redis),
			costs(access, redis),
			products(access, pool),
			releases(access, pool),
			project_routes(access, pool, redis) {}

	WebApp app;
	// start()'s handshake with the Crow thread: Pending until Crow serves
	// (Serving, with the port it bound) or until run() returns (Exited).
	enum class Run { Pending, Serving, Exited };
	std::mutex run_mu;
	std::condition_variable run_cv;
	Run run = Run::Pending;
	uint16_t port = 0;
	// How often Crow runs the tick whose first call tells start() it serves.
	static constexpr std::chrono::milliseconds kServingTick{50};
	// Who each /api request comes from (web_access.h); declared after `app`,
	// which it registers its routes on.
	WebAccess access;
	// The listed servers' routes (host_routes.h), over `access`.
	HostRoutes hosts;
	// The admin's NovaWorld sessions (connection_routes.h).
	ConnectionRoutes connections;
	// The hosted servers' routes (hosted/hosted_routes.h), over `access`.
	hosted::HostedRoutes hosted_servers;
	// The static addresses' routes (hosted/address_routes.h).
	hosted::AddressRoutes addresses;
	// The admin's fleet view and the machines' routes (hosted/fleet_routes.h).
	hosted::FleetRoutes fleet;
	// The hosted servers' stats (hosted/stats_routes.h).
	hosted::StatsRoutes stats;
	// Their console routes over the admin links (hosted/console_routes.h).
	hosted::ConsoleRoutes console;
	// The lobby browser's listings and a listing's page (lobby_routes.h).
	LobbyRoutes lobbies;
	// The CD keys' admin routes and the retail login's DFX2 activation (cd_key_routes.h).
	CdKeyRoutes cd_keys;
	// The admin's user routes (admin_user_routes.h).
	AdminUserRoutes users;
	// The admin's live settings (settings_routes.h).
	SettingsRoutes settings;
	// The support tiers' routes (billing/billing_routes.h).
	billing::BillingRoutes billing;
	// The password reset routes (password_reset_routes.h).
	PasswordResetRoutes resets;
	// The admin's background jobs routes (jobs/job_routes.h).
	jobs::JobRoutes jobs;
	// The admin's infrastructure view (infra/infra_routes.h).
	infra::InfraRoutes infra;
	// Every log on the site (infra/log_routes.h).
	infra::LogRoutes logs;
	// What everything uses, and the programs (infra/usage_routes.h).
	infra::UsageRoutes usage;
	// What needs an admin (infra/attention_routes.h).
	infra::AttentionRoutes attention;
	// The month's AWS costs (infra/cost_routes.h).
	infra::CostRoutes costs;
	// The products', the releases' and the project routes (products/).
	hosted::ProductRoutes products;
	hosted::ReleaseRoutes releases;
	projects::ProjectRoutes project_routes;
	// The games catalogue (HttpListener::games_now): loaded in start() before
	// the routes register, and again by the first request that finds it
	// games_refresh old (HttpListener::kGamesRefresh, a minute), so an
	// account's own game's lobby, which its first publish adds
	// (products/product_repository.h publish_release), answers with no
	// restart. Each load is immutable; a request holds the one it read.
	// games_mu guards the pointer and its time, games_load_mu the one load at
	// a time.
	std::chrono::milliseconds games_refresh{ HttpListener::kGamesRefresh };
	std::mutex games_mu;
	std::mutex games_load_mu;
	std::shared_ptr<const catalog::GameCatalog> games;
	std::chrono::steady_clock::time_point games_read{};
};

} // namespace opennova::novaworld_server
