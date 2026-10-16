#pragma once

// The hosted data sets' admin routes (ADR 0064; README "Game data"): the data sets an admin
// creates as rows (hosted/data_set_repository.h), the trees the admin page uploads into the
// deployment's object store (object_store.h: this program begins each file's multipart
// upload, takes each part's PUT and hands it to the store, and completes the upload), their
// finalize and delete. Each behind WebAccess::require_admin (and ONNET_API_PROXY_ONLY like
// every /api route), its state change logged with its actor and status (AccessMiddleware):
//
//   GET    /api/admin/data-sets                       the store, the titles, the games, every
//                                                     data set with its trees
//   POST   /api/admin/data-sets                       a new data set (a base or an expansion)
//   PATCH  /api/admin/data-sets/<id>                  its name, lobby
//   DELETE /api/admin/data-sets/<id>                  its trees marked pruned, the row gone
//   POST   /api/admin/data-sets/<id>/trees            a tree upload begun: {files:[{name,size}]}
//   PUT    /api/admin/data-sets/<id>/trees/<t>/files/<name>/parts/<n>?upload_id=<u>
//                                                     one part's bytes, {etag} back
//   POST   /api/admin/data-sets/<id>/trees/<t>/files/<name>/complete
//   POST   /api/admin/data-sets/<id>/trees/<t>/finalize   finalizing + the tree.finalize job
//   DELETE /api/admin/data-sets/<id>/trees/<t>        an upload aborted, or pruned + tree.prune
//
// Every reply is an error code with our own message, never database or store text; a
// timestamp not yet set is "" (the DB text). The jobs the routes enqueue commit in the
// transaction of the state move that causes them (hosted/data_set_jobs.h). Without a store
// (hosting off) every store route answers 503 store_unavailable. The admin's update-all (POST
// /api/admin/hosted/update-all) sits with the fleet routes. The HttpListener owns one
// DataSetRoutes.

#include "web_access.h"

#include <net/novaworld/db/database.h>

#include <crow.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace opennova::novaworld_server {

class ObjectStore;

namespace hosted {

struct DataSetTree; // hosted/data_set_repository.h

// A tree as the admin's JSON: {id, state, version, bytes, files (how many), missions (how
// many), pinned_by (the live servers pinning it), error, created_by, created_at, ready_at ("" until
// ready)}.
crow::json::wvalue tree_json(db::Database &db, const DataSetTree &tree);

// The key a tree's file is stored under (under tree_prefix, hosted/data_set_jobs.h).
std::string tree_file_key(int64_t tree_id, std::string_view name);

class DataSetRoutes {
public:
	// One part of a multipart upload (the page's slices, S3's parts: inside Cloudflare's
	// request body limit), and the most parts a file may take (S3's own limit).
	static constexpr int64_t kPartSize = 32 * 1024 * 1024;
	static constexpr int kMaxParts = 10000;
	// The shapes a data set's fields take.
	static constexpr size_t kDisplayNameMax = 64;
	static constexpr size_t kExpansionCodeMax = 8;

	DataSetRoutes(WebAccess &access, db::ConnectionPool &pool) : access_(access), pool_(pool) {}

	DataSetRoutes(const DataSetRoutes &) = delete;
	DataSetRoutes &operator=(const DataSetRoutes &) = delete;

	// The deployment's object store, null while hosting is off (the store routes then answer
	// 503 store_unavailable). It must outlive the listener. Before register_routes().
	void set_store(ObjectStore *store) { store_ = store; }
	void register_routes(WebApp &app);

private:
	crow::response list(const crow::request &req);
	crow::response create(const crow::request &req);
	crow::response patch(const crow::request &req, uint64_t id);
	crow::response remove(const crow::request &req, uint64_t id);
	crow::response begin_tree(const crow::request &req, uint64_t id);
	crow::response tree_file_part(const crow::request &req, uint64_t id, uint64_t tree,
			const std::string &name, uint64_t part);
	crow::response tree_file_complete(
			const crow::request &req, uint64_t id, uint64_t tree, const std::string &name);
	crow::response tree_finalize(const crow::request &req, uint64_t id, uint64_t tree);
	crow::response tree_remove(const crow::request &req, uint64_t id, uint64_t tree);

	WebAccess &access_;
	db::ConnectionPool &pool_;
	ObjectStore *store_ = nullptr;
};

} // namespace hosted
} // namespace opennova::novaworld_server
