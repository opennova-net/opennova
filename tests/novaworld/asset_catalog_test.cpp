// Native release snapshots on a migrated database and local object store. The manifests and
// bytes are synthetic: this tests catalog publication, per-project variants, sharing at every
// contributing project, and usable ZIP closures independently of the native-format extractor.
#include "common/temp_dir.h"
#include "common/test_database.h"
#include "common/test_expect.h"

#include "core/local_object_store.h"
#include "core/zip_files.h"
#include "products/asset_catalog_repository.h"
#include "products/product_repository.h"

#include <base/io/file_io.h>
#include <base/io/json.h>
#include <base/io/os_path.h>
#include <base/io/sha256.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace io = opennova::io;
namespace db = opennova::db;
using namespace opennova::novaworld_server;
using namespace opennova::novaworld_server::hosted;

namespace {

struct NativeFile {
	std::string name, bytes, origin = "project";
	std::string sha() const { return io::sha256_hex(bytes.data(), bytes.size()); }
	io::JsonValue json() const {
		auto value = io::JsonValue::make_object();
		value.set("name", io::JsonValue::make_string(name));
		value.set("sha256", io::JsonValue::make_string(sha()));
		value.set("size", io::JsonValue::make_number(double(bytes.size())));
		value.set("origin", io::JsonValue::make_string(origin));
		return value;
	}
};

io::JsonValue asset(const char *kind, const std::vector<NativeFile> &files) {
	auto value = files.front().json();
	value.set("kind", io::JsonValue::make_string(kind));
	auto members = io::JsonValue::make_array();
	for (const auto &file : files) members.push(file.json());
	value.set("files", std::move(members));
	return value;
}

io::JsonValue manifest(std::vector<io::JsonValue> assets) {
	auto value = io::JsonValue::make_object();
	value.set("schema_version", io::JsonValue::make_number(1));
	auto members = io::JsonValue::make_array();
	members.array = std::move(assets);
	value.set("assets", std::move(members));
	return value;
}

struct Rig {
	test_temp::TempDir temp;
	test_db::TestDatabase scratch;
	db::ConnectionPool pool;
	db::Database db;
	LocalObjectStore store;
	jobs::JobRegistry registry;
	std::string error;
	int staged = 0;

	explicit Rig(const char *name) :
			temp(name), pool(scratch.options()), db(scratch.open()), store(temp.file("store")) {
		register_asset_bundle(registry, store);
	}
	int64_t expansion(const char *key, int64_t base) {
		NewProduct product;
		product.key = key;
		product.display_name = std::string(key) + " project";
		product.kind = ProductKind::Expansion;
		product.parent_id = base;
		product.expansion_code = key;
		product.visibility = "private";
		product.assets_public = true;
		product.created_by = "catalog test";
		int64_t id = 0;
		return create_product(db, product, id) == ProductOutcome::Ok ? id : 0;
	}
	int64_t release(int64_t product, const char *version, int64_t base_revision = 0) {
		NewBuiltRelease release;
		release.name = "Synthetic native snapshot";
		release.product_id = product;
		release.revision = 1;
		release.base_revision = base_revision;
		release.version = version;
		release.created_by = "catalog test";
		return create_built_release(db, release);
	}
	void publish(int64_t product, int64_t release) {
		db.exec("UPDATE releases SET status='published' WHERE id=$1", {release});
		db.exec("UPDATE products SET published_release_id=$2 WHERE id=$1", {product, release});
	}
	void share(int64_t product, bool enabled) {
		db.exec("UPDATE products SET assets_public=$2 WHERE id=$1", {product, db::Bool{enabled}});
	}
	std::string stage(const io::JsonValue &value, const std::vector<NativeFile> &files) {
		const std::string dir = temp.file(("snapshot" + std::to_string(++staged)).c_str());
		if (!io::ensure_directory(io::utf8_join(dir, "blobs"), error)) return {};
		for (const auto &file : files)
			if (!io::write_file_atomic(io::utf8_join(dir, "blobs/" + file.sha()), file.bytes, error))
				return {};
		return io::write_file_atomic(io::utf8_join(dir, "manifest.json"), io::json_write(value), error)
				? dir : std::string{};
	}
	std::vector<LibraryAsset> list(const std::string &query = {}, const std::string &kind = {},
			const std::string &project = {}, int64_t before = 0, int limit = 100) {
		return list_library_assets(db, query, kind, project, before, limit);
	}
	int64_t only_asset(int64_t release) {
		const auto rows = db.query("SELECT id FROM asset_entries WHERE release_id=$1", {release});
		return rows.size() == 1 ? rows.front().as_int(0).value_or(0) : 0;
	}
};

int test_download_names() {
	LibraryAsset asset;
	asset.project_key = "LONG_PROJECT_KEY";
	asset.release_id = INT64_MAX;
	asset.name = std::string(251, 'a') + ".3di";
	const std::string filename = asset_download_name(asset);
	TEST_EXPECT(filename.size() == 255);
	TEST_EXPECT(filename.substr(filename.size() - 8) == ".3di.zip");
	TEST_EXPECT(filename.rfind("LONG_PROJECT_KEY-r9223372036854775807-", 0) == 0);
	TEST_EXPECT(asset.name.size() == 255);
	return 0;
}

int test_variants_and_sharing() {
	Rig r("asset-catalog-variants");
	const auto base = find_product_by_key(r.db, "JO");
	TEST_EXPECT(base && base->visibility == "private");
	r.share(base->id, true);
	const int64_t mod_a = r.expansion("VARIANTA", base->id);
	const int64_t mod_b = r.expansion("VARIANTB", base->id);
	TEST_EXPECT(mod_a && mod_b);
	const NativeFile model{"foo.3di", "identical native model bytes"};
	const NativeFile red{"skin.dds", "red texture variant"};
	const NativeFile blue{"skin.dds", "blue texture variant"};
	const NativeFile common{"common.dds", "pinned base texture", "base"};
	const std::vector<NativeFile> files_a{model, red, common}, files_b{model, blue, common};
	const int64_t release_a = r.release(mod_a, "1.0", 7), release_b = r.release(mod_b, "2.0", 9);
	const auto dir_a = r.stage(manifest({asset("model", files_a)}), files_a);
	const auto dir_b = r.stage(manifest({asset("model", files_b)}), files_b);
	TEST_EXPECT(!dir_a.empty() && !dir_b.empty());
	TEST_EXPECT(store_asset_snapshot(r.db, r.store, release_a, dir_a, r.error));
	TEST_EXPECT(store_asset_snapshot(r.db, r.store, release_b, dir_b, r.error));
	const int64_t id_a = r.only_asset(release_a), id_b = r.only_asset(release_b);
	TEST_EXPECT(id_a && id_b && id_a != id_b);
	TEST_EXPECT(r.list().empty() && !find_library_asset(r.db, id_a));
	r.publish(mod_a, release_a);
	r.publish(mod_b, release_b);
	const auto all = r.list();
	TEST_EXPECT(all.size() == 2 && all[0].id == id_b && all[1].id == id_a);
	TEST_EXPECT(all[0].sha256 == all[1].sha256 && all[0].name == all[1].name);
	TEST_EXPECT(all[0].project_key == "VARIANTB" && all[1].project_key == "VARIANTA");
	TEST_EXPECT(all[0].version == "2.0" && all[1].version == "1.0");
	TEST_EXPECT(all[1].file_count == 3 && all[1].total_bytes ==
			int64_t(model.bytes.size() + red.bytes.size() + common.bytes.size()));
	const auto closure_a = library_asset_files(r.db, id_a), closure_b = library_asset_files(r.db, id_b);
	TEST_EXPECT(closure_a.size() == 3 && closure_b.size() == 3);
	TEST_EXPECT(closure_a[0].name == "common.dds" && closure_a[0].product_id == base->id);
	TEST_EXPECT(closure_a[2].name == "skin.dds" && closure_a[2].sha256 == red.sha());
	TEST_EXPECT(closure_b[2].sha256 == blue.sha() && closure_b[2].product_id == mod_b);
	TEST_EXPECT(r.list("FOO", "model", "VARIANTA").size() == 1);
	TEST_EXPECT(r.list("missing").empty() && r.list({}, "terrain").empty());
	const auto first = r.list({}, {}, {}, 0, 1), second = r.list({}, {}, {}, id_b, 1);
	TEST_EXPECT(first.size() == 1 && first[0].id == id_b);
	TEST_EXPECT(second.size() == 1 && second[0].id == id_a);
	TEST_EXPECT(r.list({}, {}, {}, id_a).empty());

	// Source access and library sharing are independent, including at a base contributor.
	TEST_EXPECT(find_product(r.db, mod_a)->visibility == "private");
	r.db.exec("UPDATE products SET visibility='public' WHERE id=$1", {mod_a});
	r.share(mod_a, false);
	TEST_EXPECT(r.list().size() == 1 && !find_library_asset(r.db, id_a));
	r.share(mod_a, true);
	r.share(base->id, false);
	TEST_EXPECT(r.list().empty() && !find_library_asset(r.db, id_a) && !find_library_asset(r.db, id_b));
	r.share(base->id, true);
	TEST_EXPECT(r.list().size() == 2 && find_library_asset(r.db, id_a));
	// A missing contributor fails closed too (the FK's ON DELETE SET NULL behavior).
	r.db.exec("UPDATE asset_files SET product_id=NULL WHERE asset_id=$1 AND name='common.dds'", {id_a});
	TEST_EXPECT(r.list().size() == 1 && !find_library_asset(r.db, id_a));
	return 0;
}

int test_bundles() {
	Rig r("asset-catalog-bundles");
	const auto base = find_product_by_key(r.db, "JO");
	TEST_EXPECT(base);
	r.share(base->id, true);
	const int64_t product = r.expansion("BUNDLE", base->id);
	TEST_EXPECT(product);
	const NativeFile model{"foo.3di", std::string("native\0model", 12)};
	const NativeFile texture{"skin.dds", std::string("texture\0pixels", 14), "base"};
	const std::vector<NativeFile> files{model, texture};
	const int64_t release = r.release(product, "3.4", 11);
	const auto dir = r.stage(manifest({asset("model", files)}), files);
	TEST_EXPECT(store_asset_snapshot(r.db, r.store, release, dir, r.error));
	r.publish(product, release);
	const int64_t id = r.only_asset(release);
	TEST_EXPECT(id && !asset_bundle_pending(r.db, id));
	TEST_EXPECT(enqueue_asset_bundle(r.db, id) > 0 && asset_bundle_pending(r.db, id));
	TEST_EXPECT(enqueue_asset_bundle(r.db, id) == 0);
	// Opting out before work starts prevents a queued job from making a bundle.
	r.share(base->id, false);
	TEST_EXPECT(jobs::run_due(r.pool, r.registry) == 1);
	TEST_EXPECT(!asset_bundle_pending(r.db, id));
	TEST_EXPECT(r.db.query("SELECT bundle_key FROM asset_entries WHERE id=$1", {id})[0].as_text(0) == "");
	r.share(base->id, true);
	// A terminal integrity failure is visible, then a fresh request can retry after repair.
	TEST_EXPECT(io::write_file_atomic(r.store.path_of(asset_blob_key(product, model.sha())), "corrupt", r.error));
	const int64_t failed_job = enqueue_asset_bundle(r.db, id);
	TEST_EXPECT(failed_job > 0);
	r.db.exec("UPDATE jobs SET max_attempts=1 WHERE id=$1", {failed_job});
	TEST_EXPECT(jobs::run_due(r.pool, r.registry) == 1 && !asset_bundle_pending(r.db, id));
	TEST_EXPECT(find_library_asset(r.db, id)->bundle_error == "asset file failed its integrity check");
	TEST_EXPECT(r.store.put_file(asset_blob_key(product, model.sha()),
			io::utf8_join(dir, "blobs/" + model.sha()), r.error));
	TEST_EXPECT(enqueue_asset_bundle(r.db, id) > 0);
	TEST_EXPECT(find_library_asset(r.db, id)->bundle_error.empty() && asset_bundle_pending(r.db, id));
	TEST_EXPECT(jobs::run_due(r.pool, r.registry) == 1 && !asset_bundle_pending(r.db, id));
	const auto built = find_library_asset(r.db, id);
	TEST_EXPECT(built && !built->bundle_key.empty() && built->bundle_error.empty());
	TEST_EXPECT(asset_download_name(*built) == "BUNDLE-r" + std::to_string(release) + "-foo.3di.zip");
	const std::string zip = r.temp.file("download.zip"), unpacked = r.temp.file("unpacked");
	TEST_EXPECT(r.store.get_file(built->bundle_key, zip, r.error));
	TEST_EXPECT(unzip_files(zip, unpacked, r.error));
	for (const auto &file : files) {
		std::string bytes;
		TEST_EXPECT(io::read_file_text(io::utf8_join(unpacked, "files/" + file.name), bytes, r.error));
		TEST_EXPECT(bytes == file.bytes);
	}
	std::string text;
	io::JsonValue metadata;
	TEST_EXPECT(io::read_file_text(io::utf8_join(unpacked, "manifest.json"), text, r.error));
	TEST_EXPECT(io::json_parse(text, metadata, r.error));
	TEST_EXPECT(metadata.get_int("schema_version", 0) == 1 && metadata.get_string("project", "") == "BUNDLE");
	TEST_EXPECT(metadata.get_int("release_id", 0) == release && metadata.get_string("version", "") == "3.4");
	TEST_EXPECT(metadata.get_int("revision", 0) == 1 && metadata.get_int("base_revision", 0) == 11);
	TEST_EXPECT(metadata.get_string("asset", "") == "foo.3di");
	const auto *members = metadata.get("files");
	TEST_EXPECT(members && members->is_array() && members->array.size() == 2);
	TEST_EXPECT(members->array[0].get_string("sha256", "") == model.sha());
	TEST_EXPECT(members->array[1].get_string("sha256", "") == texture.sha());
	TEST_EXPECT(members->array[1].get_int("product_id", 0) == base->id);
	// Cached objects do not bypass later opt-out; new reads must obtain the gated entry again.
	r.share(base->id, false);
	TEST_EXPECT(!find_library_asset(r.db, id) && r.list().empty());
	r.share(base->id, true);
	TEST_EXPECT(find_library_asset(r.db, id)->bundle_key == built->bundle_key);
	return 0;
}

int test_current_release_and_kinds() {
	Rig r("asset-catalog-releases");
	const auto product = find_product_by_key(r.db, "JO");
	TEST_EXPECT(product);
	r.share(product->id, true);
	const std::vector<NativeFile> files{{"foo.3di", "model"}, {"walk.bad", "animation"},
			{"soldier.adm", "animation map"}, {"island.trn", "generated terrain"},
			{"island.dds", "generated texture"}};
	const std::vector<std::string> kinds{"model", "animation", "animation_map", "terrain", "texture"};
	std::vector<io::JsonValue> entries;
	for (size_t i = 0; i < files.size(); ++i) {
		TEST_EXPECT(library_asset_kind(kinds[i]));
		entries.push_back(asset(kinds[i].c_str(), {files[i]}));
	}
	TEST_EXPECT(!library_asset_kind("source") && !library_asset_kind(""));
	const auto dir = r.stage(manifest(entries), files);
	const int64_t old_release = r.release(product->id, "1.0"), new_release = r.release(product->id, "2.0");
	TEST_EXPECT(store_asset_snapshot(r.db, r.store, old_release, dir, r.error));
	TEST_EXPECT(store_asset_snapshot(r.db, r.store, new_release, dir, r.error));
	r.publish(product->id, old_release);
	r.db.exec("UPDATE releases SET status='draft' WHERE id=$1", {new_release});
	TEST_EXPECT(r.list().size() == 5);
	const int64_t previous_asset = r.list().front().id;
	for (const auto &entry : r.list()) TEST_EXPECT(entry.release_id == old_release);
	for (const auto &kind : kinds) TEST_EXPECT(r.list({}, kind).size() == 1);
	r.publish(product->id, new_release);
	TEST_EXPECT(r.list().size() == 5);
	for (const auto &entry : r.list()) TEST_EXPECT(entry.release_id == new_release);
	// The list advances, but a pinned link to a still-published release remains eligible.
	TEST_EXPECT(find_library_asset(r.db, previous_asset));
	r.db.exec("UPDATE releases SET status='retired' WHERE id=$1", {old_release});
	TEST_EXPECT(!find_library_asset(r.db, previous_asset) && r.list().size() == 5);
	for (const char *status : {"awaiting_approval", "retired", "invalid"}) {
		r.db.exec("UPDATE releases SET status=$2 WHERE id=$1", {new_release, std::string(status)});
		TEST_EXPECT(r.list().empty());
	}
	r.publish(product->id, new_release);
	r.share(product->id, false);
	TEST_EXPECT(r.list().empty());
	r.share(product->id, true);
	TEST_EXPECT(r.list().size() == 5);
	return 0;
}

int test_snapshot_integrity() {
	Rig r("asset-catalog-integrity");
	const auto product = find_product_by_key(r.db, "JO");
	TEST_EXPECT(product);
	const NativeFile model{"foo.3di", "synthetic model"}, texture{"skin.dds", "synthetic texture"};
	const std::vector<NativeFile> files{model, texture};
	const auto good = manifest({asset("model", files)});
	const auto dir = r.stage(good, files);
	const int64_t release = r.release(product->id, "1.0");
	TEST_EXPECT(store_asset_snapshot(r.db, r.store, release, dir, r.error));
	const int64_t id = r.only_asset(release);
	TEST_EXPECT(id && store_asset_snapshot(r.db, r.store, release, dir, r.error));
	TEST_EXPECT(r.only_asset(release) == id);
	auto changed = good;
	changed.set("extra", io::JsonValue::make_string("cannot mutate a pinned snapshot"));
	const auto changed_dir = r.stage(changed, files);
	TEST_EXPECT(!store_asset_snapshot(r.db, r.store, release, changed_dir, r.error));
	TEST_EXPECT(r.error == "a release's asset snapshot cannot change" && r.only_asset(release) == id);

	std::vector<io::JsonValue> malformed;
	auto unknown = good;
	unknown.get("assets")->array[0].set("kind", io::JsonValue::make_string("source"));
	malformed.push_back(std::move(unknown));
	auto traversal = good;
	traversal.get("assets")->array[0].get("files")->array[1].set("name", io::JsonValue::make_string("../skin.dds"));
	malformed.push_back(std::move(traversal));
	auto origin = good;
	origin.get("assets")->array[0].get("files")->array[1].set("origin", io::JsonValue::make_string("unknown"));
	malformed.push_back(std::move(origin));
	auto absent_root = good;
	absent_root.get("assets")->array[0].get("files")->array.erase(
			absent_root.get("assets")->array[0].get("files")->array.begin());
	malformed.push_back(std::move(absent_root));
	auto duplicate = good;
	auto second = duplicate.get("assets")->array.front();
	second.set("name", io::JsonValue::make_string("FOO.3DI"));
	duplicate.get("assets")->push(std::move(second));
	malformed.push_back(std::move(duplicate));
	auto unicode_collision = good;
	auto *unicode_files = unicode_collision.get("assets")->array[0].get("files");
	unicode_files->array[1].set("name", io::JsonValue::make_string("caf\xc3\xa9.dds"));
	auto alias = unicode_files->array[1];
	alias.set("name", io::JsonValue::make_string("CAF\xc3\x89.DDS"));
	unicode_files->push(std::move(alias));
	malformed.push_back(std::move(unicode_collision));
	for (const auto &value : malformed) {
		const int64_t rejected = r.release(product->id, "9.0");
		const auto rejected_dir = r.stage(value, files);
		TEST_EXPECT(!store_asset_snapshot(r.db, r.store, rejected, rejected_dir, r.error));
		TEST_EXPECT(r.db.query("SELECT release_id FROM asset_snapshots WHERE release_id=$1", {rejected}).empty());
	}
	const int64_t corrupt = r.release(product->id, "9.1");
	const auto corrupt_dir = r.stage(good, files);
	TEST_EXPECT(io::write_file_atomic(io::utf8_join(corrupt_dir, "blobs/" + model.sha()), "corrupt", r.error));
	TEST_EXPECT(!store_asset_snapshot(r.db, r.store, corrupt, corrupt_dir, r.error));
	TEST_EXPECT(r.error == "asset blob failed its integrity check");
	TEST_EXPECT(r.db.query("SELECT release_id FROM asset_snapshots WHERE release_id=$1", {corrupt}).empty());
	const int64_t expansion = r.expansion("UNPINNED", product->id);
	TEST_EXPECT(expansion);
	const int64_t unpinned = r.release(expansion, "1.0");
	const std::vector<NativeFile> inherited{model, {texture.name, texture.bytes, "base"}};
	const auto unpinned_dir = r.stage(manifest({asset("model", inherited)}), inherited);
	TEST_EXPECT(!store_asset_snapshot(r.db, r.store, unpinned, unpinned_dir, r.error));
	TEST_EXPECT(r.error == "invalid asset dependency");
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	test_db::init(argc, argv);
	const int failures = test_download_names() + test_variants_and_sharing() + test_bundles() +
			test_current_release_and_kinds() + test_snapshot_integrity();
	if (failures == 0) std::printf("asset_catalog: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
