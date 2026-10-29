#include "asset_catalog_repository.h"

#include "core/scratch_dir.h"
#include "core/zip_files.h"
#include "jobs/job_queue.h"
#include "product_repository.h"
#include <base/io/file_io.h>
#include <base/io/json.h>
#include <base/io/os_path.h>
#include <base/io/sha256.h>
#include <base/io/strutil.h>
#include <base/project/project_manifest.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>

namespace opennova::novaworld_server::hosted {
namespace {
using io::JsonValue;
namespace fs = std::filesystem;

const std::string kSelect =
	"SELECT a.id,s.product_id,a.release_id,a.name,a.kind,a.sha256,a.size,a.file_count,"
	"a.total_bytes,a.bundle_key,a.bundle_error,p.key,p.display_name,COALESCE(u.username,''),r.version "
	"FROM asset_entries a JOIN asset_snapshots s ON s.release_id=a.release_id "
	"JOIN releases r ON r.id=a.release_id JOIN products p ON p.id=s.product_id "
	"LEFT JOIN players u ON u.id=p.owner_id ";
const std::string kPublic =
	"p.assets_public AND r.status='published' AND NOT EXISTS "
	"(SELECT 1 FROM asset_files f LEFT JOIN products source ON source.id=f.product_id "
	"WHERE f.asset_id=a.id AND (source.id IS NULL OR NOT source.assets_public))";

LibraryAsset read_asset(const db::Row &r) {
	LibraryAsset a;
	a.id = r.as_int(0).value_or(0);
	a.product_id = r.as_int(1).value_or(0);
	a.release_id = r.as_int(2).value_or(0);
	a.name = r.as_text(3).value_or("");
	a.kind = r.as_text(4).value_or("");
	a.sha256 = r.as_text(5).value_or("");
	a.size = r.as_int(6).value_or(0);
	a.file_count = r.as_int(7).value_or(0);
	a.total_bytes = r.as_int(8).value_or(0);
	a.bundle_key = r.as_text(9).value_or("");
	a.bundle_error = r.as_text(10).value_or("");
	a.project_key = r.as_text(11).value_or("");
	a.project_name = r.as_text(12).value_or("");
	a.owner = r.as_text(13).value_or("");
	a.version = r.as_text(14).value_or("");
	return a;
}

bool native_name(const std::string &name) {
	std::string why;
	return name.find('/') == std::string::npos && project::is_project_path(name, why);
}
bool read_size(const JsonValue &v, int64_t &out) {
	const auto *size = v.get("size");
	if (!size || !size->is_number() || !std::isfinite(size->number) || size->number < 0 ||
			size->number > double(project::kMaxFileBytes) || std::floor(size->number) != size->number)
		return false;
	out = static_cast<int64_t>(size->number);
	return true;
}
bool verify_blob(const std::string &path, int64_t expected, const std::string &sha) {
	FILE *file = io::fopen_utf8(path.c_str(), "rb");
	if (!file) return false;
	io::Sha256 hash;
	std::array<uint8_t, 65536> bytes{};
	uint64_t size = 0;
	size_t count = 0;
	while ((count = std::fread(bytes.data(), 1, bytes.size(), file)) != 0) {
		hash.update(bytes.data(), count);
		size += count;
	}
	const bool ok = std::ferror(file) == 0 && size == uint64_t(expected);
	std::fclose(file);
	return ok && io::sha256_hex(hash.finish()) == sha;
}
struct ParsedAsset {
	LibraryAsset asset;
	std::vector<LibraryFile> files;
};

std::string bundle_job_key(int64_t id) { return "asset.bundle:" + std::to_string(id); }

jobs::JobResult bundle(jobs::JobContext &ctx, ObjectStore &store) {
	const int64_t id = jobs::member_int(ctx.job.payload, "asset_id");
	std::optional<LibraryAsset> asset;
	std::vector<LibraryFile> files;
	int64_t revision = 0, base_revision = 0;
	{
		auto db = ctx.pool.acquire();
		asset = find_library_asset(*db, id);
		if (!asset || !asset->bundle_key.empty()) return jobs::JobResult::done();
		const auto release = find_release(*db, asset->release_id);
		if (!release) return jobs::JobResult::done();
		revision = release->revision;
		base_revision = release->base_revision;
		files = library_asset_files(*db, id);
	}
	std::string error;
	const auto fail = [&](const std::string &why) {
		if (ctx.job.attempts >= ctx.job.max_attempts) {
			auto db = ctx.pool.acquire();
			db->exec("UPDATE asset_entries SET bundle_error=$2 WHERE id=$1 AND bundle_key=''", {id, why});
		}
		return jobs::JobResult::retry(why);
	};
	ScratchDir scratch = ScratchDir::make("opennova-asset-bundle", error);
	if (scratch.empty()) return fail(error);
	const std::string dir = (scratch.path() / "bundle").u8string();
	if (!io::ensure_directory(io::utf8_join(dir, "files"), error)) return fail(error);
	JsonValue manifest = JsonValue::make_object();
	manifest.set("schema_version", JsonValue::make_number(1));
	manifest.set("project", JsonValue::make_string(asset->project_key));
	manifest.set("release_id", JsonValue::make_number(double(asset->release_id)));
	manifest.set("revision", JsonValue::make_number(double(revision)));
	manifest.set("base_revision", JsonValue::make_number(double(base_revision)));
	manifest.set("version", JsonValue::make_string(asset->version));
	manifest.set("asset", JsonValue::make_string(asset->name));
	JsonValue members = JsonValue::make_array();
	for (const LibraryFile &f : files) {
		const std::string dest = io::utf8_join(dir, "files/" + f.name);
		if (!store.get_file(asset_blob_key(f.product_id, f.sha256), dest, error)) return fail(error);
		if (!verify_blob(dest, f.size, f.sha256)) return fail("asset file failed its integrity check");
		JsonValue member = JsonValue::make_object();
		member.set("name", JsonValue::make_string(f.name));
		member.set("sha256", JsonValue::make_string(f.sha256));
		member.set("size", JsonValue::make_number(double(f.size)));
		member.set("product_id", JsonValue::make_number(double(f.product_id)));
		members.array.push_back(std::move(member));
	}
	manifest.set("files", std::move(members));
	if (!io::write_file_atomic(io::utf8_join(dir, "manifest.json"), io::json_write(manifest), error))
		return fail(error);
	const std::string zip = (scratch.path() / "bundle.zip").u8string();
	if (!zip_directory(dir, zip, error)) return fail(error);
	const std::string key = "assets/" + std::to_string(asset->product_id) + "/bundles/" +
			std::to_string(asset->release_id) + "/" + std::to_string(id) + "/" + asset_download_name(*asset);
	if (!store.put_file(key, zip, error, {"application/zip", "private, no-store"})) return fail(error);
	{
		auto db = ctx.pool.acquire();
		// The artifact remains private. Every URL request independently checks sharing again.
		db->exec("UPDATE asset_entries SET bundle_key=$2,bundle_error='' WHERE id=$1", {id, key});
	}
	return jobs::JobResult::done();
}
} // namespace

bool library_asset_kind(const std::string &kind) {
	return kind == "model" || kind == "animation" || kind == "animation_map" ||
			kind == "terrain" || kind == "texture";
}
std::string asset_blob_key(int64_t product_id, const std::string &sha256) {
	return "assets/" + std::to_string(product_id) + "/blobs/" + sha256;
}
std::string asset_download_name(const LibraryAsset &asset) {
	std::string name = asset.project_key + "-r" + std::to_string(asset.release_id) + "-" + asset.name;
	for (char &c : name)
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
				(c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')) c = '_';
	// A valid native name can already occupy a filesystem's whole 255-byte component.
	// Reserve room for the ZIP suffix; only its download name changes, never its members.
	if (name.size() > 251) {
		const size_t dot = name.find_last_of('.');
		const std::string extension = dot != std::string::npos && name.size() - dot <= 8
				? name.substr(dot) : std::string{};
		name.resize(251 - extension.size());
		name += extension;
	}
	return name + ".zip";
}

bool store_asset_snapshot(db::Database &db, ObjectStore &store, int64_t release_id,
		const std::string &directory, std::string &error) {
	const auto release = find_release(db, release_id);
	if (!release || !release->product_id) { error = "release has no project"; return false; }
	const auto product = find_product(db, *release->product_id);
	if (!product || product->kind == ProductKind::App) { error = "release has no native assets"; return false; }
	std::string text;
	JsonValue manifest;
	if (!io::read_file_text(io::utf8_join(directory, "manifest.json"), text, error) ||
			!io::json_parse(text, manifest, error)) return false;
	const auto *assets = manifest.get("assets");
	const auto *schema = manifest.get("schema_version");
	if (!schema || !schema->is_number() || schema->number != 1 || !assets || !assets->is_array() ||
			assets->array.size() > 20000) { error = "invalid asset snapshot"; return false; }
	const std::string digest = io::sha256_hex(text.data(), text.size());
	const auto existing = db.query("SELECT manifest_sha256 FROM asset_snapshots WHERE release_id=$1", {release_id});
	if (!existing.empty()) {
		if (existing[0].as_text(0).value_or("") == digest) return true;
		error = "a release's asset snapshot cannot change";
		return false;
	}
	std::vector<ParsedAsset> parsed;
	std::set<std::string> names;
	std::map<std::pair<int64_t, std::string>, int64_t> blobs;
	for (const auto &item : assets->array) {
		ParsedAsset row;
		auto &a = row.asset;
		a.name = item.get_string("name", "");
		a.kind = item.get_string("kind", "");
		a.sha256 = item.get_string("sha256", "");
		const auto *files = item.get("files");
		if (!native_name(a.name) || !names.insert(project::folded_path(a.name)).second ||
				!library_asset_kind(a.kind) || !project::is_sha256_hex(a.sha256) || !read_size(item, a.size) ||
				!files || !files->is_array() || files->array.empty() || files->array.size() > 20000) {
			error = "invalid asset entry"; return false;
		}
		std::set<std::string> members;
		bool root = false;
		for (const auto &value : files->array) {
			LibraryFile f;
			f.name = value.get_string("name", "");
			f.sha256 = value.get_string("sha256", "");
			const std::string origin = value.get_string("origin", "");
			f.product_id = origin == "project" ? product->id :
					(origin == "base" && release->base_revision > 0 ? product->parent_id.value_or(0) : 0);
			if (!native_name(f.name) || !members.insert(project::folded_path(f.name)).second ||
					!project::is_sha256_hex(f.sha256) || !read_size(value, f.size) || f.product_id == 0) {
				error = "invalid asset dependency"; return false;
			}
			root = root || (f.name == a.name && f.sha256 == a.sha256 && f.size == a.size && origin == "project");
			const auto key = std::make_pair(f.product_id, f.sha256);
			if (auto found = blobs.find(key); found != blobs.end() && found->second != f.size) {
				error = "inconsistent asset file size"; return false;
			}
			blobs[key] = f.size;
			a.total_bytes += f.size;
			row.files.push_back(std::move(f));
		}
		if (!root) { error = "asset closure does not contain its own file"; return false; }
		a.file_count = int64_t(row.files.size());
		parsed.push_back(std::move(row));
	}
	for (const auto &[key, size] : blobs) {
		const std::string path = io::utf8_join(directory, "blobs/" + key.second);
		if (!verify_blob(path, size, key.second)) { error = "asset blob failed its integrity check"; return false; }
		bool exists = false;
		uint64_t stored_size = 0;
		const auto object = asset_blob_key(key.first, key.second);
		if (!store.head(object, exists, stored_size, error)) return false;
		if ((!exists || stored_size != uint64_t(size)) && !store.put_file(object, path, error)) return false;
	}
	db::Transaction tx(db);
	// Hold the release against concurrent deletion and serialize two attempts of the same build.
	if (db.query("SELECT id FROM releases WHERE id=$1 FOR UPDATE", {release_id}).empty()) {
		error = "release was removed"; return false;
	}
	const auto held = db.query("SELECT manifest_sha256 FROM asset_snapshots WHERE release_id=$1", {release_id});
	if (!held.empty()) {
		if (held[0].as_text(0).value_or("") != digest) { error = "a release's asset snapshot cannot change"; return false; }
		tx.commit(); return true;
	}
	db.exec("INSERT INTO asset_snapshots(release_id,product_id,manifest_sha256) VALUES($1,$2,$3)",
			{release_id, product->id, digest});
	for (const auto &row : parsed) {
		const auto &a = row.asset;
		const auto inserted = db.query("INSERT INTO asset_entries(release_id,name,kind,sha256,size,file_count,total_bytes) "
				"VALUES($1,$2,$3,$4,$5,$6,$7) RETURNING id", {release_id, a.name, a.kind, a.sha256, a.size, a.file_count, a.total_bytes});
		const int64_t id = inserted[0].as_int(0).value_or(0);
		for (const auto &f : row.files)
			db.exec("INSERT INTO asset_files(asset_id,name,sha256,size,product_id) VALUES($1,$2,$3,$4,$5)",
					{id, f.name, f.sha256, f.size, f.product_id});
	}
	tx.commit();
	return true;
}

std::vector<LibraryAsset> list_library_assets(db::Database &db, const std::string &query,
		const std::string &kind, const std::string &project, int64_t before, int limit) {
	const auto rows = db.query(kSelect + "WHERE " + kPublic +
			" AND p.published_release_id=r.id AND ($1::text='' OR strpos(lower(a.name),lower($1))>0) "
			"AND ($2::text='' OR a.kind=$2) AND ($3::text='' OR p.key=$3) "
			"AND ($4::bigint=0 OR a.id<$4) ORDER BY a.id DESC LIMIT $5",
			{query, kind, project, before, int64_t(std::clamp(limit, 1, 101))});
	std::vector<LibraryAsset> out;
	for (const auto &row : rows) out.push_back(read_asset(row));
	return out;
}
std::optional<LibraryAsset> find_library_asset(db::Database &db, int64_t id) {
	const auto rows = db.query(kSelect + "WHERE a.id=$1 AND " + kPublic, {id});
	return rows.empty() ? std::nullopt : std::optional<LibraryAsset>(read_asset(rows[0]));
}
std::vector<LibraryFile> library_asset_files(db::Database &db, int64_t id) {
	std::vector<LibraryFile> out;
	for (const auto &row : db.query("SELECT name,sha256,size,product_id FROM asset_files WHERE asset_id=$1 ORDER BY name", {id}))
		out.push_back({row.as_text(0).value_or(""), row.as_text(1).value_or(""), row.as_int(2).value_or(0), row.as_int(3).value_or(0)});
	return out;
}
bool asset_bundle_pending(db::Database &db, int64_t id) { return jobs::job_pending(db, bundle_job_key(id)); }
bool asset_bundle_failed(db::Database &db, int64_t id) {
	const auto rows = db.query("SELECT state FROM jobs WHERE kind='asset.bundle' AND dedupe_key=$1 ORDER BY id DESC LIMIT 1",
			{bundle_job_key(id)});
	return !rows.empty() && rows.front().as_text(0).value_or("") == "failed";
}
int64_t enqueue_asset_bundle(db::Database &db, int64_t id) {
	db::Transaction tx(db);
	jobs::NewJob job;
	job.kind = kJobAssetBundle;
	job.dedupe_key = bundle_job_key(id);
	job.payload.set("asset_id", JsonValue::make_number(double(id)));
	const int64_t queued = jobs::enqueue(db, job);
	if (queued)
		db.exec("UPDATE asset_entries SET bundle_error='' WHERE id=$1 AND bundle_key=''", {id});
	tx.commit();
	return queued;
}
void register_asset_bundle(jobs::JobRegistry &registry, ObjectStore &store) {
	registry.add({kJobAssetBundle, [&store](jobs::JobContext &ctx) { return bundle(ctx, store); }, 2});
}
} // namespace opennova::novaworld_server::hosted
