#pragma once

// The hosted data sets' work as jobs (jobs/job_queue.h, ADR 0064): what the admin routes
// enqueue in the transaction of the row change that causes it, and the workers run against the
// object store (object_store.h) and the rows (data_set_repository.h), the store's calls bounded
// by its own timeouts, so a 1 GB tree only occupies a worker and no watched loop waits on it.
// Pattern of fleet_jobs.h: payload ints, dedupe keys, a state guard first, each idempotent.
// Service policy, not a port.
//
//   tree.finalize     {tree_id}, cap 2. A tree the admin uploaded under trees/<id>/: the tree
//                     must be finalizing. Every object downloaded to the worker's scratch
//                     directory; a name the data set's kind does not take
//                     (data_set_files.h canonical_tree_file) or a required one missing fails
//                     the tree; each file sized; the version the first line of version.txt;
//                     the missions the catalog's multiplayer rows (a base tree mounted as
//                     opennova-serve mounts an install, mission_catalog::build; an expansion
//                     tree its pair alone, mission_catalog::archive_rows); then
//                     trees/<id>/manifest.json written and the row made ready (finish_tree:
//                     the data set's current tree). A refusal fails the tree with its reason
//                     and the job is done (the admin reads it and deletes the tree); a store
//                     failure runs the job again, and fails the tree once its attempts are
//                     used up.
//   tree.prune        {}, cap 1. The objects of every tree prunable_trees names removed, its
//                     uploads in flight aborted, the row marked pruned. Hourly
//                     (data_set_schedule), after each finalize, and from the delete routes.
//
// The routes' enqueues below return the job's id, 0 when one of its dedupe key is queued or
// running already.

#include "jobs/scheduler.h"
#include "jobs/worker_pool.h"
#include "object_store.h"

#include <net/novaworld/db/database.h>

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::novaworld_server::hosted {

inline constexpr const char *kJobTreeFinalize = "tree.finalize";
inline constexpr const char *kJobTreePrune = "tree.prune";

// The object a tree's manifest is written as, under its prefix.
inline constexpr const char *kTreeManifestName = "manifest.json";

// A tree's prefix ("trees/<id>/").
std::string tree_prefix(int64_t tree_id);

// How old a ready or failed tree must be, and an upload left in flight, before tree.prune
// takes it (data_set_repository.h prunable_trees).
inline constexpr int64_t kPruneMinAgeS = 60 * 60;
inline constexpr int64_t kPruneAbandonedUploadS = 7 * 24 * 60 * 60;

int64_t enqueue_tree_finalize(db::Database &db, int64_t tree_id);
int64_t enqueue_tree_prune(db::Database &db);

// The handlers over `store`, which must outlive the workers; `scratch_dir` is where a job
// downloads what it works on (one directory of its own per job, removed when it ends).
void register_data_set_jobs(
		jobs::JobRegistry &registry, ObjectStore &store, std::string scratch_dir);
// The periodic prune, hourly (the controller's scheduler).
std::vector<jobs::PeriodicJob> data_set_schedule();

} // namespace opennova::novaworld_server::hosted
