// opennova-novaworld-fleet: the fleet program (program/program.h). Elected among the
// fleet processes on the database (election.h), it runs while it leads
// the internal listener (the machines' agent routes, bound only by the leader,
// so the deployment's internal name reaches the one that renders the start
// files), the hosted servers' supervisor with its admin links and the console
// drain. It calls no cloud, runs no job and schedules none: what it decides it
// writes as rows and jobs, which the workers' pools run. A
// standby waits for the election's lock; a leadership lost ends the process
// (71), and its container starts it again. On ECS, with
// ONNET_INTERNAL_HTTP_BIND unset, the internal listener binds the task's
// private address (core/ecs_metadata.h), which the deployment's Cloud Map name
// (ONNET_INTERNAL_URL) points at.

#include "core/ecs_metadata.h"
#include "core/program_log.h"
#include "election.h"
#include "parts.h"
#include "program/program.h"

#include "core/object_store.h"
#include "hosted/admin_link.h"
#include "hosted/console_drain.h"
#include "hosted/fleet_health.h"
#include "hosted/fleet_repository.h"
#include "hosted/hosted_repository.h"
#include "hosted/internal_listener.h"
#include "hosted/limits_hold.h"
#include "hosted/net_cidr.h"
#include "hosted/supervisor.h"

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

	const HostedPolicy hosting = hosted_policy(config);
	const hosted::FleetConfig &fleet = hosting.fleet;
	// The machines' admin.cfg admits the address the fleet's admin links
	// connect from, its internal listener's bind: on a cloud machine's
	// network, the fleet task's private address.
	if (hosting.provider == "ec2") {
		const auto address = hosted::parse_ipv4(fleet.internal_address);
		if (!address || !hosted::ipv4_cidr_private(hosted::Ipv4Cidr{ *address, ~uint32_t{ 0 } }))
			return program::fatal(
					"the ec2 driver needs the fleet's internal listener on its private IPv4 "
					"(ONNET_INTERNAL_HTTP_BIND, or its ECS task's): the machines' admin.cfg admits "
					"that address");
	}
	// The admin links to the running hosted servers (hosted/admin_link.h): the
	// supervisor keeps them in step with its rows and publishes their states
	// (the routes read those rows), and the console drain runs the routes'
	// commands on them (hosted/console_drain.h). The supervisor, then the
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
		// a pure function of the store's config (the fleet holds no store).
		hosted_config.tree_url = [store = hosting.store](int64_t tree_id) {
			return object_store_url(store, "trees/" + std::to_string(tree_id) + "/");
		};
		hosted_config.placement = fleet.placement;
		hosted_config.idle_ms = static_cast<uint64_t>(fleet.idle_minutes) * 60 * 1000;
		hosted_config.slot_cpus = fleet.slot_cpus;
		hosted_config.slot_memory_mb = fleet.slot_memory_mb;
		hosted_config.slot_pids = fleet.slot_pids;
		hosted_config.serve_image = fleet.serve_image;
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

	// The election (election.h): while it leads, its parts run in order, each
	// pass asking that it still does; a leadership lost ends the process, and
	// its container starts it again as a standby.
	fleet::ElectionConfig election_config;
	{
		char tag[16];
		std::snprintf(tag, sizeof(tag), "%08x", os_random_u32());
		election_config.holder = std::string("fleet-") + tag;
	}
	election_config.on_lost = [](const std::string &) {
		std::fflush(nullptr);
		program::drain_log_tee(500);
		std::_Exit(fleet::kLeadershipLostExitCode);
	};
	fleet::Election leader(boot.db_options, election_config);
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
							"[fleet] FATAL: the internal listener could not bind %s:%u "
							"(ONNET_INTERNAL_HTTP_BIND)\n",
							bind_address.c_str(), static_cast<unsigned>(bind_port));
					std::fflush(nullptr);
					program::drain_log_tee(500);
					std::_Exit(1);
				},
				[internal] { internal->stop(); } });
		supervisor->set_pass_guard(
				[&leader](db::Database &db) { return leader.still_leads(db); });
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
	leader.start();

	// The fleet's watchdog: its supervisor's passes while it leads, the
	// election's turns while it waits as a standby (no stall), and with
	// hosting off the election's turns alone.
	Watchdog watchdog;
	if (supervisor) {
		hosted::Supervisor *watched = supervisor.get();
		watchdog.watch("hosted servers' supervisor", [watched, &leader] {
			return leader.leading() ? watched->pass_turns() : leader.turns();
		});
	} else {
		watchdog.watch("election", [&leader] { return leader.turns(); });
	}

	program::LoopHooks hooks;
	uint64_t launch_failures_reported = 0;
	hosted::Supervisor *counted = supervisor.get();
	// The leader's own fields: the age of its supervisor's last pass that failed
	// in nothing (first, so a database that fails the rest still sends it), then
	// the fleet's health over the rows (hosted/fleet_health.h) against the
	// release's images.
	const hosted::ReleaseImages release{ fleet.image_id, fleet.hostd_image };
	const hosted::PlacementPolicy placement = fleet.placement;
	hooks.heartbeat = [counted, &leader, &launch_failures_reported, release, placement](
							  db::Database &db, Heartbeat &beat, int64_t window_s) {
		if (counted == nullptr)
			return;
		const uint64_t failures = counted->launch_failures();
		beat.launch_failures = static_cast<int64_t>(failures - launch_failures_reported);
		launch_failures_reported = failures;
		beat.hosted_live = hosted::count_live(db);
		beat.hosted_failed = hosted::count_failed_since(db, window_s);
		beat.machines_lost = hosted::count_machines(db, hosted::FleetMachineState::Lost);
		beat.machines_held = hosted::count_held_machines(db);
		beat.limits_held = hosted::limits_hold::held(db) ? 1 : 0;
		if (!leader.leading())
			return;
		const uint64_t now_ms = counted->config().clock_ms();
		const uint64_t good_ms = counted->last_good_pass_ms();
		beat.fleet_pass_age_s =
				static_cast<int64_t>(now_ms > good_ms ? (now_ms - good_ms) / 1000 : 0);
		const hosted::HealthInputs inputs = hosted::read_health(db, &placement);
		const hosted::HealthGauges gauges =
				hosted::health_gauges(inputs, hosted::fleet_health(inputs, release));
		beat.attention_danger = gauges.danger;
		beat.attention_warn = gauges.warn;
		beat.machines_stuck = gauges.machines_stuck;
		if (gauges.disk_free_min_percent)
			beat.machine_disk_free_pct = *gauges.disk_free_min_percent;
	};
	const int exit_code = program::run_main_loop("fleet", boot, watchdog, hooks);

	// Its parts in reverse (the console drain; the supervisor's thread, so no
	// pass runs, and the admin links while their servers still run, each
	// logged-in session ending with QUIT; the internal listener), then its lock
	// goes, and a standby leads. The rows keep their states; the cloud machines and their
	// containers run on, and the next leader's reconciliation resumes them.
	std::printf("[shutdown] stopping the fleet\n");
	leader.stop();
	return exit_code;
}

} // namespace

int main() {
	return opennova::novaworld_server::program::run_program({ "fleet", configure, run });
}
