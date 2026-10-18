#pragma once

// The hosted data sets' work as jobs (jobs/job_queue.h, ADR 0064): what the admin routes
// enqueue in the transaction of the row change that causes it, and the data program's workers
// (apps/novaworld_data) run against the object store (object_store.h) and the rows
// (data_set_repository.h), the store's calls bounded by its own timeouts, so a 1 GB tree only
// occupies a data worker, never the worker program's machine launches, and no watched loop waits
// on it.
// Pattern of fleet_jobs.h: payload ints, dedupe keys, a state guard first, each idempotent.
// Service policy, not a port.
//
//   tree.finalize     {tree_id}, cap 2. A baseline the admin uploaded under trees/<id>/: the
//                     tree must be finalizing. Every object downloaded to the worker's scratch
//                     directory; a name the data set's kind does not take
//                     (data_set_files.h canonical_tree_file) or a required one missing fails
//                     the tree; each file sized; the version the first line of version.txt;
//                     the missions the catalog's multiplayer rows (a base tree mounted as
//                     opennova-serve mounts an install, mission_catalog::build; an expansion
//                     tree its pair alone, mission_catalog::archive_rows); then
//                     trees/<id>/manifest.json written and the row made ready (finish_tree:
//                     the data set's current and baseline tree, the published release
//                     retired). A refusal fails the tree with its reason and the job is done
//                     (the admin reads it and deletes the tree); a store failure runs the job
//                     again, and fails the tree once its attempts are used up.
//   release.validate  {release_id}, cap 1. The package at updates/upload-<id>.sfs: the release
//                     must be validating. Its SHA-256 (another release's: invalid), read as
//                     1SFS, each section mapped to one data set by the command file it carries
//                     (a base section to a base data set, an expansion section to an expansion
//                     of that code under the package's base), every row of each command file
//                     checked as the apply library checks it (its blob there, inflating to its
//                     size, its crc, a safe name, a TARGET "." or a .pff); the object copied to
//                     updates/<sha>.sfs and the row made draft with its sections
//                     (set_release_validated), the upload object removed after. A refusal
//                     makes the release invalid with its reason.
//   release.apply     {release_id, data_set_id}, cap 1. One section of a published release
//                     applied to its data set's baseline: the release must still be published
//                     and the data set's published release; the section's tree, when it has
//                     one, still building (a run again: its objects removed, the same row
//                     continued). A building tree row is made (the section's), the baseline's
//                     objects copied server-side under it, the package and the baseline files
//                     the rows touch downloaded, the section applied with
//                     formats/update/apply.h, each changed file sized and uploaded and each
//                     deleted one removed, the manifest, the version and the missions redone,
//                     the row made ready (the data set's current tree). No ready baseline: the
//                     tree fails "no baseline uploaded". A refusal fails the tree with the
//                     error; the release stays published (the admin retires it).
//   tree.prune        {}, cap 1. The objects of every tree prunable_trees names removed, its
//                     uploads in flight aborted, the row marked pruned. Hourly
//                     (data_set_schedule), after each job above, and from the delete routes.
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
inline constexpr const char *kJobReleaseValidate = "release.validate";
inline constexpr const char *kJobReleaseApply = "release.apply";
inline constexpr const char *kJobTreePrune = "tree.prune";

// The object a tree's manifest is written as, under its prefix.
inline constexpr const char *kTreeManifestName = "manifest.json";

// A tree's prefix ("trees/<id>/"), the package of a validated release ("updates/<sha>.sfs")
// and the upload of one not yet validated ("updates/upload-<id>.sfs").
std::string tree_prefix(int64_t tree_id);
std::string release_package_key(const std::string &sha256);
std::string release_upload_key(int64_t release_id);

// How old a ready or failed tree must be, and an upload left in flight, before tree.prune
// takes it (data_set_repository.h prunable_trees).
inline constexpr int64_t kPruneMinAgeS = 60 * 60;
inline constexpr int64_t kPruneAbandonedUploadS = 7 * 24 * 60 * 60;

int64_t enqueue_tree_finalize(db::Database &db, int64_t tree_id);
int64_t enqueue_release_validate(db::Database &db, int64_t release_id);
int64_t enqueue_release_apply(db::Database &db, int64_t release_id, int64_t data_set_id);
int64_t enqueue_tree_prune(db::Database &db);

// The handlers over `store`, which must outlive the workers; `scratch_dir` is where a job
// downloads what it works on (one directory of its own per job, removed when it ends).
void register_data_set_jobs(
		jobs::JobRegistry &registry, ObjectStore &store, std::string scratch_dir);
// The periodic prune, hourly (the data program's scheduler, parts.h data_schedule).
std::vector<jobs::PeriodicJob> data_set_schedule();

} // namespace opennova::novaworld_server::hosted
