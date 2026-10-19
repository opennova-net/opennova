#include "fleet_jobs.h"

#include "jobs/job_queue.h"

#include <utility>

namespace opennova::novaworld_server::hosted {

namespace {

std::string launch_key(int64_t machine_id) {
	return std::string(kJobMachineLaunch) + ":" + std::to_string(machine_id);
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

std::vector<jobs::PeriodicJob> fleet_schedule(int64_t sweep_interval_ms) {
	jobs::PeriodicJob p;
	p.kind = kJobFleetSweep;
	p.period_ms = sweep_interval_ms;
	return { std::move(p) };
}

} // namespace opennova::novaworld_server::hosted
