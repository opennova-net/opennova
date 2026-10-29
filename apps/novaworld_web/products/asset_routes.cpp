#include "asset_routes.h"

#include "http_json.h"
#include "redis_client.h"
#include "products/asset_catalog_repository.h"
#include "core/scratch_dir.h"
#include <base/io/strutil.h>
#include <climits>
#include <cstdio>

namespace opennova::novaworld_server::hosted {
namespace {
std::string parameter(const crow::request &req, const char *name) {
	const char *value = req.url_params.get(name);
	return value ? value : "";
}
bool unsigned_number(const std::string &text, int64_t fallback, int64_t &out) {
	if (text.empty()) { out = fallback; return true; }
	uint64_t value = 0;
	for (const char c : text) {
		if (c < '0' || c > '9' || value > (uint64_t(INT64_MAX) - uint64_t(c - '0')) / 10) return false;
		value = value * 10 + uint64_t(c - '0');
	}
	out = int64_t(value);
	return true;
}
crow::json::wvalue asset_json(const LibraryAsset &a) {
	crow::json::wvalue value;
	value["id"] = a.id;
	value["name"] = a.name;
	value["kind"] = a.kind;
	value["project"]["key"] = a.project_key;
	value["project"]["name"] = a.project_name;
	text_or_null(value["project"], "owner", a.owner);
	value["release"]["id"] = a.release_id;
	value["release"]["version"] = a.version;
	value["size"] = a.size;
	value["file_count"] = a.file_count;
	value["total_bytes"] = a.total_bytes;
	return value;
}
} // namespace

void AssetRoutes::register_routes(WebApp &app) {
	CROW_ROUTE(app, "/api/assets")([this](const crow::request &req) { return list(req); });
	CROW_ROUTE(app, "/api/assets/<uint>/download").methods(crow::HTTPMethod::POST)(
			[this](const crow::request &req, uint64_t id) { return download(req, path_id(id)); });
	CROW_ROUTE(app, "/api/assets/<uint>/file")([this](uint64_t id) { return file(path_id(id)); });
}

crow::response AssetRoutes::list(const crow::request &req) {
	const std::string query = parameter(req, "q"), kind = parameter(req, "kind"), project = parameter(req, "project");
	int64_t before = 0, limit = 50;
	if (query.size() > 256 || project.size() > 128 || (!kind.empty() && !library_asset_kind(kind)) ||
			!unsigned_number(parameter(req, "before"), 0, before) ||
			!unsigned_number(parameter(req, "limit"), 50, limit) || limit < 1 || limit > 100)
		return no_store(json_error(400, "invalid_filter"));
	try {
		auto db = pool_.acquire();
		auto assets = list_library_assets(*db, query, kind, project, before, int(limit) + 1);
		const bool more = assets.size() > size_t(limit);
		if (more) assets.resize(size_t(limit));
		std::vector<crow::json::wvalue> rows;
		for (const auto &asset : assets) rows.push_back(asset_json(asset));
		crow::json::wvalue out;
		out["assets"] = std::move(rows);
		out["next_before"] = more ? crow::json::wvalue(assets.back().id) : crow::json::wvalue(nullptr);
		return no_store(json_reply(200, out));
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] asset list failed: %s\n", e.what());
		return no_store(json_error(500, "db_error"));
	}
}

crow::response AssetRoutes::download(const crow::request &req, int64_t id) {
	if (!store_) return no_store(json_error(503, "store_unavailable"));
	auto body = JsonBody::parse(req.body.empty() ? "{}" : req.body);
	if (!body) return no_store(json_error(400, "invalid_json"));
	const bool explicit_retry = body->flag("retry").value_or(false);
	if (body->wrong_type()) return no_store(json_error(400, "invalid_field"));
	try {
		std::optional<LibraryAsset> asset;
		{
			auto db = pool_.acquire();
			asset = find_library_asset(*db, id);
			if (!asset) return no_store(json_error(404, "not_found"));
			if (asset->bundle_key.empty()) {
				if (asset_bundle_pending(*db, id)) {
					crow::json::wvalue out; out["state"] = "preparing";
					return no_store(json_reply(202, out));
				}
				if (!explicit_retry && (!asset->bundle_error.empty() || asset_bundle_failed(*db, id)))
					return no_store(json_error(503, "bundle_failed", "This download could not be prepared. Please try again later."));
			}
		}
		const int64_t retry = requests_.take(access_.client_ip(req), RateLimiter::Clock::now());
		if (retry > 0) {
			auto response = json_error(429, "rate_limited");
			response.set_header("Retry-After", std::to_string(retry));
			return no_store(std::move(response));
		}
		if (asset->bundle_key.empty()) {
			auto db = pool_.acquire();
			asset = find_library_asset(*db, id);
			if (!asset) return no_store(json_error(404, "not_found"));
			if (asset->bundle_key.empty()) {
				if (!asset_bundle_pending(*db, id)) {
					if (!explicit_retry && (!asset->bundle_error.empty() || asset_bundle_failed(*db, id)))
						return no_store(json_error(503, "bundle_failed", "This download could not be prepared. Please try again later."));
					enqueue_asset_bundle(*db, id);
				}
				crow::json::wvalue out; out["state"] = "preparing";
				return no_store(json_reply(202, out));
			}
		}
		const std::string filename = asset_download_name(*asset);
		std::string url = "/api/assets/" + std::to_string(id) + "/file";
		if (urls_) {
			projects::SignedTransfer transfer;
			std::string error;
			if (!urls_->download(asset->bundle_key, filename, transfer, error))
				return no_store(json_error(503, "store_unavailable"));
			url = transfer.url;
		}
		crow::json::wvalue out;
		out["state"] = "ready"; out["url"] = url; out["filename"] = filename;
		return no_store(json_reply(200, out));
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] asset download failed: %s\n", e.what());
		return no_store(json_error(500, "db_error"));
	} catch (const RedisError &) {
		return no_store(json_error(503, "temporarily_unavailable"));
	}
}

crow::response AssetRoutes::file(int64_t id) {
	if (!store_) return no_store(json_error(503, "store_unavailable"));
	try {
		std::optional<LibraryAsset> asset;
		{
			auto db = pool_.acquire();
			asset = find_library_asset(*db, id);
		}
		if (!asset || asset->bundle_key.empty()) return no_store(json_error(404, "not_found"));
		if (urls_) return no_store(json_error(404, "not_found")); // S3 bytes go directly to the signed URL.
		std::string error;
		std::filesystem::path path;
		{
			std::lock_guard<std::mutex> lock(downloads_mutex_);
			if (downloads_.empty()) downloads_ = ScratchDir::make("opennova-asset-download", error);
			if (downloads_.empty()) return no_store(json_error(503, "store_unavailable"));
			path = downloads_.path() / (std::to_string(id) + ".zip");
			std::error_code ec;
			if (!std::filesystem::is_regular_file(path, ec) &&
					!store_->get_file(asset->bundle_key, path.u8string(), error)) {
				std::filesystem::remove(path, ec);
				return no_store(json_error(503, "store_unavailable"));
			}
		}
		crow::response response;
		// This absolute path is generated from our scratch directory and numeric ID.
		// Crow's public filename sanitizer strips the leading slash of absolute paths.
		response.set_static_file_info_unsafe(path.u8string());
		response.set_header("Content-Type", "application/zip");
		response.set_header("Content-Disposition", "attachment; filename=\"" + asset_download_name(*asset) + "\"");
		return no_store(std::move(response));
	} catch (const db::Error &) {
		return no_store(json_error(500, "db_error"));
	}
}
} // namespace opennova::novaworld_server::hosted
