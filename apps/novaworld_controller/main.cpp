// opennova-novaworld-controller: the controller (program.h). Elected among the
// controller processes on the database (controller.h), it runs while it leads
// the internal listener (the machines' agent routes, bound only by the leader,
// so the deployment's internal name reaches the one that renders the start
// files), the hosted servers' supervisor with its admin links, the console
// drain and the periodic jobs' scheduler. It calls no cloud and runs no job:
// what it decides it writes as rows and jobs, which the workers' pools run. A
// standby waits for the election's lock; a leadership lost ends the process
// (71), and its container starts it again. On ECS, with
// ONNET_INTERNAL_HTTP_BIND unset, the internal listener binds the task's
// private address (ecs_metadata.h), which the deployment's Cloud Map name
// (ONNET_INTERNAL_URL) points at.

#include "controller.h"
#include "ecs_metadata.h"
#include "parts.h"
#include "program.h"

#include "hosted/admin_link.h"
#include "hosted/console_queue.h"
#include "hosted/fleet_repository.h"
#include "hosted/hosted_repository.h"
#include "hosted/internal_listener.h"
#include "hosted/limits_hold.h"
#include "hosted/net_cidr.h"
#include "hosted/supervisor.h"
#include "jobs/scheduler.h"
#include "object_store.h"

#include <base/os_random/os_random.h>
#include <net/napi/session.h>
#include <net/novaworld/lobby_repository.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace opennova;
using namespace opennova::novaworld_server;

std::string configure(ServerConfig &config) {
	// The internal listener's bind from the ECS task, where the environment
	// names none: its private IPv4 at the internal port.
	ecs::Sources sources;
	if (const char *bind = std::getenv("ONNET_INTERNAL_HTTP_BIND"))
		sources.configured = bind;
	sources.metadata_uri = ecs::metadata_uri_from_env();
	sources.port = hosted::FleetConfig{}.internal_port;
	std::string error;
	config.internal_bind = ecs::internal_bind(sources, error);
	return error;
}

int run(const program::Boot &boot) {
	const ServerConfig &config = boot.config;

	const HostedSetup hosting = hosted_setup(config);
	const hosted::FleetConfig &fleet = hosting.fleet;
	// The machines' admin.cfg admits the address the controller's admin links
	// connect from, its internal listener's bind: on a cloud machine's
	// network, the controller's private address.
	if (hosting.provider == "ec2") {
		const auto address = hosted::parse_ipv4(fleet.internal_address);
		if (!address || !hosted::ipv4_cidr_private(hosted::Ipv4Cidr{ *address, ~uint32_t{ 0 } }))
			return program::fatal(
					"the ec2 driver needs the controller's internal listener on its private IPv4 "
					"(ONNET_INTERNAL_HTTP_BIND, or its ECS task's): the machines' admin.cfg admits "
					"that address");
	}
	// The periodic jobs it enqueues (the workers run them): the prunes, the
	// website's and the hosted stats' among them, billing's and the fleet's
	// (parts.h, which web's jobs page reads alike). The controller builds none
	// of billing (no transport, no Stripe call).
	jobs::Scheduler scheduler(boot.pool, periodic_jobs(config, hosting));

	// The admin links to the running hosted servers (hosted/admin_link.h): the
	// supervisor keeps them in step with its rows and publishes their states
	// (the routes read those rows), and the console drain runs the routes'
	// commands on them (hosted/console_queue.h). The supervisor, then the
	// internal listener whose agent routes ask it what each machine runs
	// (declared after it, so destroyed first).
	std::unique_ptr<hosted::AdminLinks> admin_links;
	std::unique_ptr<hosted::ConsoleDrain> console_drain;
	std::unique_ptr<hosted::Supervisor> supervisor;
	std::unique_ptr<hosted::InternalListener> internal_listener;
	if (hosting.on) {
		admin_links = std::make_unique<hosted::AdminLinks>(&boot.pool);
		console_drain = std::make_unique<hosted::ConsoleDrain>(boot.pool, *admin_links);
		hosted::SupervisorConfig hosted_config;
		hosted_config.cap = config.hosted_cap;
		// Where every server reaches NovaWorld, from the config alone: a machine
		// lists through the gate's public address, so the listing records the
		// machine's own (a local stack's machine container its address as the
		// gate sees it).
		hosted_config.master_host = config.public_host;
		hosted_config.master_gate_port = config.gate_udp_port;
		// Where the machines fetch each tree from: the store's URL of trees/<id>/,
		// a pure function of the store's config (the controller holds no store).
		hosted_config.tree_url = [store = hosting.store](int64_t tree_id) {
			return object_store_url(store, "trees/" + std::to_string(tree_id) + "/");
		};
		hosted_config.placement = fleet.placement;
		hosted_config.idle_ms = static_cast<uint64_t>(fleet.idle_minutes) * 60 * 1000;
		hosted_config.slot_cpus = fleet.slot_cpus;
		hosted_config.slot_memory_mb = fleet.slot_memory_mb;
		hosted_config.slot_pids = fleet.slot_pids;
		hosted_config.serve_image = fleet.ec2.serve_image;
		// A cloud container lists over the internet: a longer listing clock than
		// a local stack's machine's (its boot clock, 15 min, covers a fresh
		// machine's tree fetch and image pull on either driver).
		if (hosting.provider == "ec2")
			hosted_config.listing_timeout_ms = 5 * 60 * 1000;
		supervisor =
				std::make_unique<hosted::Supervisor>(boot.pool, hosting.profile, hosted_config);
		// A stop's ServerStopHosting, scoped to the service account that holds
		// the listing (the sysop punt's code, as the admin stop route's): a
		// command for the lobby that holds it, on the pass's connection.
		supervisor->set_stop_listing([](db::Database &db, uint32_t rid, int64_t account) {
			try {
				return lobbydb::queue_stop_hosting(db, rid, SERVER_MSG_CODE_NOVAWORLD_SYSOP_PUNT, 0,
							   0, account) == lobbydb::PushResult::Queued;
			} catch (const db::Error &e) {
				std::fprintf(
						stderr, "[hosted] WARN ServerStopHosting to RID %u: %s\n", rid, e.what());
				return false;
			}
		});
		supervisor->set_admin_links(admin_links.get());
		internal_listener = std::make_unique<hosted::InternalListener>(
				boot.pool, *supervisor, hosted::agent_route_config(fleet));
	}

	// The election (controller.h): while it leads, its parts run in order, each
	// pass asking that it still does; a leadership lost ends the process, and
	// its container starts it again as a standby.
	controller::ControllerConfig controller_config;
	{
		char tag[16];
		std::snprintf(tag, sizeof(tag), "%08x", os_random_u32());
		controller_config.holder = std::string("controller-") + tag;
	}
	controller_config.on_lost = [](const std::string &) {
		std::fflush(nullptr);
		std::_Exit(controller::kLeadershipLostExitCode);
	};
	controller::Controller leader(boot.db_options, controller_config);
	const auto leads = [&leader](db::Database &db) { return leader.still_leads(db); };
	if (supervisor) {
		// The machines' agent routes, bound only while this process leads and
		// before the supervisor's first pass launches a machine whose agent
		// calls them. A bind that fails ends the process (its container starts
		// it again).
		hosted::InternalListener *internal = internal_listener.get();
		const std::string bind_address = fleet.internal_address;
		const uint16_t bind_port = fleet.internal_port;
		leader.add_part({ "the internal listener",
				[internal, bind_address, bind_port] {
					if (internal->start(bind_address, bind_port))
						return;
					std::fprintf(stderr,
							"[controller] FATAL: the internal listener could not bind %s:%u "
							"(ONNET_INTERNAL_HTTP_BIND)\n",
							bind_address.c_str(), static_cast<unsigned>(bind_port));
					std::fflush(nullptr);
					std::_Exit(1);
				},
				[internal] { internal->stop(); } });
		supervisor->set_pass_guard(leads);
		hosted::Supervisor *sup = supervisor.get();
		hosted::AdminLinks *links = admin_links.get();
		hosted::ConsoleDrain *drain = console_drain.get();
		leader.add_part({ "the hosted servers' supervisor", [sup] { sup->start(); },
				[sup, links] {
					sup->stop();
					links->stop_all();
				} });
		leader.add_part(
				{ "the console drain", [drain] { drain->start(); }, [drain] { drain->stop(); } });
	}
	scheduler.set_guard(leads);
	leader.add_part({ "the scheduler", [&scheduler] { scheduler.start(); },
			[&scheduler] { scheduler.stop(); } });
	leader.start();

	// The controller's watchdog: its supervisor's passes and its scheduler's
	// ticks while it leads, the election's turns while it waits as a standby
	// (no stall).
	Watchdog watchdog;
	if (supervisor) {
		hosted::Supervisor *watched = supervisor.get();
		watchdog.watch("hosted servers' supervisor", [watched, &leader] {
			return leader.leading() ? watched->pass_turns() : leader.turns();
		});
	}
	watchdog.watch("job scheduler", [&scheduler, &leader] {
		return leader.leading() ? scheduler.turns() : leader.turns();
	});

	program::LoopHooks hooks;
	uint64_t launch_failures_reported = 0;
	hosted::Supervisor *counted = supervisor.get();
	hooks.heartbeat = [counted, &launch_failures_reported](
							  db::Database &db, Heartbeat &beat, int64_t window_s) {
		if (counted == nullptr)
			return;
		const uint64_t failures = counted->launch_failures();
		beat.launch_failures = static_cast<int64_t>(failures - launch_failures_reported);
		launch_failures_reported = failures;
		beat.hosted_live = hosted::count_live(db);
		beat.hosted_failed = hosted::count_failed_since(db, window_s);
		beat.machines_lost = hosted::count_machines(db, hosted::FleetMachineState::Lost);
		beat.limits_held = hosted::limits_hold::held(db) ? 1 : 0;
	};
	const int exit_code = program::run_main_loop("controller", boot, watchdog, hooks);

	// Its parts in reverse (the scheduler, so nothing is enqueued from here
	// on; the console drain; the supervisor's thread, so no pass runs, and the
	// admin links while their servers still run, each logged-in session ending
	// with QUIT; the internal listener), then its lock goes, and a standby
	// leads. The rows keep their states; the cloud machines and their
	// containers run on, and the next leader's reconciliation resumes them.
	std::printf("[shutdown] stopping the controller\n");
	leader.stop();
	return exit_code;
}

} // namespace

int main() {
	return opennova::novaworld_server::program::run_program({ "controller", configure, run });
}
