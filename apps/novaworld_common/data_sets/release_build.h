#pragma once

// A release built from a revision of its game's project (ADR 0069 d7; content_repository.h): the
// data program's release.build job, which the content store's release route enqueues in the
// transaction that makes the release (building). Every release of a game with a project is
// built so; only a game with none (the runtime) has its packages uploaded:
//
//   release.build  {release_id}, cap 1. The release must be building. Its project's revision
//                  is laid out from the store (every file by its SHA-256 from the program's own
//                  cache of the project's files, synced down once per build: sync_down), an
//                  expansion's beside its base game's revision where its project.opennova's
//                  base_project names it, and exported by opennova-project. Every package is
//                  whole (--full: every file of the export a row, so the jointops and dfx2 zips
//                  install their game into an empty folder from it), carrying the removals
//                  since the data set's published release (--previous) and leaving out the
//                  runtime's own files in a game's folder (update.exe, revupdat.exe,
//                  README.txt). A retail game's also carries the removals since its stock
//                  revision (1, a fresh import of the stock install, an expansion's on its
//                  base's, exported by our own writer as the edited one is); our own game's
//                  tree gets an UPDATE.WIZ naming its product (the deployment's update host,
//                  ONNET_UPDATE_HOST, its URL). opennova-update-tool packs it with the data
//                  set's command file, product name and executable. The package is put where an
//                  upload lands (release_upload_key), and the release made validating with
//                  release.validate queued, as a completed upload is. A refusal (no project, no
//                  product, a tool failing) makes the release invalid with its reason, what the
//                  tools said kept as its build_log; a store failure runs the job again.
//
// The tools run as child processes from argv arrays (apps/common/command_runner.h), their
// environment the system's alone: they never see the store's credentials. Service policy, not a
// port.

#include "core/object_store.h"
#include "jobs/worker_pool.h"

#include <net/novaworld/db/database.h>

#include <cstdint>
#include <string>

namespace opennova::novaworld_server::hosted {

inline constexpr const char *kJobReleaseBuild = "release.build";

// What a build runs: opennova-project (its export) and opennova-update-tool (its package
// builder), by absolute path; the host our own game's UPDATE.WIZ names ("" names none, and the
// updater dials its own); how long either may run.
struct ReleaseTools {
	std::string project_tool;
	std::string update_tool;
	std::string update_host;
	int timeout_ms = 60 * 60 * 1000;
};

// The release `release_id`'s build queued; the job's id, 0 when one is queued or running.
int64_t enqueue_release_build(db::Database &db, int64_t release_id);
// That build's job's dedupe key (jobs::job_pending: whether it is queued or running).
std::string release_build_key(int64_t release_id);

// release.build's handler over `store`, its work under `scratch_dir` (the projects' files cached
// there between builds).
void register_release_build(jobs::JobRegistry &registry, ObjectStore &store,
		std::string scratch_dir, ReleaseTools tools);

} // namespace opennova::novaworld_server::hosted
