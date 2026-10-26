#pragma once

// The games' admin routes (ADR 0064; apps/novaworld_common/hosted/README.md "Games"): the data
// sets an admin creates as rows (data_sets/data_set_repository.h), each one game with, when it has
// a key, its project (data_sets/content_repository.h: one create makes the whole game, one patch
// edits it, a key rename its row alone since the store names a project's objects by the game's
// id, one delete removes it, its objects removed after by the data program's game.purge). A
// game's hosted trees are built by the data program from its published release
// (data_sets/data_set_jobs.h release.apply), never uploaded. Each behind WebAccess::require_admin
// (and ONNET_API_PROXY_ONLY like every /api route), its state change logged with its actor and
// status (AccessMiddleware):
//
//   GET    /api/admin/data-sets          the store, the titles, the games, every game with its
//                                        project's key, visibility and head and its trees
//   POST   /api/admin/data-sets          a new game (a base or an expansion, its project when
//                                        it has a key)
//   PATCH  /api/admin/data-sets/<id>     its name, lobby, updater product, project key (a
//                                        rename) and visibility
//   DELETE /api/admin/data-sets/<id>     the row gone (its trees' and its project's rows with
//                                        it), its objects' game.purge queued
//
// Every reply is an error code with our own message, never database or store text; a
// timestamp not yet set is "" (the DB text). The admin's update-all (POST
// /api/admin/hosted/update-all) sits with the fleet routes; a game's releases with the release
// routes (release_routes.h) and the content routes (content/content_routes.h: a release built
// from a revision). The HttpListener owns one DataSetRoutes beside its ReleaseRoutes.

#include "web_access.h"

#include <net/novaworld/db/database.h>

#include <crow.h>

#include <cstddef>
#include <cstdint>

namespace opennova::novaworld_server {

class ObjectStore;

namespace hosted {

struct DataSetTree; // data_sets/data_set_repository.h

// A tree as the admin's JSON: {id, state, release_id, version, bytes, files (how many),
// missions (how many), pinned_by (the live servers pinning it), error, created_by, created_at,
// ready_at ("" until ready)}.
crow::json::wvalue tree_json(db::Database &db, const DataSetTree &tree);

class DataSetRoutes {
public:
	// The shapes a data set's fields take.
	static constexpr size_t kDisplayNameMax = 64;
	static constexpr size_t kUpdaterLoginMax = 32;
	static constexpr size_t kCommandFileMax = 15;
	static constexpr size_t kExpansionCodeMax = 8;

	DataSetRoutes(WebAccess &access, db::ConnectionPool &pool) : access_(access), pool_(pool) {}

	DataSetRoutes(const DataSetRoutes &) = delete;
	DataSetRoutes &operator=(const DataSetRoutes &) = delete;

	// The deployment's object store, null while hosting is off (a delete then queues no purge:
	// with no store there are no objects). It must outlive the listener. Before
	// register_routes().
	void set_store(ObjectStore *store) { store_ = store; }
	void register_routes(WebApp &app);

private:
	crow::response list(const crow::request &req);
	crow::response create(const crow::request &req);
	crow::response patch(const crow::request &req, uint64_t id);
	crow::response remove(const crow::request &req, uint64_t id);

	WebAccess &access_;
	db::ConnectionPool &pool_;
	ObjectStore *store_ = nullptr;
};

} // namespace hosted
} // namespace opennova::novaworld_server
