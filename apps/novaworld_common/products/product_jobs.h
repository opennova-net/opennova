#pragma once

// The hosted products' work as jobs (jobs/job_queue.h, ADR 0064): what the admin routes
// enqueue in the transaction of the row change that causes it, and the data program's workers
// (apps/novaworld_data) run against the object store (core/object_store.h) and the rows
// (product_repository.h), the store's calls bounded by its own timeouts, so a 1 GB tree only
// occupies a data worker, never the worker program's machine launches, and no watched loop waits
// on it.
// Pattern of fleet_jobs.h: payload ints, dedupe keys, a state guard first, each idempotent.
// Service policy, not a port.
//
//   release.validate  {release_id}, cap 1. The package at updates/upload-<id>.sfs: the release
//                     must be validating. Its SHA-256 (another release's: invalid), read as
//                     1SFS, each section mapped to one product by the command file it carries
//                     (a base section to a base product or standalone updater product, an
//                     expansion section to an expansion of that code under the package's
//                     base), every row of each command file
//                     checked as the apply library checks it (its blob there, inflating to its
//                     size, its crc, a safe name, a TARGET "." or a .pff); an uploaded package
//                     (no release.build made it) mapping a game with a project refused, that
//                     game's releases being built; the object copied to updates/<sha>.sfs and
//                     the row made draft with its sections (set_release_validated: an upload
//                     that named no game becomes its base section's), the upload object
//                     removed after. A refusal makes the release invalid with its reason.
//   release.apply     {release_id, product_id}, cap 1. One section of a published release
//                     built into its product's tree from nothing: the release must still be
//                     published and the product's published release; the section's tree, when
//                     it has one, still building (a run again: its objects removed, the same
//                     row continued). A building tree row is made (the section's), the package
//                     downloaded and the section applied with formats/update/apply.h to an empty
//                     directory (every release is whole: its rows make every archive, resource.pff
//                     and the rest, from nothing; an expansion's tree is its own, beside its base
//                     game's), the files the kind takes (tree_files.h canonical_tree_file)
//                     kept, a required one missing failing the tree; the missions the catalog's
//                     multiplayer rows (a base tree mounted as opennova-serve mounts an install,
//                     mission_catalog::build; an expansion tree its pair alone,
//                     mission_catalog::archive_rows), the version the section's VERSION; each
//                     file sized and uploaded under trees/<id>/, trees/<id>/manifest.json
//                     written, the row made ready (finish_tree: the product's current tree).
//                     Standalone updater products queue no apply job. A refusal fails the tree
//                     with the error; the release stays published (the admin retires it); a
//                     store failure runs the job again, and fails the tree once its attempts
//                     are used up.
//   release.build     {release_id}, cap 1: a release built from a revision of its game's
//                     project, handed to release.validate as an upload is (release_build.h,
//                     ADR 0069).
//   tree.prune        {}, cap 1. The objects of every tree prunable_trees names removed, the
//                     row marked pruned. Hourly (product_schedule) and after each apply.
//   game.purge        {product_id, trees: [tree_id]}, cap 1. A deleted game's objects: each of
//                     its trees' prefixes and its project's files and manifests (content/<id>/,
//                     project_repository.h), removed whole. The games' admin delete enqueues it
//                     in the transaction that deletes the row, so the route answers at once
//                     however many files a retail game's project holds, and it runs
//                     kGamePurgeDelaySeconds later. A store failure runs it again.
//
// The routes' enqueues below return the job's id, 0 when one of its dedupe key is queued or
// running already.

#include "core/object_store.h"
#include "jobs/scheduler.h"
#include "jobs/worker_pool.h"

#include <net/novaworld/db/database.h>

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::novaworld_server::hosted {

inline constexpr const char *kJobReleaseValidate = "release.validate";
inline constexpr const char *kJobReleaseApply = "release.apply";
inline constexpr const char *kJobTreePrune = "tree.prune";
inline constexpr const char *kJobGamePurge = "game.purge";

// The object a tree's manifest is written as, under its prefix.
inline constexpr const char *kTreeManifestName = "manifest.json";

// A tree's prefix ("trees/<id>/"), the package of a validated release ("updates/<sha>.sfs")
// and the upload of one not yet validated ("updates/upload-<id>.sfs").
std::string tree_prefix(int64_t tree_id);
std::string release_package_key(const std::string &sha256);
std::string release_upload_key(int64_t release_id);

// How old a ready or failed tree must be before tree.prune takes it (product_repository.h
// prunable_trees).
inline constexpr int64_t kPruneMinAgeS = 60 * 60;

int64_t enqueue_release_validate(db::Database &db, int64_t release_id);
// Standalone updater products have no hosted tree and queue no apply job (returns 0).
int64_t enqueue_release_apply(db::Database &db, int64_t release_id, int64_t product_id);
int64_t enqueue_tree_prune(db::Database &db);
// A game's purge waits this long after its delete: past every presigned transfer its project's
// routes handed out before it (an hour, apps/novaworld_web/products/project_urls.h
// S3ContentUrls::kExpiresS) and the jobs then working on its trees, so none of them writes an
// object back under its prefixes after the purge removed them.
inline constexpr int64_t kGamePurgeDelaySeconds = 75 * 60;

// The objects of the game `product_id` (deleted in the caller's transaction) and of its trees
// `tree_ids` removed by the data program, kGamePurgeDelaySeconds from now.
int64_t enqueue_game_purge(
		db::Database &db, int64_t product_id, const std::vector<int64_t> &tree_ids);

// The handlers over `store`, which must outlive the workers; `scratch_dir` is where a job
// downloads what it works on (one directory of its own per job, removed when it ends).
void register_product_jobs(
		jobs::JobRegistry &registry, ObjectStore &store, std::string scratch_dir);
// The periodic prune, hourly (the data program's scheduler, while there is a store).
std::vector<jobs::PeriodicJob> product_schedule();

} // namespace opennova::novaworld_server::hosted
