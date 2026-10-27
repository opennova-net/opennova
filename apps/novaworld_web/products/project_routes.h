#pragma once

// The project routes (ADR 0069, ADR 0074; products/project_repository.h,
// base/project/project_manifest.h): what the OpenNova Editor's NovaWorld window reads and pushes,
// each game's project named by its key; an account's own projects (a mod, or a game, which any
// signed-in account may make, push, release and publish); the releases built from a revision; and
// the mods a game's players may install. Who may do what is one rule, every route's
// (project_repository.h project_rights): a public project is anyone's to read, signed in or not, a
// private one its owner's and an admin's alone (404 to anyone else, the same as no project); a
// push and a release need the own session of its owner or an admin (never the Bearer
// ADMIN_API_TOKEN, so every revision and release names its author); a change of its name or its
// visibility, and its delete, its owner or an admin. A game of the service's own (no owner) is
// made, changed and deleted by the products' admin routes (products/product_routes.h), which
// change an account's own too; an admin's change there is logged with its actor and status
// (AccessMiddleware), and so is an admin's change here of a project not its own
// (WebAccess::note_admin_actor), an owner's by the route:
//
//   GET    /api/products                              the projects the caller may read
//   POST   /api/products                              {key, display_name, kind, game?, base?,
//                                                     expansion_code?, visibility}: a project of
//                                                     the caller's own
//   GET    /api/products/<key>                        one, with its head revision
//   PATCH  /api/products/<key>                        {display_name?, visibility?}
//   DELETE /api/products/<key>                        the project, its objects' purge queued
//   GET    /api/me/projects                           the caller's own, with their usage and limits
//   GET    /api/products/<key>/revisions?before=&limit=    its history, newest first
//   GET    /api/products/<key>/revisions/<n>/manifest      a revision's manifest text
//   GET    /api/products/<key>/blobs/<sha256>         one file's bytes
//   POST   /api/products/<key>/downloads              {hashes}: each file's download
//   POST   /api/products/<key>/missing                {blobs: [{sha256, size}]}: the ones to
//                                                     upload, each with its upload
//   PUT    /api/products/<key>/blobs/<sha256>         one file's bytes, checked by its sha
//   POST   /api/products/<key>/revisions              {parent, message, manifest}: a push
//   GET    /api/products/<key>/releases               its releases, newest first
//   POST   /api/products/<key>/releases               {version, revision?, base_revision?}: a
//                                                     release built from a revision (building;
//                                                     products/release_build.h)
//   POST   /api/products/<key>/releases/<id>/publish  one of its releases published
//   POST   /api/products/<key>/releases/<id>/retire   retired
//   DELETE /api/products/<key>/releases/<id>          deleted
//   GET    /api/mods?game=<command file>              the mods of a game its players may install
//
// Every project reply is project_json's: {key, title, kind, game, expansion_code, visibility,
// retail, base, product_id, head, owner, can_push, can_manage, can_release, release ({id,
// version} of its published release, or null), storage?} (project_routes.cpp). An account's own
// project's key is at most 11 characters with no QQ, and names its updater product (updater_login
// KEY, command_file KEY.CMD) and a public game's download (player-<key>-game-windows.zip); a game
// of its gets its own lobby at its first publish (product_repository.h publish_release). An
// account's own projects are at most hosted::kOwnedProjectsMax (10), holding at most
// hosted::kOwnedBytesMax (4 GiB) in their stores and their releases' packages together
// (product_repository.h; an admin's own take neither limit): a create past the first is 403
// too_many_projects, an upload past the second 413 quota_exceeded {used, limit}, whoever pushes
// it. An account that is no admin has one release building at a time (409 release_in_progress)
// and starts at most kReleasesPerDay a UTC day (429 too_many_releases), none while its usage is
// at the quota (413 quota_exceeded). A file moves straight between the editor and the object store
// where the store signs URLs of its own (S3's presigned GET and PUT, project_urls.h, ADR 0069 d6):
// a download and an upload are then the store's URL with the headers it names; with none (a
// directory's store) they are this program's own blob routes, the bytes in the request's or the
// reply's body. A push names the head it was made on: 409 {error: "stale", head} when the head
// moved; 400 missing_blobs {hashes} when the store does not hold a file its manifest lists at its
// size; 400 unchanged when it lists the head's files. Every reply is an error code with our own
// message, never database or store text. Without a store (hosting off) every route that reads or
// writes the store's objects answers 503 store_unavailable. The HttpListener owns one
// ProjectRoutes beside its ReleaseRoutes.

#include "web_access.h"

#include "core/scratch_dir.h"
#include "rate_limiter.h"

#include <net/novaworld/db/database.h>

#include <crow.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace opennova::novaworld_server {

class ObjectStore;
class RedisPool;

namespace hosted {
struct ProductRow; // products/product_repository.h
}

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
	// An account's project creates: 5 at once, then one a minute (service policy: each is a row
	// and a store prefix, and an account holds at most 10).
	static constexpr RateLimiter::Params kCreatePerUser{ "project_create", 5, 1.0 / 60 };
	// The releases an account that is no admin starts a UTC day (service policy: the data program
	// builds one release at a time for the whole service, each up to an hour).
	static constexpr int64_t kReleasesPerDay = 5;

	ProjectRoutes(WebAccess &access, db::ConnectionPool &pool, RedisPool &redis) :
			access_(access), pool_(pool), create_per_user_(redis, kCreatePerUser) {}

	ProjectRoutes(const ProjectRoutes &) = delete;
	ProjectRoutes &operator=(const ProjectRoutes &) = delete;

	// The deployment's object store, null while hosting is off (a delete then queues no purge:
	// with no store there are no objects); it must outlive the listener. Before
	// register_routes().
	void set_store(ObjectStore *store) { store_ = store; }
	// The store's own signed URLs (S3's), which the routes hand out in place of carrying a file's
	// bytes; null (a directory's store) carries them. Before register_routes().
	void set_urls(ProjectUrls *urls) { urls_ = urls; }
	void register_routes(WebApp &app);

private:
	crow::response list(const crow::request &req);
	crow::response create(const crow::request &req);
	crow::response project(const crow::request &req, const std::string &key);
	crow::response update(const crow::request &req, const std::string &key);
	crow::response remove(const crow::request &req, const std::string &key);
	crow::response mine(const crow::request &req);
	crow::response revisions(const crow::request &req, const std::string &key);
	crow::response manifest(const crow::request &req, const std::string &key, uint64_t number);
	crow::response blob(const crow::request &req, const std::string &key, const std::string &sha);
	crow::response missing(const crow::request &req, const std::string &key);
	crow::response downloads(const crow::request &req, const std::string &key);
	crow::response upload(const crow::request &req, const std::string &key, const std::string &sha);
	crow::response push(const crow::request &req, const std::string &key);
	crow::response releases(const crow::request &req, const std::string &key);
	crow::response release(const crow::request &req, const std::string &key);
	enum class ReleaseChange : uint8_t { Publish, Retire, Delete };
	crow::response release_change(
			const crow::request &req, const std::string &key, uint64_t id, ReleaseChange change);
	crow::response mods(const crow::request &req);

	// An admin's session acting on a project not its own: its state change logged with its actor
	// (WebAccess::note_admin_actor).
	void audit(const crow::request &req, const hosted::ProductRow &project,
			const WebSessionUser &user);

	// A push's caller (the missing, upload and push routes): its own session under
	// require_user's rules; the Bearer token alone, a machine's, names no author (403
	// session_required).
	RouteAccess pusher(const crow::request &req);

	// A file in the scratch directory for one request's bytes, its name its own; "" with
	// `error` when there is no scratch directory.
	std::string scratch_file(const char *what, std::string &error);

	WebAccess &access_;
	db::ConnectionPool &pool_;
	RateLimiter create_per_user_;
	ObjectStore *store_ = nullptr;
	ProjectUrls *urls_ = nullptr;
	ScratchDir scratch_;
	std::atomic<uint64_t> next_scratch_{ 0 };
};

} // namespace projects
} // namespace opennova::novaworld_server
