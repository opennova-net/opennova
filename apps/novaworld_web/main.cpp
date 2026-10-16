// opennova-novaworld-web: web (program.h). The HTTP listener on
// ONNET_HTTP_PORT (8080: the retail NW*.dll routes, the website's API, GET
// /livez and /healthz) over the database and the web state's Redis, billing's
// routes, the mail it queues, and the first site admin. Any number of web
// processes serve the same clients behind one load-balanced endpoint
// (ONNET_WEB_HOST): what a later request reads back lives in Redis or the
// database, and what the lobby or the controller must act on goes through
// their tables. No supervisor, no admin link, no lobby.

#include "auth.h"
#include "http_listener.h"
#include "loggable.h"
#include "parts.h"
#include "program.h"
#include "redis_client.h"

#include <net/novaworld/unknown_tracker.h>

#include <cstdio>
#include <exception>
#include <optional>
#include <string>

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
	return {};
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
	const HostedSetup hosting = hosted_setup(config);
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
	// A start's at_capacity and the list's `full` ask the fleet's planner.
	http.set_fleet_placement(hosting.fleet.placement);
	// The admin usage view's limits: each container's memory and CPUs.
	http.set_fleet_slots(hosting.fleet.slot_memory_mb, hosting.fleet.slot_cpus);
	// The admin jobs page's schedules: the controller's, read from the same
	// settings.
	http.set_job_schedules(periodic_jobs(config, hosting));
	if (!http.start(config))
		return program::fatal("HTTP listener start failed");

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
	// connection); then the billing's and the mail's scratch directories go
	// with their stacks.
	std::printf("[shutdown] stopping listeners\n");
	http.stop();
	return exit_code;
}

} // namespace

int main() {
	return program::run_program({ "web", configure, run });
}
