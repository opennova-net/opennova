#pragma once

// The project routes (ADR 0069; products/project_repository.h,
// base/project/project_manifest.h): what the OpenNova Editor's NovaWorld window reads and pushes,
// each game's project named by its key, and the release built from a revision. A game, its
// project with it, is made, changed and deleted by the products' admin routes
// (products/product_routes.h). A public project is anyone's to read, signed in or not; a
// private one answers 404 to anyone but an admin (WebAccess::caller_is_admin). A push needs an
// admin's own session (never the Bearer ADMIN_API_TOKEN, so every revision names its author),
// and an admin's change is logged with its actor and status (AccessMiddleware):
//
//   GET    /api/products                              the projects the caller may read
//   GET    /api/products/<key>                        one, with its head revision
//   GET    /api/products/<key>/revisions?before=&limit=    its history, newest first
//   GET    /api/products/<key>/revisions/<n>/manifest      a revision's manifest text
//   GET    /api/products/<key>/blobs/<sha256>         one file's bytes
//   POST   /api/products/<key>/downloads              {hashes}: each file's download
//   POST   /api/products/<key>/missing                {blobs: [{sha256, size}]}: the ones to
//                                                     upload, each with its upload
//   PUT    /api/products/<key>/blobs/<sha256>         one file's bytes, checked by its sha
//   POST   /api/products/<key>/revisions              {parent, message, manifest}: a push
//   POST   /api/admin/products/<key>/releases         {version, revision?, base_revision?}: a
//                                                     release built from a revision (building;
//                                                     products/release_build.h)
//
// A file moves straight between the editor and the object store where the store signs URLs of its
// own (S3's presigned GET and PUT, project_urls.h, ADR 0069 d6): a download and an upload are then
// the store's URL with the headers it names; with none (a directory's store) they are this
// program's own blob routes, the bytes in the request's or the reply's body. A push names the head
// it was made on: 409 {error: "stale", head} when the head moved; 400 missing_blobs {hashes} when
// the store does not hold a file its manifest lists at its size; 400 unchanged when it lists the
// head's files. Every reply is an error code with our own message, never database or store text.
// Without a store (hosting off) every route that reads or writes the store's objects answers 503
// store_unavailable. The HttpListener owns one ProjectRoutes beside its ReleaseRoutes.

#include "web_access.h"

#include "core/scratch_dir.h"

#include <net/novaworld/db/database.h>

#include <crow.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace opennova::novaworld_server {

class ObjectStore;

namespace projects {

class ProjectUrls; // project_urls.h

class ProjectRoutes {
public:
	// A revision's message, in bytes.
	static constexpr size_t kMessageMax = 2000;
	// The files one missing request may name, and one manifest list.
	static constexpr size_t kMissingMax = 20000;
	// The files one downloads request may name.
	static constexpr size_t kDownloadsMax = 1000;
	// The most files a push asks the store of one by one; more, by one listing of the project's.
	static constexpr size_t kHeadsAtMost = 4;
	// A history page's revisions: the default and the most.
	static constexpr int kPageDefault = 50;
	static constexpr int kPageMax = 200;
	// The most a file's bytes may be through this program's own blob routes (a directory's store,
	// which signs no URLs: the local stack's and the tests'), each held in memory once: the cap
	// the portal's nginx puts on every /api/ body (web/nginx.conf, client_max_body_size 32m), so
	// the route refuses (413 too_large) exactly the sizes the proxy in front of it would. The S3
	// store's transfers go straight to the store and take no cap here.
	static constexpr uint64_t kBodyMax = uint64_t(32) << 20;

	ProjectRoutes(WebAccess &access, db::ConnectionPool &pool) : access_(access), pool_(pool) {}

	ProjectRoutes(const ProjectRoutes &) = delete;
	ProjectRoutes &operator=(const ProjectRoutes &) = delete;

	// The deployment's object store, null while hosting is off; it must outlive the listener.
	// Before register_routes().
	void set_store(ObjectStore *store) { store_ = store; }
	// The store's own signed URLs (S3's), which the routes hand out in place of carrying a file's
	// bytes; null (a directory's store) carries them. Before register_routes().
	void set_urls(ProjectUrls *urls) { urls_ = urls; }
	void register_routes(WebApp &app);

private:
	crow::response list(const crow::request &req);
	crow::response project(const crow::request &req, const std::string &key);
	crow::response revisions(const crow::request &req, const std::string &key);
	crow::response manifest(const crow::request &req, const std::string &key, uint64_t number);
	crow::response blob(const crow::request &req, const std::string &key, const std::string &sha);
	crow::response missing(const crow::request &req, const std::string &key);
	crow::response downloads(const crow::request &req, const std::string &key);
	crow::response upload(const crow::request &req, const std::string &key, const std::string &sha);
	crow::response push(const crow::request &req, const std::string &key);
	crow::response release(const crow::request &req, const std::string &key);

	// A file in the scratch directory for one request's bytes, its name its own; "" with
	// `error` when there is no scratch directory.
	std::string scratch_file(const char *what, std::string &error);

	WebAccess &access_;
	db::ConnectionPool &pool_;
	ObjectStore *store_ = nullptr;
	ProjectUrls *urls_ = nullptr;
	ScratchDir scratch_;
	std::atomic<uint64_t> next_scratch_{ 0 };
};

} // namespace projects
} // namespace opennova::novaworld_server
