#include "fleet_jobs.h"

#include "fleet_repository.h"
#include "loggable.h"

#include <cstdio>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>

namespace opennova::novaworld_server::hosted {

namespace {

std::string launch_key(int64_t machine_id) {
	return std::string(kJobMachineLaunch) + ":" + std::to_string(machine_id);
}

int64_t member_int(const io::JsonValue &payload, const char *key) {
	const io::JsonValue *v = payload.get(key);
	return v != nullptr && v->is_number() ? static_cast<int64_t>(v->number) : 0;
}

jobs::JobResult launch(jobs::JobContext &ctx, MachineDriver &driver, const PlacementPolicy &limits) {
	const int64_t id = member_int(ctx.job.payload, "machine_id");
	std::optional<FleetMachineRow> row;
	int of_kind = 0;
	{
		auto lease = ctx.pool.acquire();
		row = find_machine(*lease, id);
		// The machines of its kind that take servers, as placement counts them
		// toward the maximum: this one among them.
		if (row)
			for (const FleetMachineRow &m : list_live_machines(*lease))
				if (m.kind == row->kind && machine_takes_servers(m.state))
					++of_kind;
	}
	// Ended meanwhile (an admin's end, the supervisor's "never finished"): nothing to launch.
	if (!row || row->state != FleetMachineState::Launching)
		return jobs::JobResult::done();
	// Past the fleet's maximum for the kind, whatever made the row: no cloud call.
	const bool own = row->kind == MachineKind::Own;
	const int max = own ? limits.max_own : limits.max_shared;
	if (of_kind > max) {
		std::fprintf(stderr,
				"[fleet] machine %lld (%s, for server %lld): refused, %d %s machines would run past "
				"the fleet's maximum of %d (%s)\n",
				static_cast<long long>(id), machine_kind_text(row->kind),
				static_cast<long long>(row->launched_for), of_kind, machine_kind_text(row->kind),
				max, own ? "ONNET_FLEET_MAX_OWN" : "ONNET_FLEET_MAX_SHARED");
		auto lease = ctx.pool.acquire();
		fail_refused_launch(*lease, id);
		return jobs::JobResult::fail("past the fleet's maximum");
	}
	MachineLaunch request;
	request.machine_id = id;
	request.kind = row->kind;
	request.slots = row->slots;
	request.bootstrap_token = ctx.job.secret;
	request.attempt = ctx.job.attempts;
	// Where an earlier run last called the cloud, and each call's subnet written
	// before the call: a run again after this one (its mark_launched failed, its
	// claim lapsed) calls there first, under the same client token, though the
	// cloud may not list this run's machine yet.
	request.subnet = row->launch_subnet;
	request.record_subnet = [&ctx, id](const std::string &subnet) {
		try {
			auto lease = ctx.pool.acquire();
			return record_launch_subnet(*lease, id, subnet);
		} catch (const db::Error &e) {
			std::fprintf(stderr, "[fleet] machine %lld: its launch's subnet was not recorded: %s\n",
					static_cast<long long>(id), e.what());
			return false;
		}
	};
	std::string error;
	const std::optional<LaunchedMachine> launched = driver.launch(request, error);
	auto lease = ctx.pool.acquire();
	db::Database &db = *lease;
	if (!launched) {
		// The driver's own text (a CLI's error) goes to the log alone; the server's
		// event, which the owner reads, says what happened (the supervisor's).
		std::fprintf(stderr, "[fleet] machine %lld (%s, for server %lld): the launch failed: %s\n",
				static_cast<long long>(id), machine_kind_text(row->kind),
				static_cast<long long>(row->launched_for), error.c_str());
		fail_refused_launch(db, id);
		return jobs::JobResult::fail("the launch failed");
	}
	if (mark_launched(db, id, *launched)) {
		std::printf("[fleet] machine %lld (%s, %d slot%s) launched as %s for server %lld\n",
				static_cast<long long>(id), machine_kind_text(row->kind), row->slots,
				row->slots == 1 ? "" : "s", loggable(launched->cloud_id).c_str(),
				static_cast<long long>(row->launched_for));
		return jobs::JobResult::done();
	}
	// The row left launching meanwhile. Another run of this job (the same client
	// token) recorded this very machine: it stays. Otherwise no row runs it.
	const auto now = find_machine(db, id);
	if (now && now->cloud_id == launched->cloud_id)
		return jobs::JobResult::done();
	std::printf("[fleet] machine %lld left launching during its launch; %s ends\n",
			static_cast<long long>(id), loggable(launched->cloud_id).c_str());
	driver.terminate(launched->cloud_id);
	return jobs::JobResult::done();
}

jobs::JobResult describe(jobs::JobContext &ctx, MachineDriver &driver) {
	const int64_t id = member_int(ctx.job.payload, "machine_id");
	const io::JsonValue *cloud_id = ctx.job.payload.get("cloud_id");
	if (cloud_id == nullptr || !cloud_id->is_string() || cloud_id->string.empty())
		return jobs::JobResult::fail("no cloud machine named");
	// The launch's grace by the row's clock, whichever worker launched it.
	int64_t launched_ago = -1;
	{
		auto lease = ctx.pool.acquire();
		launched_ago = launched_ago_ms(*lease, id).value_or(-1);
	}
	const CloudMachine cloud = driver.describe(cloud_id->string, launched_ago);
	auto lease = ctx.pool.acquire();
	record_cloud_state(*lease, id, cloud_id->string, cloud);
	return jobs::JobResult::done();
}

jobs::JobResult terminate(jobs::JobContext &ctx, MachineDriver &driver) {
	const io::JsonValue *cloud_id = ctx.job.payload.get("cloud_id");
	if (cloud_id == nullptr || !cloud_id->is_string() || cloud_id->string.empty())
		return jobs::JobResult::fail("no cloud machine named");
	driver.terminate(cloud_id->string);
	return jobs::JobResult::done();
}

jobs::JobResult sweep(jobs::JobContext &ctx, MachineDriver &driver) {
	std::vector<TaggedCloudMachine> tagged;
	try {
		tagged = driver.list_tagged();
	} catch (const std::runtime_error &e) {
		// No listing, nothing ended; the next sweep tries again.
		std::fprintf(stderr, "[fleet] WARN sweep: %s; nothing is ended\n", e.what());
		return jobs::JobResult::fail("the machines could not be listed");
	}
	std::set<std::string> listed;
	std::vector<std::string> orphans;
	std::vector<FleetMachineRow> unlisted;
	{
		auto lease = ctx.pool.acquire();
		db::Database &db = *lease;
		for (const TaggedCloudMachine &t : tagged) {
			const auto row = t.machine_id > 0 ? find_machine(db, t.machine_id) : std::nullopt;
			const bool live = row && machine_is_live(row->state) && row->provider == driver.name();
			// A row still launching (no cloud id until its launch's answer is written)
			// names whatever its launch in flight made: never an orphan.
			if (live && (row->state == FleetMachineState::Launching || row->cloud_id.empty()))
				continue;
			if (!live || row->cloud_id != t.cloud_id) {
				std::printf(
						"[fleet] sweep: machine %s (row %lld) is no live machine's; ending it\n",
						loggable(t.cloud_id).c_str(), static_cast<long long>(t.machine_id));
				orphans.push_back(t.cloud_id);
				continue;
			}
			listed.insert(t.cloud_id);
		}
		for (FleetMachineRow &m : list_live_machines(db)) {
			if (m.state == FleetMachineState::Launching || m.cloud_id.empty() ||
					listed.count(m.cloud_id) != 0)
				continue;
			unlisted.push_back(std::move(m));
		}
	}
	for (const std::string &cloud_id : orphans)
		driver.terminate(cloud_id);
	// Not listed: the driver's own describe decides (a cloud machine launched a
	// moment ago may not be listed yet); a gone one ends at the next pass.
	for (const FleetMachineRow &m : unlisted) {
		int64_t launched_ago = -1;
		{
			auto lease = ctx.pool.acquire();
			launched_ago = launched_ago_ms(*lease, m.id).value_or(-1);
		}
		const CloudMachine cloud = driver.describe(m.cloud_id, launched_ago);
		auto lease = ctx.pool.acquire();
		record_cloud_state(*lease, m.id, m.cloud_id, cloud);
	}
	return jobs::JobResult::done();
}

} // namespace

int64_t enqueue_machine_launch(db::Database &db, int64_t machine_id, MachineKind kind, int slots,
		const std::string &bootstrap_token) {
	jobs::NewJob job;
	job.kind = kJobMachineLaunch;
	job.payload.set("machine_id", io::json_number(static_cast<double>(machine_id)));
	job.payload.set("kind", io::json_string(machine_kind_text(kind)));
	job.payload.set("slots", io::json_number(slots));
	job.secret = bootstrap_token;
	job.dedupe_key = launch_key(machine_id);
	job.max_attempts = 5;
	return jobs::enqueue(db, job);
}

int64_t enqueue_machine_describe(
		db::Database &db, int64_t machine_id, const std::string &cloud_id) {
	jobs::NewJob job;
	job.kind = kJobMachineDescribe;
	job.payload.set("machine_id", io::json_number(static_cast<double>(machine_id)));
	job.payload.set("cloud_id", io::json_string(cloud_id));
	job.dedupe_key = std::string(kJobMachineDescribe) + ":" + std::to_string(machine_id);
	job.max_attempts = 2;
	return jobs::enqueue(db, job);
}

int64_t enqueue_machine_terminate(db::Database &db, const std::string &cloud_id) {
	jobs::NewJob job;
	job.kind = kJobMachineTerminate;
	job.payload.set("cloud_id", io::json_string(cloud_id));
	job.dedupe_key = std::string(kJobMachineTerminate) + ":" + cloud_id;
	job.max_attempts = 5;
	return jobs::enqueue(db, job);
}

int64_t enqueue_fleet_sweep(db::Database &db) {
	jobs::NewJob job;
	job.kind = kJobFleetSweep;
	job.dedupe_key = kJobFleetSweep;
	job.max_attempts = 1;
	return jobs::enqueue(db, job);
}

bool machine_launch_pending(db::Database &db, int64_t machine_id) {
	return jobs::job_pending(db, launch_key(machine_id));
}

void register_fleet_jobs(jobs::JobRegistry &registry, MachineDriver &driver,
		PlacementPolicy limits, FleetJobCaps caps) {
	jobs::JobKind launch_kind{ kJobMachineLaunch,
		[&driver, limits](jobs::JobContext &ctx) { return launch(ctx, driver, limits); },
		caps.launch };
	launch_kind.retry_base_ms = 2000;
	launch_kind.retry_max_ms = 60 * 1000;
	registry.add(std::move(launch_kind));
	jobs::JobKind describe_kind{ kJobMachineDescribe,
		[&driver](jobs::JobContext &ctx) { return describe(ctx, driver); }, caps.describe };
	describe_kind.retry_base_ms = 1000;
	describe_kind.retry_max_ms = 10 * 1000;
	registry.add(std::move(describe_kind));
	registry.add({ kJobMachineTerminate,
			[&driver](jobs::JobContext &ctx) { return terminate(ctx, driver); }, caps.terminate });
	registry.add(
			{ kJobFleetSweep, [&driver](jobs::JobContext &ctx) { return sweep(ctx, driver); }, 1 });
}

std::vector<jobs::PeriodicJob> fleet_schedule(int64_t sweep_interval_ms) {
	jobs::PeriodicJob p;
	p.kind = kJobFleetSweep;
	p.period_ms = sweep_interval_ms;
	return { std::move(p) };
}

} // namespace opennova::novaworld_server::hosted
