#pragma once

// The fleet's cloud calls as jobs (jobs/job_queue.h): the supervisor's pass
// never calls the MachineDriver (machine_driver.h); it enqueues these, in the
// transaction of the row change that causes them, and reads what they found
// from the machine rows (fleet_repository.h). The workers run them
// (hosted/cloud/fleet_job_handlers.h), the driver's calls bounded by its own
// timeouts, so a slow cloud only occupies a worker and no watched loop
// (core/health.h) waits on it. Each is idempotent. Service policy, not a
// port.
//
//   machine.launch     the row's launch (its id, kind and slots; its one-time
//                      bootstrap token the job's secret), skipped once the row
//                      left launching, and refused before any cloud call when
//                      the machines of its kind that take servers (placement.h
//                      machine_takes_servers, the row itself among them) run
//                      past the fleet's maximum for the kind
//                      (ONNET_FLEET_MAX_SHARED, ONNET_FLEET_MAX_OWN): a runaway
//                      pass or a forged row launches nothing past the cap, and
//                      the row fails as a refused launch. Launched: launching -> booting
//                      (mark_launched); the row left launching meanwhile: the
//                      cloud machine is ended, unless another run of this job
//                      recorded it. Refused: the row failed with the refusal
//                      left for the supervisor to count (fail_refused_launch,
//                      its end reason and event naming the cloud's error code,
//                      launch_failure_reason; the driver's own text goes to the
//                      log alone), the job failed for good. A run cut short (a worker gone, a
//                      database error) runs again with the same machine id, so
//                      the same client token in each subnet (told it runs
//                      again, the driver reads where its first run launched
//                      and tries that subnet first): EC2 answers it with the
//                      machine it launched then, never a second one.
//   machine.describe   what the cloud says of a live machine, onto its row
//                      (record_cloud_state); deduped per machine.
//   machine.terminate  the cloud machine ended (the driver checks it is this
//                      deployment's); deduped per cloud machine.
//   fleet.sweep        the driver's machines against the rows: a cloud
//                      machine no live row names is terminated (one whose
//                      row still launches is its launch's, kept), and a live
//                      row's machine the listing lacks is described (one that
//                      is gone ends at the next pass). A listing that fails
//                      ends nothing and fails the job (the next sweep tries
//                      again). Then the static addresses the cloud holds
//                      that no held row names are recorded as orphans
//                      (hosted/cloud/address_job_handlers.h
//                      sweep_addresses; never released).
//                      Periodic (fleet_schedule, every
//                      kFleetSweepIntervalMs), and at the supervisor's
//                      reconciliation.

#include "jobs/scheduler.h"
#include "placement.h"

#include <net/novaworld/db/database.h>

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::novaworld_server::hosted {

inline constexpr const char *kJobMachineLaunch = "machine.launch";
inline constexpr const char *kJobMachineDescribe = "machine.describe";
inline constexpr const char *kJobMachineTerminate = "machine.terminate";
inline constexpr const char *kJobFleetSweep = "fleet.sweep";

// The fleet.sweep job's period (fleet_schedule).
inline constexpr int64_t kFleetSweepIntervalMs = 5 * 60 * 1000;

// The launch of machine `machine_id` (`kind`, `slots`), its token carried as
// the job's secret; on `db`, in the caller's transaction. Its job id.
int64_t enqueue_machine_launch(db::Database &db, int64_t machine_id, MachineKind kind, int slots,
		const std::string &bootstrap_token);
// A describe of machine `machine_id` (`cloud_id`): 0 when one is queued or running.
int64_t enqueue_machine_describe(db::Database &db, int64_t machine_id, const std::string &cloud_id);
int64_t enqueue_machine_terminate(db::Database &db, const std::string &cloud_id);
int64_t enqueue_fleet_sweep(db::Database &db);
// Whether machine `machine_id`'s launch is queued or running.
bool machine_launch_pending(db::Database &db, int64_t machine_id);

// The periodic sweep, every `sweep_interval_ms`.
std::vector<jobs::PeriodicJob> fleet_schedule(int64_t sweep_interval_ms = kFleetSweepIntervalMs);

} // namespace opennova::novaworld_server::hosted
