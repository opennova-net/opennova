// Release-native catalogs: equal model bytes remain separate resolved variants, an inherited
// dependency's outgoing edges see the mod's overrides, and generated terrain bytes are bundled.
// Everything is minted by engine writers, with no proprietary install or authored fixtures.
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <base/io/sha256.h>
#include <editor/session/native_asset_catalog.h>
#include <editor/import/terrain_import.h>
#include <formats/threedi/threedi_o3d_lower.h>
#include <formats/tga/tga.h>
#include <runtime/renderer/model_target.h>

#include "common/temp_dir.h"
#include "common/test_expect.h"
#include "editor/minted_install.h"

using namespace opennova;
using namespace opennova::editor;
using io::JsonValue;
using minted_install::File;

namespace {

std::string hash(const std::vector<uint8_t> &bytes) {
	return io::sha256_hex(bytes.data(), bytes.size());
}

bool texture(int side, uint8_t colour, std::vector<uint8_t> &out, std::string &error) {
	std::vector<uint8_t> rgba(size_t(side) * side * 4, colour);
	for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
	return tga::tga_write_rgb24(rgba.data(), uint32_t(side), uint32_t(side), out, error);
}

bool model(const std::string &texture_name, std::vector<uint8_t> &out) {
	std::istringstream text("o3d 2\nmodel foo\nmaterial FF_ST_OP\ntexture " + texture_name +
			"\nlod 0\npart 0 0 0 0\nmesh 0\n"
			"v 0 -0.5 0 1 0 0 0 1\nv 0 0.5 0 1 0 0 1 1\nv 0 0 1 1 0 0 0.5 0\n"
			"v 0 -0.5 0 -1 0 0 0 1\nv 0 0.5 0 -1 0 0 1 1\nv 0 0 1 -1 0 0 0.5 0\n"
			"t 0 1 2\nt 4 3 5\npanm 0 0\n");
	std::vector<threedi::SceneFinding> findings;
	if (threedi::threedi_o3d_build(text, renderer::retail_model_target(), out, findings)) return true;
	for (const auto &finding : findings) std::fprintf(stderr, "%s\n", finding.message.c_str());
	return false;
}

const JsonValue *named(const JsonValue &parent, const char *list, const char *name) {
	const JsonValue *rows = parent.get(list);
	if (rows) for (const JsonValue &row : rows->array)
		if (row.get_string("name", "") == name) return &row;
	return nullptr;
}

bool capture(const std::string &source, const std::string &base, const std::string &out,
		JsonValue &manifest, NativeAssetCatalogResult &result, std::string &error) {
	NativeAssetCatalogOptions options{ source, base, "jo", base.empty() ? "" : "nifty", out };
	if (!write_native_asset_catalog(options, result, error)) {
		std::fprintf(stderr, "catalog: %s\n", error.c_str());
		return false;
	}
	for (const std::string &why : result.omitted) std::fprintf(stderr, "omitted %s\n", why.c_str());
	return io::json_parse(test_io::read_file_text(out + "/manifest.json"), manifest, error);
}

int expect_blobs(const JsonValue &manifest, const std::string &out) {
	const JsonValue *assets = manifest.get("assets");
	TEST_EXPECT(assets && assets->is_array());
	TEST_EXPECT(manifest.get_int("schema_version", 0) == 1);
	for (const JsonValue &asset : assets->array) {
		const JsonValue *files = asset.get("files");
		TEST_EXPECT(files && files->is_array());
		for (const JsonValue &file : files->array) {
			const std::string sha = file.get_string("sha256", "");
			const auto bytes = test_io::read_file(out + "/blobs/" + sha);
			TEST_EXPECT(!bytes.empty() && hash(bytes) == sha);
			TEST_EXPECT(double(bytes.size()) == file.get_number("size", -1));
			TEST_EXPECT(file.get_string("name", "").find('/') == std::string::npos);
		}
	}
	return 0;
}

int variants_and_animations() {
	test_temp::TempDir temp("native_asset_variants");
	std::string error;
	std::vector<uint8_t> model_bytes, first_texture, second_texture, missing_texture;
	TEST_EXPECT(model("foo.tga", model_bytes));
	TEST_EXPECT(model("absent.tga", missing_texture));
	TEST_EXPECT(texture(16, 30, first_texture, error));
	TEST_EXPECT(texture(16, 200, second_texture, error));
	std::vector<File> base, first, second;
	TEST_EXPECT(minted_install::blank(base, "_ffp.fx", error));
	TEST_EXPECT(minted_install::blank(base, "rig_rst.bad", error));
	minted_install::put(base, "baseonly.3di", model_bytes);
	minted_install::put(base, "foo.tga", second_texture);
	TEST_EXPECT(minted_install::write_install(temp.file("base"), base, "", {}, error));
	minted_install::put(first, "foo.3di", model_bytes);
	minted_install::put(first, "foo.tga", first_texture);
	minted_install::put(first, "missing.3di", missing_texture);
	minted_install::put(first, "broken.bad", {1, 2, 3});
	TEST_EXPECT(minted_install::blank(first, "rig.adm", error));
	TEST_EXPECT(minted_install::blank(first, "orphan.adm", error));
	TEST_EXPECT(minted_install::blank(first, "own.bad", error));
	second = first;
	minted_install::put(second, "foo.tga", second_texture);
	TEST_EXPECT(minted_install::write_install(temp.file("first"), {}, "nifty", first, error));
	TEST_EXPECT(minted_install::write_install(temp.file("second"), {}, "nifty", second, error));
	JsonValue a, b;
	NativeAssetCatalogResult result;
	TEST_EXPECT(capture(temp.file("first"), temp.file("base"), temp.file("a"), a, result, error));
	TEST_EXPECT(result.omitted.size() == 3);
	TEST_EXPECT(capture(temp.file("second"), temp.file("base"), temp.file("b"), b, result, error));
	const JsonValue *am = named(a, "assets", "foo.3di"), *bm = named(b, "assets", "foo.3di");
	TEST_EXPECT(am && bm);
	TEST_EXPECT(am->get_string("sha256", "") == hash(model_bytes));
	TEST_EXPECT(am->get_string("sha256", "") == bm->get_string("sha256", ""));
	const JsonValue *at = named(*am, "files", "foo.tga"), *bt = named(*bm, "files", "foo.tga");
	TEST_EXPECT(at && bt && at->get_string("sha256", "") == hash(first_texture));
	TEST_EXPECT(bt->get_string("sha256", "") == hash(second_texture));
	TEST_EXPECT(at->get_string("origin", "") == "project");
	const JsonValue *shader = named(*am, "files", "_ffp.fx");
	TEST_EXPECT(shader && shader->get_string("origin", "") == "base");
	TEST_EXPECT(!named(a, "assets", "baseonly.3di"));
	TEST_EXPECT(!named(a, "assets", "rig_rst.bad"));
	TEST_EXPECT(!named(a, "assets", "missing.3di"));
	TEST_EXPECT(!named(a, "assets", "broken.bad"));
	TEST_EXPECT(!named(a, "assets", "orphan.adm"));
	const JsonValue *rig = named(a, "assets", "rig.adm");
	TEST_EXPECT(rig && rig->get_string("kind", "") == "animation_map");
	const JsonValue *rest = named(*rig, "files", "rig_rst.bad");
	TEST_EXPECT(rest && rest->get_string("origin", "") == "base");
	const JsonValue *own = named(a, "assets", "own.bad");
	TEST_EXPECT(own && own->get_string("kind", "") == "animation");
	TEST_EXPECT(expect_blobs(a, temp.file("a")) == 0);
	TEST_EXPECT(expect_blobs(b, temp.file("b")) == 0);
	// Catalogs cannot overwrite an existing snapshot, even after input contents change.
	NativeAssetCatalogOptions overwrite{ temp.file("second"), temp.file("base"), "jo", "nifty", temp.file("a") };
	TEST_EXPECT(!write_native_asset_catalog(overwrite, result, error));
	TEST_EXPECT(test_io::read_file_text(temp.file("a/manifest.json")) == io::json_write(a));
	return 0;
}

int generated_terrain_and_inherited_closure() {
	test_temp::TempDir temp("native_asset_terrain");
	std::string error;
	std::vector<uint8_t> colour, foliage, texture_bytes, model_bytes;
	TEST_EXPECT(texture(1024, 80, colour, error));
	TEST_EXPECT(texture(16, 1, foliage, error));
	TEST_EXPECT(texture(16, 140, texture_bytes, error));
	TEST_EXPECT(model("foo.tga", model_bytes));
	TEST_EXPECT(test_io::write_file(temp.file("source/height.raw"), std::vector<uint8_t>(1024 * 1024, 20)));
	TEST_EXPECT(test_io::write_file(temp.file("source/colour.tga"), colour));
	TEST_EXPECT(test_io::write_file(temp.file("source/foliage.tga"), foliage));
	TerrainSet set;
	set.heightmap = "height.raw";
	set.colormap = "colour.tga";
	set.foliagemap = "foliage.tga";
	FoliageDef fern;
	fern.graphic = "baseonly"; // the real resolver adds the model extension
	fern.match[0] = 1;
	set.foliage.push_back(fern);
	const auto source = write_terrain_set(set);
	const ImportOptions options;
	ImportContext context("isle.tset", source, options, temp.file("source"), temp.file("source"));
	ImportProduct generated;
	const bool imported = run_terrain_import(context, generated);
	for (const Diagnostic &finding : generated.diagnostics) std::fprintf(stderr, "%s\n", finding.message.c_str());
	TEST_EXPECT(imported);
	std::vector<File> own, base;
	for (const ImportOutput &file : generated.outputs) minted_install::put(own, file.name, file.bytes);
	minted_install::put(own, "foo.tga", texture_bytes);
	minted_install::put(base, "baseonly.3di", model_bytes);
	TEST_EXPECT(minted_install::blank(base, "_ffp.fx", error));
	TEST_EXPECT(minted_install::write_install(temp.file("base"), base, "", {}, error));
	TEST_EXPECT(minted_install::write_install(temp.file("mod"), {}, "nifty", own, error));
	JsonValue manifest;
	NativeAssetCatalogResult result;
	TEST_EXPECT(capture(temp.file("mod"), temp.file("base"), temp.file("catalog"), manifest, result, error));
	TEST_EXPECT(result.omitted.empty());
	const JsonValue *terrain = named(manifest, "assets", "isle.trn");
	TEST_EXPECT(terrain && terrain->get_string("kind", "") == "terrain");
	for (const ImportOutput &file : generated.outputs) {
		const JsonValue *member = named(*terrain, "files", file.name.c_str());
		TEST_EXPECT(member && member->get_string("sha256", "") == hash(file.bytes));
		TEST_EXPECT(member->get_string("origin", "") == "project");
	}
	const JsonValue *inherited = named(*terrain, "files", "baseonly.3di");
	TEST_EXPECT(inherited && inherited->get_string("origin", "") == "base");
	// The own terrain reaches a base model, whose texture is overridden by this mod.
	const JsonValue *overridden = named(*terrain, "files", "foo.tga");
	TEST_EXPECT(overridden && overridden->get_string("origin", "") == "project");
	TEST_EXPECT(overridden->get_string("sha256", "") == hash(texture_bytes));
	TEST_EXPECT(!named(*terrain, "files", "height.raw"));
	TEST_EXPECT(!named(*terrain, "files", "isle.tset"));
	TEST_EXPECT(!named(manifest, "assets", "baseonly.3di"));
	TEST_EXPECT(expect_blobs(manifest, temp.file("catalog")) == 0);
	// A standalone release owns its whole export, without a synthetic base origin.
	std::vector<File> standalone = base;
	for (const File &file : own) minted_install::put(standalone, file.name, *file.bytes);
	TEST_EXPECT(minted_install::write_install(temp.file("standalone"), standalone, "", {}, error));
	TEST_EXPECT(capture(temp.file("standalone"), "", temp.file("standalone-catalog"), manifest, result, error));
	const JsonValue *base_model = named(manifest, "assets", "baseonly.3di");
	TEST_EXPECT(result.omitted.empty() && base_model);
	for (const JsonValue &member : base_model->get("files")->array)
		TEST_EXPECT(member.get_string("origin", "") == "project");
	return 0;
}

} // namespace

int main() {
	if (variants_and_animations()) return 1;
	if (generated_terrain_and_inherited_closure()) return 1;
	return 0;
}
