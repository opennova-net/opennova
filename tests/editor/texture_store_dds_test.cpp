// ADR 0046 S18, store_as_dds: a .tga every use of which reads the .dds of its name first (a model's diffuse
// and detail rows) stored as that .dds, DXT1 for a texture of no alpha and DXT5 for one with alpha, every
// level to 1 x 1: a plain file made an import's output (its copy in art/, the plain file set aside), an
// import's output's own record taking the form; the referrers still naming the .tga, which the project
// no longer holds, and finding nothing missing; the build packing the .dds and neither the .tga nor its
// copy. Refused, nothing written: a .tga a terrain reads by its own name, sides that are not powers of two,
// a file that is no .tga, a .dds of the name already there, a name the project lacks.
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <editor/assets/asset_import.h>
#include <editor/import/sidecar.h>
#include <editor/import/texture_import.h>
#include <editor/project/project_files.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/dds/dds.h>
#include <formats/pff/pff.h>
#include <formats/tga/tga.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova;
using namespace opennova::editor;
namespace fs = std::filesystem;

namespace {

std::vector<uint8_t> tga_bytes(uint32_t w, uint32_t h, uint8_t alpha) {
	std::vector<uint8_t> rgba;
	for (uint32_t i = 0; i < w * h; ++i) rgba.insert(rgba.end(), {uint8_t(i * 7), uint8_t(i * 3), 200, alpha});
	std::vector<uint8_t> out;
	std::string why;
	tga::tga_write_rgba32(rgba.data(), w, h, out, why);
	return out;
}

ImportChoice loose(std::string path) {
	ImportChoice choice;
	choice.path = std::move(path);
	return choice;
}

bool read_dds(const std::string &path, dds::DdsImage &image) {
	std::vector<uint8_t> bytes;
	std::string error;
	return read_file_bytes(path, bytes, error) && dds::dds_read(bytes.data(), bytes.size(), image, error) && image.loads;
}

} // namespace

int main() {
	editor_test::TempProjectDir dir{"opennova_editor_texture_store_dds"};
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Store"));
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	const std::string root = view.project.root;
	// A model of a diffuse and a detail row, a glass row with alpha, an odd-sided row; a terrain that reads a
	// colour map and a detail map by their own names.
	const std::string scene = dir.file("scene");
	TEST_EXPECT(editor_test::write_text(scene + "/thing.o3d",
	                                    "o3d 1\nmodel THING\nmaterial FF_MT_OP\ntexture body.tga 1 0\ntexture grain.tga 2 0\n"
	                                    "material FF_ST_OP\ntexture glass.tga 1 0\nmaterial FF_ST_OP\ntexture odd.tga 1 0\n"
	                                    "material FF_ST_OP\ntexture skin.tga 1 0\nmaterial FF_ST_OP\ntexture dup.tga 1 0\n"
	                                    "lod 0\npart 0 0 0 0\nstrip 0 0\n"
	                                    "v 0 0 0 0 0 1 0 0\nv 1 0 0 0 0 1 1 0\nv 0 1 0 0 0 1 0 1\nt 0 1 2\n"));
	TEST_EXPECT(import_assets({loose(scene + "/thing.o3d")}, ProjectPaths::for_root(root), *view.project.document, false)
	                    .imported == std::vector<std::string>({"models/thing.3di"}));
	const std::string t = root + "/textures/";
	TEST_EXPECT(editor_test::write_bytes(t + "body.tga", tga_bytes(8, 8, 255)) && editor_test::write_bytes(t + "grain.tga", tga_bytes(4, 4, 255)) &&
	            editor_test::write_bytes(t + "glass.tga", tga_bytes(4, 4, 128)) && editor_test::write_bytes(t + "odd.tga", tga_bytes(6, 4, 255)) &&
	            editor_test::write_bytes(t + "map.tga", tga_bytes(4, 4, 255)) && editor_test::write_bytes(t + "dup.tga", tga_bytes(4, 4, 255)) &&
	            editor_test::write_bytes(t + "dup.dds", tga_bytes(4, 4, 255)));
	TEST_EXPECT(editor_test::write_text(root + "/terrains/isle.trn",
	                                    "polytrn_colormap map.tga\npolytrn_detailmap cdet.tga\npolytrn_polydata isle.cpt\n"
	                                    "polytrn_sectorcount 1\npolytrn_sectors 0\n"));
	// skin.tga: an import's output (a TGA source with its record writing it as a 32-bit TGA).
	TEST_EXPECT(editor_test::write_bytes(root + "/art/skin_src.tga", tga_bytes(4, 4, 255)));
	{
		ImportSidecar record;
		record.importer = "image";
		record.version = kImageImporterVersion;
		record.options = {{"format", "tga"}, {"name", "skin.tga"}};
		Diagnostic error;
		TEST_EXPECT(save_import_sidecar(root + "/art/skin_src.tga" + kImportSidecarSuffix, record, error));
	}
	editor_test::handle_to_end(session, request::rescan());
	session.run_operations();
	TEST_EXPECT(view.project.scan->find("skin.tga") && view.project.scan->find("skin.tga")->imported_from == "art/skin_src.tga");

	const auto refused = [&](const std::string &path, const std::string &words) {
		session.handle(request::store_as_dds(path));
		session.run_operations();
		const bool ok = session.outcome().refused && !session.outcome().findings.empty() &&
		                session.outcome().findings.back().code() == "texture.store_dds" &&
		                session.outcome().findings.back().message.find(words) != std::string::npos;
		if (!ok)
			std::fprintf(stderr, "store_as_dds %s: %s\n", path.c_str(),
			             session.outcome().findings.empty() ? "(no finding)" : session.outcome().findings.back().message.c_str());
		return ok;
	};
	TEST_EXPECT(refused("textures/map.tga", "reads map.tga itself"));
	TEST_EXPECT(refused("textures/odd.tga", "powers of two"));
	TEST_EXPECT(refused("textures/dup.tga", "has dup.dds already"));
	TEST_EXPECT(refused("textures/dup.dds", "no .tga"));
	TEST_EXPECT(refused("textures/gone.tga", "no texture named"));
	TEST_EXPECT(fs::exists(t + "map.tga") && fs::exists(t + "odd.tga") && !fs::exists(root + "/art/map_src.tga"));

	// A plain .tga: its copy in art/ makes body.dds, the .tga set aside; DXT1 with its chain.
	editor_test::handle_to_end(session, request::store_as_dds("textures/body.tga"));
	TEST_EXPECT(session.outcome().done() && !session.outcome().refused);
	session.run_operations();
	const AssetEntry *body = view.project.scan->find("body.dds");
	TEST_EXPECT(body && body->imported_from == "art/body_src.tga" && !fs::exists(t + "body.tga") && !view.project.scan->find("body.tga"));
	dds::DdsImage image;
	TEST_EXPECT(body && read_dds(root + "/" + body->relative_path, image) && std::string(image.format.name) == "DXT1" &&
	            image.levels.size() == 4);
	// With alpha: DXT5. A detail row reads its .dds alike.
	editor_test::handle_to_end(session, request::store_as_dds("textures/glass.tga"));
	editor_test::handle_to_end(session, request::store_as_dds("textures/grain.tga"));
	session.run_operations();
	const AssetEntry *glass = view.project.scan->find("glass.dds");
	TEST_EXPECT(glass && read_dds(root + "/" + glass->relative_path, image) && std::string(image.format.name) == "DXT5");
	TEST_EXPECT(view.project.scan->find("grain.dds") != nullptr);
	// An import's output: its own record writes the .dds now.
	editor_test::handle_to_end(session, request::store_as_dds("skin.tga"));
	session.run_operations();
	{
		ImportSidecar record;
		Diagnostic error;
		TEST_EXPECT(load_import_sidecar(root + "/art/skin_src.tga" + kImportSidecarSuffix, record, error) &&
		            record.options == ImportOptions({{"dds", "dxt1"}, {"format", "dds"}, {"name", "skin.dds"}}));
		const AssetEntry *skin = view.project.scan->find("skin.dds");
		TEST_EXPECT(skin && skin->imported_from == "art/skin_src.tga" && !view.project.scan->find("skin.tga"));
	}

	// The model's rows still name the .tga, which the project lacks: nothing missing, nothing never read.
	for (const Diagnostic &d : view.findings.diagnostics) {
		const bool about = d.message.find("body.") != std::string::npos || d.message.find("glass.") != std::string::npos ||
		                   d.message.find("grain.") != std::string::npos || d.message.find("skin.") != std::string::npos;
		if (about && (d.code() == "reference.missing" || d.code() == "texture.not_read"))
			std::fprintf(stderr, "unexpected %s: %s\n", d.code().c_str(), d.message.c_str());
		TEST_EXPECT(!about || (d.code() != "reference.missing" && d.code() != "texture.not_read"));
	}
	// The build packs the .dds, never the .tga's copy (an import source) or the set-aside file (the terrain,
	// whose 4 x 4 colour map refuses a build, taken out first).
	std::error_code ec;
	fs::remove(root + "/terrains/isle.trn", ec);
	fs::remove(t + "map.tga", ec);
	editor_test::handle_to_end(session, request::rescan());
	session.run_operations();
	const std::string built = dir.file("built");
	editor_test::handle_to_end(session, request::build(built));
	session.run_operations();
	std::string resource; // the build's own folder under the one named
	for (const auto &file : fs::recursive_directory_iterator(built, ec))
		if (file.path().filename() == "resource.pff") resource = file.path().string();
	pff::PffArchive archive{};
	TEST_EXPECT(!resource.empty() && pff::pff_open(&archive, resource.c_str()) == 0);
	const auto packed = [&](const char *name) { return pff::pff_find(&archive, name) != nullptr; };
	TEST_EXPECT(packed("body.dds") && packed("glass.dds") && packed("grain.dds") && packed("skin.dds") && packed("thing.3di"));
	TEST_EXPECT(!packed("body.tga") && !packed("body_src.tga") && !packed("skin_src.tga") && !packed("skin.tga"));
	pff::pff_close(&archive);
	std::printf("texture_store_dds: all checks passed\n");
	return 0;
}
