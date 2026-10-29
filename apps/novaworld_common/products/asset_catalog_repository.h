#pragma once

// Native release artifacts and their resolved dependency closure. These never grant access to
// editor sources. Every public read checks the current sharing flag of every contributing project.
#include "core/object_store.h"
#include "jobs/worker_pool.h"
#include <net/novaworld/db/database.h>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace opennova::novaworld_server::hosted {

struct LibraryAsset {
	int64_t id = 0, product_id = 0, release_id = 0;
	std::string name, kind, sha256, project_key, project_name, owner, version;
	int64_t size = 0, file_count = 0, total_bytes = 0;
	std::string bundle_key, bundle_error;
};
struct LibraryFile {
	std::string name, sha256;
	int64_t size = 0, product_id = 0;
};

// The extractor writes manifest.json and blobs/<sha256>. Validate and upload first; then
// publish the complete snapshot in one transaction. A retry of identical content is a no-op.
bool store_asset_snapshot(db::Database &db, ObjectStore &store, int64_t release_id,
		const std::string &directory, std::string &error);
std::vector<LibraryAsset> list_library_assets(db::Database &db, const std::string &query,
		const std::string &kind, const std::string &project, int64_t before, int limit);
std::optional<LibraryAsset> find_library_asset(db::Database &db, int64_t id);
std::vector<LibraryFile> library_asset_files(db::Database &db, int64_t id);
std::string asset_blob_key(int64_t product_id, const std::string &sha256);
std::string asset_download_name(const LibraryAsset &asset);
bool library_asset_kind(const std::string &kind);

inline constexpr const char *kJobAssetBundle = "asset.bundle";
int64_t enqueue_asset_bundle(db::Database &db, int64_t id);
bool asset_bundle_pending(db::Database &db, int64_t id);
// A worker loss or cancellation can fail the job without recording an asset's bundle_error.
// Only the latest attempt matters: an explicit retry supersedes the previous failed job.
bool asset_bundle_failed(db::Database &db, int64_t id);
void register_asset_bundle(jobs::JobRegistry &registry, ObjectStore &store);

} // namespace opennova::novaworld_server::hosted
