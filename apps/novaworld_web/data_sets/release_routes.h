#pragma once

// The update releases' admin routes (ADR 0064; apps/novaworld_common/hosted/README.md
// "Game data"): the 1SFS packages an
// admin uploads into the object store (one multipart upload each, every part PUT through
// this program as the trees' are, data_set_routes.h), validated by the data program
// (release.validate), published to the updater (the update listener serves them,
// updater/release_catalog.h) and applied to their data sets (one release.apply job per
// section), retired and deleted. Each behind WebAccess::require_admin (and
// ONNET_API_PROXY_ONLY like every /api route), its state change logged with its actor and
// status (AccessMiddleware):
//
//   GET    /api/admin/releases                 every release with its sections, newest first
//   POST   /api/admin/releases                 {name?, size}: a release uploading
//   PUT    /api/admin/releases/<id>/parts/<n>?upload_id=<u>   one part's bytes, {etag} back
//   POST   /api/admin/releases/<id>/complete   the upload completed: validating + release.validate
//   POST   /api/admin/releases/<id>/publish    a draft published: one release.apply per section
//   POST   /api/admin/releases/<id>/retire     a published one retired
//   DELETE /api/admin/releases/<id>            a draft, invalid, retired or uploading one gone
//
// Every reply is an error code with our own message, never database or store text; a
// timestamp not yet set is "" (the DB text). Without a store (hosting off) every store route
// answers 503 store_unavailable. The HttpListener owns one ReleaseRoutes beside its
// DataSetRoutes.

#include "web_access.h"

#include <net/novaworld/db/database.h>

#include <crow.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace opennova::novaworld_server {

class ObjectStore;

namespace hosted {

struct ReleaseRow; // data_sets/data_set_repository.h

// A release as the admin's JSON: {id, status, name, sha256, bytes, package_product, error,
// sections ([{data_set_id, display_name, expansion, command_file, version, tree ({id,
// state} or null)}]), created_by, created_at, published_at, retired_at ("" until set)}.
crow::json::wvalue release_json(db::Database &db, const ReleaseRow &release);

class ReleaseRoutes {
public:
	static constexpr size_t kNameMax = 64;

	ReleaseRoutes(WebAccess &access, db::ConnectionPool &pool) : access_(access), pool_(pool) {}

	ReleaseRoutes(const ReleaseRoutes &) = delete;
	ReleaseRoutes &operator=(const ReleaseRoutes &) = delete;

	// The deployment's object store, null while hosting is off; it must outlive the
	// listener. Before register_routes().
	void set_store(ObjectStore *store) { store_ = store; }
	void register_routes(WebApp &app);

private:
	crow::response list(const crow::request &req);
	crow::response create(const crow::request &req);
	crow::response part(const crow::request &req, uint64_t id, uint64_t part);
	crow::response complete(const crow::request &req, uint64_t id);
	crow::response publish(const crow::request &req, uint64_t id);
	crow::response retire(const crow::request &req, uint64_t id);
	crow::response remove(const crow::request &req, uint64_t id);

	WebAccess &access_;
	db::ConnectionPool &pool_;
	ObjectStore *store_ = nullptr;
};

} // namespace hosted
} // namespace opennova::novaworld_server
