// opennova-novaworld-web: web (program.h). The HTTP listener on
// ONNET_HTTP_PORT (8080: the retail NW*.dll routes, the website's API, GET
// /livez and /healthz) over the database and the web state's Redis, billing's
// routes, the mail it queues, and the first site admin. Any number of web
// processes serve the same clients behind one load-balanced endpoint
// (ONNET_WEB_HOST): what a later request reads back lives in Redis or the
// database, and what the lobby or the fleet must act on goes through
// their tables. No supervisor, no admin link, no lobby.

#include "auth.h"
#include "http_listener.h"
#include "infra/infra_status.h"
#include "infra/log_store.h"
#include "infra/metrics_store.h"
#include "loggable.h"
#include "object_store.h"
#include "parts.h"
#include "program.h"
#include "redis_client.h"
#include "updater/update_listener.h"

#include <base/io/os_path.h>
#include <net/novaworld/unknown_tracker.h>

#include <cstdio>
#include <exception>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace opennova;
using namespace opennova::novaworld_server;

// ONNET_BOOTSTRAP_ADMIN names an existing account, promoted only while no
// account is an admin (bootstrap_admin, under the migration lock, so two web
// processes booting at once promote one).
void bootstrap_first_admin(const ServerConfig &config, db::Database &db) {
	if (config.bootstrap_admin.empty())
		return;
	const std::string name = loggable(config.bootstrap_admin);
	try {
		const auto result = bootstrap_admin(db, config.bootstrap_admin);
		switch (result.outcome) {
			case BootstrapAdminResult::Outcome::Promoted:
				std::printf("[boot] bootstrap admin: '%s' is now the first admin; remove "
							"ONNET_BOOTSTRAP_ADMIN\n",
						name.c_str());
				break;
			case BootstrapAdminResult::Outcome::AdminExists:
				std::fprintf(stderr,
						"[boot] WARN bootstrap admin: skipped '%s', an admin exists ('%s'); "
						"remove ONNET_BOOTSTRAP_ADMIN\n",
						name.c_str(), loggable(result.existing_admin).c_str());
				break;
			case BootstrapAdminResult::Outcome::NoSuchAccount:
				std::fprintf(stderr,
						"[boot] WARN bootstrap admin: skipped, no account named '%s'\n",
						name.c_str());
				break;
		}
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[boot] WARN bootstrap admin: %s\n", e.what());
	}
}

std::string configure(ServerConfig &config) {
	if (config.web_host.empty())
		return "web needs ONNET_WEB_HOST, the web endpoint its pages name";
	// The update listener's ports (ONNET_UPDATE_PORTS; updater/update_listener.h): a
	// list that does not read is this boot's FATAL.
	std::vector<uint16_t> ports;
	if (!update_service::parse_update_ports(config.update_ports, ports))
		return "ONNET_UPDATE_PORTS is no comma-separated list of ports 1..65535";
	return std::string();
}

int run(const program::Boot &boot) {
	ServerConfig config = boot.config;

	// --- Redis: the HTTP listener's shared state (redis_client.h) ----------
	// Required: the retail sessions, the pins and the rate limiters live there
	// alone, so a web process that cannot reach it does not boot. Declared
	// before the HTTP listener, which it outlives.
	add_log_secret(config.redis_password);
	std::printf("  redis=%s\n", db::masked_url(config.redis_url).c_str());
	RedisOptions redis_options;
	redis_options.url = config.redis_url;
	redis_options.password = config.redis_password;
	RedisPool redis_pool(redis_options);
	try {
		const RedisReply pong = redis_pool.command({ "PING" });
		std::printf("[boot] redis: %s\n", pong.string.c_str());
	} catch (const RedisError &e) {
		std::fprintf(stderr, "[boot] FATAL: redis unreachable: %s\n", e.what());
		return 1;
	}

	// The boot's database work on a lease of its own, returned before the
	// listener starts (its start leases on this thread).
	const HostedPolicy hosting = hosted_policy(config);
	config.hosted_provider = hosting.provider;
	std::optional<BillingStack> billing_stack;
	try {
		auto lease = boot.pool.acquire();
		bootstrap_first_admin(config, *lease);
		billing_stack.emplace(make_billing(billing::billing_config_from_env(), boot.pool, *lease));
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[boot] FATAL: web's boot: %s\n", e.what());
		return 1;
	}
	// Whether the routes queue the welcome and reset mails (mail.send jobs, a
	// worker's to send): the worker decides alike from the same settings.
	const MailStack mail = make_mail(config);
	// The object store the data set routes upload into and read (the admin's
	// data sets page). Declared before the listener, which it outlives.
	ObjectStoreStack store;
	if (hosting.store_ok) {
		std::string error;
		store = make_object_store(hosting, config, error);
		if (!store.store)
			return program::fatal("the object store: " + error);
		std::printf("[boot] data sets: routes over the %s store\n", store.store->kind());
	}

	// The admin's infrastructure view's AWS reads (infra/infra_status.h): the
	// deployment's ECS services, balancer, database and cache through web's task
	// role; none on the local stack, which the view says.
	const infra::InfraStack infra_stack = infra::make_infra();
	// The admin's log view's store (infra/log_store.h, ONNET_LOG_STORE): the
	// deployment's CloudWatch groups, through the same task role and curl, or
	// the local stack's directory.
	std::string log_store_none;
	const std::unique_ptr<infra::LogStore> log_store = infra::make_log_store(
			*infra_stack.runner, infra_stack.scratch.path().u8string(), log_store_none);
	if (log_store)
		std::printf("[boot] logs: read from the %s store\n", log_store->source());
	else
		std::printf("[boot] logs: none read (%s)\n", log_store_none.c_str());

	// The usage view's AWS metrics (infra/metrics_store.h): the database's, the
	// cache's and the balancer's through the same task role; none on the local
	// stack, which the view says.
	infra::MetricsStore metrics_store(*infra_stack.runner,
			infra::metrics_config_from_env(infra_stack.scratch.path().u8string()));

	// The 404s the routes record, flushed by the main loop into
	// unknown_messages (GET /api/unknowns reads them there).
	UnknownTracker unknown_tracker;
	// Web watches no loop of its own (Crow's threads serve each request
	// alone); GET /livez and /healthz read the main loop's last check.
	Watchdog watchdog;

	HttpListener http(boot.pool, redis_pool);
	http.set_unknown_tracker(&unknown_tracker);
	http.set_mail_enabled(mail.on);
	http.set_watchdog(&watchdog);
	http.set_billing(billing_stack->billing.get());
	http.set_infra(infra_stack.status.get());
	http.set_log_store(log_store.get(), log_store_none);
	http.set_metrics_store(&metrics_store);
	// A start's at_capacity and the list's `full` ask the fleet's planner.
	http.set_fleet_placement(hosting.fleet.placement);
	// The images the release pins: the launch templates' (a fleet roll's
	// preview) and hostd's, each machine's held against them.
	http.set_fleet_release({ hosting.fleet.image_id, hosting.fleet.hostd_image });
	// The static addresses: on or off, and the zone they are named in.
	http.set_addresses(hosting.addresses_on, hosting.fleet.address_zone);
	// The admin usage view's limits: each container's memory and CPUs.
	http.set_fleet_slots(hosting.fleet.slot_memory_mb, hosting.fleet.slot_cpus);
	// The admin jobs page's schedules: every program's (the worker's and the
	// data program's), read from the same settings.
	http.set_job_schedules(periodic_jobs(config, hosting));
	// The game data routes (data_sets/) work on the deployment's object store
	// (ONNET_OBJECT_STORE_URL, ADR 0064), built above: none without one, and those
	// routes answer 503 store_unavailable.
	ObjectStore *object_store = store.store.get();
	http.set_object_store(object_store);
	if (!http.start(config))
		return program::fatal("HTTP listener start failed");

	// The update listener (updater/update_listener.h): UPDATE.EXE's gateway and
	// update server over the published releases, on ONNET_UPDATE_PORTS, when
	// ONNET_UPDATE_HOST names this deployment's update host; its packages cached
	// under ONNET_UPDATE_CACHE_DIR, or a scratch directory of its own (fetched again
	// after a restart). It serves the packages from the object store, so without one
	// (hosting off) it does not start. Watched by the stall watchdog like the loops.
	std::string update_error;
	update_service::UpdateListenerConfig update_config;
	update_config.update_host = config.update_host;
	update_config.cache_dir = config.update_cache_dir;
	update_service::parse_update_ports(config.update_ports, update_config.ports); // checked by configure
	ScratchDir update_cache;
	std::optional<update_service::UpdateListener> update_listener;
	if (!update_config.update_host.empty()) {
		if (object_store == nullptr) {
			std::fprintf(stderr,
					"[boot] WARN update listener not started: no object store (hosting off)\n");
		} else {
			if (update_config.cache_dir.empty()) {
				update_cache = ScratchDir::make("update-cache", update_error);
				if (update_cache.empty())
					return program::fatal("update cache: " + update_error);
				update_config.cache_dir = io::utf8_path(update_cache.path());
			}
			update_listener.emplace(boot.pool, *object_store, update_config);
			if (!update_listener->start())
				return program::fatal("update listener start failed");
			watchdog.watch(
					"update listener", [&update_listener] { return update_listener->turns(); });
		}
	} else {
		std::printf("[boot] update listener off (ONNET_UPDATE_HOST unset)\n");
	}

	billing::Billing *billing = billing_stack->billing.get();
	uint64_t webhook_rejects_reported = 0;
	program::LoopHooks hooks;
	// GET /livez is the HTTP listener's own (on ONNET_HTTP_PORT).
	hooks.health_listener = false;
	hooks.tick = [&unknown_tracker](db::Database &db, uint64_t) {
		try {
			unknown_tracker.flush(db);
		} catch (const db::Error &e) {
			std::fprintf(stderr, "[tick] WARN unknown_tracker flush: %s\n", e.what());
		}
	};
	hooks.heartbeat = [billing, &webhook_rejects_reported](
							  db::Database &, Heartbeat &beat, int64_t) {
		if (billing == nullptr)
			return;
		const uint64_t rejects = billing->webhook_rejects();
		beat.webhook_rejects = static_cast<int64_t>(rejects - webhook_rejects_reported);
		webhook_rejects_reported = rejects;
	};
	const int exit_code = program::run_main_loop("web", boot, watchdog, hooks);

	// Crow's threads joined (no request still holds a lease or a Redis
	// connection); then the billing's, the mail's and the infrastructure
	// view's scratch directories go with their stacks.
	std::printf("[shutdown] stopping listeners\n");
	if (update_listener)
		update_listener->stop();
	http.stop();
	return exit_code;
}

} // namespace

int main() {
	return program::run_program({ "web", configure, run });
}
