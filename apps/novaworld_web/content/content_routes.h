#pragma once

// The content store's routes (ADR 0069; data_sets/content_repository.h,
// base/content/content_manifest.h): what the OpenNova Editor's Content window reads and pushes,
// and the admin's management of the projects. A public project is anyone's to read, signed in or
// not; a private one answers 404 to anyone but an admin (WebAccess::caller_is_admin). A push needs
// an admin's own session (never the Bearer ADMIN_API_TOKEN, so every revision names its author),
// and an admin's change is logged with its actor and status (AccessMiddleware):
//
//   GET    /api/content/projects                             the projects the caller may read
//   GET    /api/content/projects/<key>                       one, with its head revision
//   GET    /api/content/projects/<key>/revisions?before=&limit=   its history, newest first
//   GET    /api/content/projects/<key>/revisions/<n>/manifest     a revision's manifest text
//   GET    /api/content/projects/<key>/blobs/<sha256>        one file's bytes
//   POST   /api/content/projects/<key>/downloads             {hashes}: each file's download
//   POST   /api/content/projects/<key>/missing               {blobs: [{sha256, size}]}: the ones
//                                                            to upload, each with its upload
//   PUT    /api/content/projects/<key>/blobs/<sha256>        one file's bytes, checked by its sha
//   POST   /api/content/projects/<key>/revisions             {parent, message, manifest}: a push
//   POST   /api/admin/content/projects                       {key, title, visibility, retail?,
//                                                            data_set_id?, base?}: a new project
//   PATCH  /api/admin/content/projects/<key>                 {title?, visibility?}
//   DELETE /api/admin/content/projects/<key>                 a project with its objects gone
//
// A file moves straight between the editor and the object store where the store signs URLs of its
// own (S3's presigned GET and PUT, content_urls.h, ADR 0069 d6): a download and an upload are then
// the store's URL with the headers it names; with none (a directory's store) they are this
// program's own blob routes, the bytes in the request's or the reply's body. A push names the head
// it was made on: 409 {error: "stale", head} when the head moved; 400 missing_blobs {hashes} when
// the store does not hold a file its manifest lists at its size; 400 unchanged when it lists the
// head's files. Every reply is an error code with our own message, never database or store text.
// Without a store (hosting off) every route but the project list and the admin's writes answers 503
// store_unavailable. The HttpListener owns one ContentRoutes beside its ReleaseRoutes.

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

namespace content {

class ContentUrls; // content_urls.h

class ContentRoutes {
public:
	// A project's title and a revision's message, in characters and bytes.
	static constexpr size_t kTitleMax = 64;
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
	// The most a file's bytes may be through this program (each held in memory once).
	static constexpr uint64_t kBodyMax = uint64_t(512) << 20;

	ContentRoutes(WebAccess &access, db::ConnectionPool &pool) : access_(access), pool_(pool) {}

	ContentRoutes(const ContentRoutes &) = delete;
	ContentRoutes &operator=(const ContentRoutes &) = delete;

	// The deployment's object store, null while hosting is off; it must outlive the listener.
	// Before register_routes().
	void set_store(ObjectStore *store) { store_ = store; }
	// The store's own signed URLs (S3's), which the routes hand out in place of carrying a file's
	// bytes; null (a directory's store) carries them. Before register_routes().
	void set_urls(ContentUrls *urls) { urls_ = urls; }
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
	crow::response create(const crow::request &req);
	crow::response patch(const crow::request &req, const std::string &key);
	crow::response remove(const crow::request &req, const std::string &key);

	// A file in the scratch directory for one request's bytes, its name its own; "" with
	// `error` when there is no scratch directory.
	std::string scratch_file(const char *what, std::string &error);

	WebAccess &access_;
	db::ConnectionPool &pool_;
	ObjectStore *store_ = nullptr;
	ContentUrls *urls_ = nullptr;
	ScratchDir scratch_;
	std::atomic<uint64_t> next_scratch_{ 0 };
};

} // namespace content
} // namespace opennova::novaworld_server
