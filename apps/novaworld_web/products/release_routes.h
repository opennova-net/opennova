#pragma once

// The releases' admin routes (ADR 0064; apps/novaworld_common/hosted/README.md "Products"): every
// product's, built by the data program (products/release_build.h) from a game's project (the
// project routes' release route, its owner's or an admin's, products/project_routes.h, which
// publish, retire and delete a project's own releases as these do) or from an app's build
// uploaded into the object store (the
// Publish workflow's: one multipart upload each, every part PUT through this program; a game
// answers 409 built_from_project), an updatable app's packed and validated as a game's is
// (release.validate), a download-only app's kept as uploaded, published to the updater (the
// update listener serves the packages, updater/release_catalog.h) and built into their
// products' trees (one release.apply job per section), retired and deleted. Each behind
// WebAccess::require_admin (and ONNET_API_PROXY_ONLY like every /api route), its state change
// logged with its actor and status (AccessMiddleware):
//
//   GET    /api/admin/releases                 every release with its sections, newest first
//   POST   /api/admin/releases                 {name?, size, product_id, version}: an app's
//                                               release uploading
//   PUT    /api/admin/releases/<id>/parts/<n>?upload_id=<u>   one part's bytes, {etag} back
//   POST   /api/admin/releases/<id>/complete   the upload completed: building + release.build
//   POST   /api/admin/releases/<id>/publish    a draft published: one release.apply per section,
//                                               and downloads.publish (products/downloads_job.h)
//   POST   /api/admin/releases/<id>/retire     a published one retired, and downloads.publish
//   DELETE /api/admin/releases/<id>            a draft, invalid, retired or uploading one gone
//
// Every reply is an error code with our own message, never database or store text; a
// timestamp not yet set is "" (the DB text). Without a store (hosting off) every store route
// answers 503 store_unavailable. The HttpListener owns one ReleaseRoutes beside its
// ProductRoutes.

#include "web_access.h"

#include <net/novaworld/db/database.h>

#include <crow.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace opennova::novaworld_server {

class ObjectStore;

namespace hosted {

struct ReleaseRow; // products/product_repository.h

// A release as the admin's JSON: {id, status, name, sha256, bytes, package_product, error,
// product_id (its game; null for an upload that named none until its validation maps it),
// sections ([{product_id, display_name, expansion, command_file, version, tree ({id, state} or
// null)}]), created_by, created_at, published_at, retired_at ("" until set), source (a built
// release's {project, revision, base_revision (or null)}, null for an upload), version,
// build_log}.
crow::json::wvalue release_json(db::Database &db, const ReleaseRow &release);

// What a release's publish, retire and delete come to, the admin's routes' and a project's own
// (products/project_routes.h) alike, `actor` naming who in the log line ("admin <name>",
// "user <name>", "token"). The publish: a draft published (its sections' products serve it to the
// updater, the release they named before retired; an account's own game's first gets its own
// lobby, product_repository.h publish_release), one release.apply per section and the downloads
// host's render (downloads.publish) queued, in one transaction: 202 {"release"}; 409 not_draft.
// The retire: a published release retired and downloads.publish queued, in one transaction: 202
// {"release"}; 409 not_published. The delete: a draft, invalid, retired or uploading (abandoned)
// release, its objects removed (an app's uploaded build, with the upload in flight aborted, and
// the package's upload key, which an invalid one may keep, then its package or download once it
// has a sha; a missing object is no failure), the row gone: 204; 409 published (retire it
// first), validating or building (the data program is on it; a building one whose job ended may
// go); 502 store_error; 503 store_unavailable (no store).
crow::response publish_release_reply(
		db::Database &db, const ReleaseRow &release, const std::string &actor);
crow::response retire_release_reply(
		db::Database &db, const ReleaseRow &release, const std::string &actor);
crow::response delete_release_reply(
		db::Database &db, ObjectStore *store, const ReleaseRow &release, const std::string &actor);

class ReleaseRoutes {
public:
	static constexpr size_t kNameMax = 64;
	// One part of a package's multipart upload (S3's parts: inside Cloudflare's request body
	// limit), and the most parts a package may take (S3's own limit).
	static constexpr int64_t kPartSize = 32 * 1024 * 1024;
	static constexpr int kMaxParts = 10000;

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
