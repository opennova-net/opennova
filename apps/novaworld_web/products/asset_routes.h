#pragma once

#include "web_access.h"
#include "rate_limiter.h"
#include "core/object_store.h"
#include "core/scratch_dir.h"
#include "project_urls.h"
#include <net/novaworld/db/database.h>

namespace opennova::novaworld_server::hosted {

// Public native assets have their own authorization, independent of editor-project access.
class AssetRoutes {
public:
	AssetRoutes(WebAccess &access, db::ConnectionPool &pool, RedisPool &redis) :
			access_(access), pool_(pool), requests_(redis, {"asset_download", 20, 1.0 / 3}) {}
	void set_store(ObjectStore *store) { store_ = store; }
	void set_urls(projects::ProjectUrls *urls) { urls_ = urls; }
	void register_routes(WebApp &app);
private:
	crow::response list(const crow::request &req);
	crow::response download(const crow::request &req, int64_t id);
	crow::response file(int64_t id);
	WebAccess &access_;
	db::ConnectionPool &pool_;
	RateLimiter requests_;
	ObjectStore *store_ = nullptr;
	projects::ProjectUrls *urls_ = nullptr;
	// Local-store downloads are streamed from this listener-lifetime cache. Access is checked
	// before using it; Crow may still be sending a file after its route handler returns.
	ScratchDir downloads_;
	std::mutex downloads_mutex_;
};
} // namespace opennova::novaworld_server::hosted
