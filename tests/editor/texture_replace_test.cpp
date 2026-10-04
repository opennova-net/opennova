// ADR 0046 S18, a texture made from an image (import/texture_source, replace_texture) and a texture split
// (graph/rename_transaction plan_split, split_texture): the options that make a texture again as it is
// stored; a plain texture replaced by a PNG (the image an import source in art/, its record making the
// texture under its name, the plain file set aside under .opennova/replaced/), an import's output
// replaced by an image of its source's kind (the source written over) and by one of another (a new
// source, the old set aside), a name the project lacks made; the refusals; a texture two files use split
// so one of them names a copy (a plain file copied; an import's output copied as its source); the source
// a paint program edits for each kind of texture.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <editor/documents/texture_image.h>
#include <editor/import/png_encode.h>
#include <editor/import/sidecar.h>
#include <editor/import/texture_import.h>
#include <editor/import/texture_source.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/dds/dds.h>
#include <formats/env/env.h>
#include <formats/pcx/pcx_io.h>
#include <formats/tga/tga.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova;
using namespace opennova::editor;
namespace fs = std::filesystem;

namespace {

std::vector<uint8_t> solid(int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
	std::vector<uint8_t> out;
	for (int i = 0; i < w * h; ++i) out.insert(out.end(), {r, g, b, a});
	return out;
}

std::vector<uint8_t> tga32(const std::vector<uint8_t> &rgba, int w, int h) {
	std::vector<uint8_t> out;
	std::string why;
	tga::tga_write_rgba32(rgba.data(), uint32_t(w), uint32_t(h), out, why);
	return out;
}

std::vector<uint8_t> png(const std::vector<uint8_t> &rgba, int w, int h) { return encode_png_rgba(rgba.data(), uint32_t(w), uint32_t(h)); }

std::vector<uint8_t> read_bytes(const std::string &path) {
	std::vector<uint8_t> out;
	std::string error;
	read_file_bytes(path, out, error);
	return out;
}

// The first texel of a file as the game reads it.
std::vector<uint8_t> first_texel(const std::string &name, const std::vector<uint8_t> &bytes) {
	const std::shared_ptr<const TextureImage> image = decode_texture(name, bytes);
	if (!image || !image->loads || image->levels.empty()) return {};
	return std::vector<uint8_t>(image->levels[0].rgba.begin(), image->levels[0].rgba.begin() + 4);
}

ImportOptions record_of(const std::string &root, const std::string &source) {
	ImportSidecar record;
	Diagnostic error;
	load_import_sidecar(root + "/" + source + kImportSidecarSuffix, record, error);
	return record.options;
}

bool set_aside(const std::string &root, const std::string &relative) {
	std::error_code ec;
	for (const auto &stamp : fs::directory_iterator(fs::path(root) / ".opennova" / "replaced", ec))
		if (fs::is_regular_file(stamp.path() / relative, ec)) return true;
	return false;
}

int test_options() {
	const std::vector<uint8_t> rgba = solid(4, 4, 10, 20, 30, 128);
	TEST_EXPECT(texture_reproducing_options("body.tga", tga32(rgba, 4, 4), "body.png", false) == ImportOptions({{"format", "tga"}}));
	std::vector<uint8_t> rgb;
	std::string why;
	TEST_EXPECT(tga::tga_write_rgb24(rgba.data(), 4, 4, rgb, why));
	TEST_EXPECT(texture_reproducing_options("map.tga", rgb, "map_src.tga", false) ==
	            ImportOptions({{"format", "tga24"}, {"name", "map.tga"}}));
	std::vector<uint8_t> dds;
	TEST_EXPECT(dds::dds_write_a8r8g8b8(rgba.data(), 4, 4, dds, why));
	TEST_EXPECT(texture_reproducing_options("skin.dds", dds, "skin.png", false) == ImportOptions({{"dds", "argb"}, {"format", "dds"}}));
	TEST_EXPECT(texture_reproducing_options("isle_f.pcx", {}, "isle_f_src.pcx", true) ==
	            ImportOptions({{"format", "pcx"}, {"name", "isle_f.pcx"}, {"palette", "indices"}}));
	TEST_EXPECT(texture_reproducing_options("bump.mdt", {}, "bump.png", false) == ImportOptions({{"format", "mdt"}}));
	std::printf("options: a 32- and a 24-bit TGA, a DDS's form, an indexed PCX's indices, an MDT, the name\n");
	return 0;
}

int test_replace() {
	editor_test::TempProjectDir dir{"opennova_editor_texture_replace"};
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Replace"));
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	const std::string root = view.project.root;
	const std::string outside = dir.file("mine");
	TEST_EXPECT(editor_test::write_bytes(root + "/textures/body.tga", tga32(solid(8, 8, 200, 0, 0), 8, 8)) &&
	            editor_test::write_bytes(outside + "/skin.png", png(solid(8, 8, 0, 200, 0), 8, 8)) &&
	            editor_test::write_bytes(outside + "/skin2.png", png(solid(8, 8, 0, 0, 200), 8, 8)) &&
	            editor_test::write_bytes(outside + "/paint.tga", tga32(solid(8, 8, 50, 60, 70), 8, 8)) &&
	            editor_test::write_text(outside + "/notes.txt", "x"));
	editor_test::handle_to_end(session, request::rescan());
	session.run_operations();

	// A plain texture replaced by a PNG: the PNG an import source in art/, the texture its output under its
	// own name, the plain file set aside.
	editor_test::handle_to_end(session, request::replace_texture("textures/body.tga", outside + "/skin.png"));
	TEST_EXPECT(session.outcome().done());
	session.run_operations();
	const AssetEntry *body = view.project.scan->find("body.tga");
	TEST_EXPECT(body && body->imported_from == "art/skin.png");
	TEST_EXPECT(!fs::exists(root + "/textures/body.tga") && set_aside(root, "textures/body.tga"));
	TEST_EXPECT(record_of(root, "art/skin.png") == ImportOptions({{"format", "tga"}, {"name", "body.tga"}}));
	if (body) TEST_EXPECT(first_texel("body.tga", read_bytes(root + "/" + body->relative_path)) == std::vector<uint8_t>({0, 200, 0, 255}));

	// An import's output replaced by an image of its source's kind: the source written over.
	editor_test::handle_to_end(session, request::replace_texture("body.tga", outside + "/skin2.png"));
	TEST_EXPECT(session.outcome().done());
	session.run_operations();
	body = view.project.scan->find("body.tga");
	TEST_EXPECT(body && body->imported_from == "art/skin.png" && set_aside(root, "art/skin.png"));
	if (body) TEST_EXPECT(first_texel("body.tga", read_bytes(root + "/" + body->relative_path)) == std::vector<uint8_t>({0, 0, 200, 255}));
	// By one of another kind: a TGA source of a name of its own, the old source and its record set aside.
	editor_test::handle_to_end(session, request::replace_texture("body.tga", outside + "/paint.tga"));
	TEST_EXPECT(session.outcome().done());
	session.run_operations();
	body = view.project.scan->find("body.tga");
	TEST_EXPECT(body && body->imported_from == "art/paint.tga" && !fs::exists(root + "/art/skin.png") &&
	            set_aside(root, "art/skin.png.import"));
	TEST_EXPECT(record_of(root, "art/paint.tga") == ImportOptions({{"format", "tga"}, {"name", "body.tga"}}));
	if (body) TEST_EXPECT(first_texel("body.tga", read_bytes(root + "/" + body->relative_path)) == std::vector<uint8_t>({50, 60, 70, 255}));

	// A name the project lacks (a field's missing texture): made under it (art/skin.png free again, its old
	// source set aside).
	editor_test::handle_to_end(session, request::replace_texture("grass.tga", outside + "/skin.png"));
	TEST_EXPECT(session.outcome().done());
	session.run_operations();
	const AssetEntry *grass = view.project.scan->find("grass.tga");
	TEST_EXPECT(grass && grass->imported_from == "art/skin.png" &&
	            record_of(root, "art/skin.png") == ImportOptions({{"format", "tga"}, {"name", "grass.tga"}}));

	// Refused, nothing written: an image the importer does not read, a file that is no texture, an option
	// no row takes.
	session.handle(request::replace_texture("grass.tga", outside + "/notes.txt"));
	TEST_EXPECT(session.outcome().refused && session.outcome().findings.back().code() == "texture.replace");
	session.handle(request::replace_texture("project.opennova", outside + "/skin.png"));
	TEST_EXPECT(session.outcome().refused);
	session.handle(request::replace_texture("grass.tga", outside + "/skin.png", {{"shine", "on"}}));
	TEST_EXPECT(session.outcome().refused);

	// The source a paint program edits: an output's own; a plain TGA's a copy beside in art/ under a name of
	// its own; a DDS's a PNG of its first level.
	{
		TextureSourcePlan plan = plan_texture_source(ProjectPaths::for_root(root), *view.project.scan, "body.tga");
		TEST_EXPECT(plan.ok() && plan.source == "art/paint.tga" && plan.bytes.empty());
		TEST_EXPECT(editor_test::write_bytes(root + "/textures/rock.tga", tga32(solid(4, 4, 1, 2, 3), 4, 4)));
		std::vector<uint8_t> dds;
		std::string why;
		const std::vector<uint8_t> blue = solid(4, 4, 0, 0, 255);
		TEST_EXPECT(dds::dds_write_a8r8g8b8(blue.data(), 4, 4, dds, why) && editor_test::write_bytes(root + "/textures/sky.dds", dds));
		editor_test::handle_to_end(session, request::rescan());
		session.run_operations();
		plan = plan_texture_source(ProjectPaths::for_root(root), *view.project.scan, "textures/rock.tga");
		TEST_EXPECT(plan.ok() && plan.source == "art/rock_src.tga" && plan.replaced == "textures/rock.tga" &&
		            plan.options == ImportOptions({{"format", "tga"}, {"name", "rock.tga"}}));
		plan = plan_texture_source(ProjectPaths::for_root(root), *view.project.scan, "sky.dds");
		TEST_EXPECT(plan.ok() && plan.source == "art/sky.png" && plan.options == ImportOptions({{"dds", "argb"}, {"format", "dds"}}));
		ImageSource image;
		TEST_EXPECT(decode_image_source("sky.png", plan.bytes, image, why) && image.image.pixels == blue);
	}
	std::printf("replace: a plain texture, an output by its source's kind and another, a missing name, the refusals, "
	            "the sources to edit\n");
	return 0;
}

int test_split() {
	editor_test::TempProjectDir dir{"opennova_editor_texture_split"};
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Split"));
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	const std::string root = view.project.root;
	TEST_EXPECT(editor_test::write_bytes(root + "/textures/cloud.tga", tga32(solid(4, 4, 9, 9, 9), 4, 4)) &&
	            editor_test::write_text(root + "/envs/a.env", "sky_map1 cloud.tga\r\n") &&
	            editor_test::write_text(root + "/envs/b.env", "sky_map1 cloud.tga\r\nsky_map2 cloud.tga\r\n"));
	editor_test::handle_to_end(session, request::rescan());
	session.run_operations();
	// A plain file: copied, b.env's two layers moved to the copy, a.env's left.
	editor_test::handle_to_end(session, request::split_texture("textures/cloud.tga", "cloud_2.tga", {"envs/b.env"}));
	TEST_EXPECT(session.outcome().done());
	session.run_operations();
	TEST_EXPECT(fs::exists(root + "/textures/cloud.tga") && fs::exists(root + "/textures/cloud_2.tga"));
	std::string text, error;
	TEST_EXPECT(read_file_text(root + "/envs/b.env", text, error) && text == "sky_map1 cloud_2.tga\r\nsky_map2 cloud_2.tga\r\n");
	TEST_EXPECT(read_file_text(root + "/envs/a.env", text, error) && text == "sky_map1 cloud.tga\r\n");
	// An import's output: its source copied beside it, the copy's record naming the new file.
	TEST_EXPECT(editor_test::write_bytes(root + "/art/haze.png", png(solid(4, 4, 7, 7, 7), 4, 4)));
	ImportSidecar record;
	record.importer = "image";
	record.version = kImageImporterVersion;
	record.options = {{"format", "tga"}};
	Diagnostic saved;
	TEST_EXPECT(save_import_sidecar(root + "/art/haze.png" + kImportSidecarSuffix, record, saved));
	TEST_EXPECT(editor_test::write_text(root + "/envs/c.env", "sky_map1 haze.tga\r\n") &&
	            editor_test::write_text(root + "/envs/d.env", "sky_map1 haze.tga\r\n"));
	editor_test::handle_to_end(session, request::rescan());
	session.run_operations();
	TEST_EXPECT(view.project.scan->find("haze.tga") && view.project.scan->find("haze.tga")->imported_from == "art/haze.png");
	editor_test::handle_to_end(session, request::split_texture("haze.tga", "haze_2.tga", {"envs/d.env"}));
	TEST_EXPECT(session.outcome().done());
	session.run_operations();
	const AssetEntry *copy = view.project.scan->find("haze_2.tga");
	TEST_EXPECT(copy && copy->imported_from == "art/haze_2.png" &&
	            record_of(root, "art/haze_2.png") == ImportOptions({{"format", "tga"}, {"name", "haze_2.tga"}}));
	TEST_EXPECT(read_file_text(root + "/envs/d.env", text, error) && text == "sky_map1 haze_2.tga\r\n");
	TEST_EXPECT(read_file_text(root + "/envs/c.env", text, error) && text == "sky_map1 haze.tga\r\n");
	// Refused: no referrer named, one that does not use it, a name taken.
	session.handle(request::split_texture("textures/cloud.tga", "cloud_3.tga", {}));
	session.run_operations();
	TEST_EXPECT(!fs::exists(root + "/textures/cloud_3.tga"));
	session.handle(request::split_texture("textures/cloud.tga", "cloud_3.tga", {"envs/c.env"}));
	session.run_operations();
	TEST_EXPECT(!fs::exists(root + "/textures/cloud_3.tga"));
	session.handle(request::split_texture("textures/cloud.tga", "haze.tga", {"envs/a.env"}));
	session.run_operations();
	TEST_EXPECT(read_file_text(root + "/envs/a.env", text, error) && text == "sky_map1 cloud.tga\r\n");
	std::printf("split: a plain file copied, an output's source copied, the uses moved, the refusals\n");
	return 0;
}

} // namespace

int main() {
	int failures = test_options();
	failures += test_replace();
	failures += test_split();
	if (failures == 0) std::printf("editor_texture_replace: all passed\n");
	return failures == 0 ? 0 : 1;
}
