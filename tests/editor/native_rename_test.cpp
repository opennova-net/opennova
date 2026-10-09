// ADR 0046 S18: a file's rename rewrites the names a native text writes, a terrain's, an environment's, a
// particle file's and the HUD layout's, as text (graph/native_text_sites.h): only the token the game reads
// at each site, every other byte kept (a comment naming it, a line the game reads over, the line ends); a
// particle flipbook's frame refuses the rename (its name is the graphic's); a file stored in the SCR form
// is written back as plain text; the way back rewrites the same tokens. The retail leg (OPENNOVA_JO_DIR):
// every name the install's native texts write is found as its one token.
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include <base/vfs/vfs.h>
#include <base/vfs/vfs_decode.h>
#include <editor/assets/install_view.h>
#include <editor/assets/asset_import.h>
#include <editor/assets/asset_type_registry.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/native_text_sites.h>
#include <editor/graph/rename_transaction.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/scr/scr.h>
#include <formats/tga/tga.h>

#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;

namespace {

std::vector<uint8_t> tga_bytes() {
	const std::vector<uint8_t> rgba(16, 200);
	std::vector<uint8_t> out;
	std::string error;
	opennova::tga::tga_write_rgba32(rgba.data(), 2, 2, out, error);
	return out;
}

std::string read_text(const std::string &path) {
	std::string text, error;
	opennova::io::read_file_text(path, text, error);
	return text;
}

// The token rewrite alone, over each kind's text.
int test_rewrite_text() {
	std::vector<size_t> missed;
	// A terrain: the comment and the line the game reads over keep the name; the last line is the one read.
	std::string trn = "; polytrn_colormap map.tga, the old map\r\npolytrn_colormap \"map.tga\"\r\npolytrn_detailmap grain.tga\r\n"
	                  "polytrn_polydata isle.cpt\r\npolytrn_colormap map.tga ; the one read\r\n";
	TEST_EXPECT(rewrite_native_text("isle.trn", AssetKind::Terrain, "jo", trn, {{"Terrain", "polytrn_colormap", "map.tga", "land.tga"}},
	                                missed) == 1 &&
	            missed.empty());
	TEST_EXPECT(trn == "; polytrn_colormap map.tga, the old map\r\npolytrn_colormap \"map.tga\"\r\npolytrn_detailmap grain.tga\r\n"
	                   "polytrn_polydata isle.cpt\r\npolytrn_colormap land.tga ; the one read\r\n");
	// A particle file: two definitions name the same graphic, each a site; a comment between them keeps it.
	std::string ptl = "[particledef]\n{\n\tid = a;\n\tgraphic1 = spark.tga, additive;\n}\n// spark.tga\n[particledef]\n{\n"
	                  "\tid = b;\n\tgraphic1 = spark.tga,blend;\n\tgraphic2 = smoke.tga;\n}\n";
	TEST_EXPECT(rewrite_native_text("fx.ptl", AssetKind::Particles, "jo", ptl,
	                                {{"a", "graphic1", "spark.tga", "fire.tga"}, {"b", "graphic1", "spark.tga", "fire.tga"}},
	                                missed) == 2 &&
	            missed.empty());
	TEST_EXPECT(ptl == "[particledef]\n{\n\tid = a;\n\tgraphic1 = fire.tga, additive;\n}\n// spark.tga\n[particledef]\n{\n"
	                   "\tid = b;\n\tgraphic1 = fire.tga,blend;\n\tgraphic2 = smoke.tga;\n}\n");
	// The HUD layout: the stance's icon, and the static frame the HUD reads (the last line authored).
	std::string hud = "// stance.tga\r\nHUDSTANCE 0 10 10 stance.tga STAND\r\nSTATICFRAME frame.tga 0 0\r\nSTATICFRAME frame.tga 4 4\r\n";
	TEST_EXPECT(rewrite_native_text("hudpos.def", AssetKind::HudPosDefs, "jo", hud,
	                                {{"HUDSTANCE 0", "texture", "stance.tga", "pose.tga"}, {"StaticFrame", "texture", "frame.tga", "border.tga"}},
	                                missed) == 2 &&
	            missed.empty());
	TEST_EXPECT(hud == "// stance.tga\r\nHUDSTANCE 0 10 10 pose.tga STAND\r\nSTATICFRAME frame.tga 0 0\r\nSTATICFRAME border.tga 4 4\r\n");
	// A name the text does not read where the site says: missed, the text as it was.
	const std::string before = hud;
	TEST_EXPECT(rewrite_native_text("hudpos.def", AssetKind::HudPosDefs, "jo", hud, {{"", "hudls_moreav", "pose.tga", "x.tga"}},
	                                missed) == 0 &&
	            missed == std::vector<size_t>({0}) && hud == before);
	return 0;
}

// DI-17: where a native text writes a record's name (native_text_place), the line a Go to lands on: the
// token whose change changes that record's name alone (a comment naming it, an earlier line the game reads
// over and another record naming the same file are not it), found without case; two records on one line,
// each its own column; a particle file's effect by its id; nothing for a name the loader derives, a record or
// a field the text does not have, or the file alone.
int test_native_text_place() {
	size_t line = 0, column = 0;
	using editor_test::crlf;
	const std::string trn = "; polytrn_colormap map.tga, the old map\r\npolytrn_colormap \"map.tga\"\r\npolytrn_detailmap grain.tga\r\n"
	                        "polytrn_polydata isle.cpt\r\npolytrn_colormap MAP.tga ; the one read\r\n";
	TEST_EXPECT(native_text_place("isle.trn", AssetKind::Terrain, "jo", trn, "Terrain", "polytrn_colormap", line, column) &&
	            line == 5 && column == 18);
	TEST_EXPECT(native_text_place("isle.trn", AssetKind::Terrain, "jo", trn, "Terrain", "polytrn_detailmap", line, column) &&
	            line == 3 && column == 19);
	// Two heads name one model: each record's own line.
	const std::string avatars =
	        crlf("define head A\n{\n\tname\t\tAV_A\n\tgraphic\t\tboonie.3di\n\tcamo\t\t0 0 0\n\tvoice\t\t1\n\tsex\t\tm\n}\n\n"
	             "define head B\n{\n\tname\t\tAV_B\n\tgraphic\t\tboonie.3di\n\tcamo\t\t3 0 0\n\tvoice\t\t1\n\tsex\t\tm\n}\n");
	TEST_EXPECT(native_text_place("Avatars.def", AssetKind::AvatarDefs, "jo", avatars, "B", "graphic", line, column) &&
	            line == 13 && column == 11);
	TEST_EXPECT(native_text_place("Avatars.def", AssetKind::AvatarDefs, "jo", avatars, "A", "graphic", line, column) &&
	            line == 4 && column == 11);
	TEST_EXPECT(native_text_place("Avatars.def", AssetKind::AvatarDefs, "jo", avatars, "B", "name", line, column) && line == 12);
	// A record named alone: the first name the parser reads of it.
	TEST_EXPECT(native_text_place("Avatars.def", AssetKind::AvatarDefs, "jo", avatars, "B", "", line, column) && line == 13);
	// A face animation: each eye on the one line that writes both; the base texture's .MDT twin, a name the
	// loader derives and the text never writes, nowhere.
	std::string grm;
	TEST_EXPECT(test_io::read_file_text(std::string(test_paths_repo_root(__FILE__)) + "/fixtures/grm/person.grm", grm));
	TEST_EXPECT(native_text_place("person.grm", AssetKind::FaceAnimation, "jo", grm, "", "basetexture", line, column) &&
	            line == 5 && column == 16);
	TEST_EXPECT(native_text_place("person.grm", AssetKind::FaceAnimation, "jo", grm, "eye 1", "eyetexture", line, column) &&
	            line == 6 && column == 16);
	TEST_EXPECT(native_text_place("person.grm", AssetKind::FaceAnimation, "jo", grm, "eye 2", "eyetexture", line, column) &&
	            line == 6 && column == 28);
	TEST_EXPECT(!native_text_place("person.grm", AssetKind::FaceAnimation, "jo", grm, "", "basetexture.mdt", line, column));
	// A particle file's effect (a symbol the record names): its id's line.
	const std::string ptl = "[particledef]\n{\n\tid = spark;\n\tgraphic1 = spark.tga;\n}\n[effectdef]\n{\n\tid = Hit;\n"
	                        "\tpdefs = spark;\n}\n";
	Extracted read;
	Diagnostic error;
	TEST_EXPECT(extract_from_bytes("fx.ptl", AssetKind::Particles, std::vector<uint8_t>(ptl.begin(), ptl.end()), "jo", read, error) &&
	            read.symbols.size() == 1);
	TEST_EXPECT(native_text_place("fx.ptl", AssetKind::Particles, "jo", ptl, "Hit", "", line, column) && line == 8 && column == 7);
	// Nowhere: a record or field the text has not, the file alone.
	TEST_EXPECT(!native_text_place("Avatars.def", AssetKind::AvatarDefs, "jo", avatars, "C", "graphic", line, column));
	TEST_EXPECT(!native_text_place("isle.trn", AssetKind::Terrain, "jo", trn, "Terrain", "polytrn_charmap", line, column));
	TEST_EXPECT(!native_text_place("isle.trn", AssetKind::Terrain, "jo", trn, "", "", line, column));
	return 0;
}

// Through the session: each kind's sites rewritten by a file's rename, the frame refused, the SCR form.
int test_rename_in_native_texts() {
	editor_test::TempProjectDir dir{"opennova_editor_native_rename"};
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Native"));
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	const std::string root = view.project.root;
	for (const char *name : {"map.tga", "grain.tga", "spark.tga", "fx_01.tga", "fx_02.tga", "stance.tga", "cloud.pcx"})
		TEST_EXPECT(editor_test::write_bytes(root + "/textures/" + name, tga_bytes()));
	const std::string trn = "; map.tga\r\npolytrn_colormap map.tga\r\npolytrn_detailmap grain.tga\r\npolytrn_polydata isle.cpt\r\n";
	const std::string env = "sky_map1 cloud.pcx ; the near layer\r\nsky_map2 cloud.pcx\r\n";
	const std::string ptl = "[particledef]\r\n{\r\n\tid = a;\r\n\tgraphic1 = spark.tga, additive;\r\n}\r\n[particledef]\r\n{\r\n"
	                        "\tid = b;\r\n\tgraphic1 = fx.tga, blend;\r\n\tg1_flip_frames = 2;\r\n}\r\n";
	const std::string hud = "HUDSTANCE 0 10 10 stance.tga STAND\r\n";
	TEST_EXPECT(editor_test::write_text(root + "/terrains/isle.trn", trn) && editor_test::write_text(root + "/envs/day.env", env) &&
	            editor_test::write_text(root + "/effects/fx.ptl", ptl) && editor_test::write_text(root + "/hud/hudpos.def", hud));
	// The night's environment stored in the SCR form, as a retail archive may hold it.
	{
		std::string payload = "sky_map1 cloud.pcx\r\n";
		std::vector<uint8_t> stored{'S', 'C', 'R', 1};
		std::vector<uint8_t> body(payload.begin(), payload.end());
		opennova::scr::scr_encrypt(body.data(), body.size(), opennova::scr::SCR_KEY_JO_DFX2);
		stored.insert(stored.end(), body.begin(), body.end());
		TEST_EXPECT(editor_test::write_bytes(root + "/envs/night.env", stored));
	}
	editor_test::handle_to_end(session, request::rescan());
	session.run_operations();

	// The terrain's colour map: the line read, the comment kept.
	editor_test::handle_to_end(session, request::rename_asset("textures/map.tga", "land.tga"));
	TEST_EXPECT(session.outcome().done());
	TEST_EXPECT(read_text(root + "/terrains/isle.trn") ==
	            "; map.tga\r\npolytrn_colormap land.tga\r\npolytrn_detailmap grain.tga\r\npolytrn_polydata isle.cpt\r\n");
	// The two cloud layers of one environment, and the one of an environment stored in the SCR form,
	// which is written back as plain text.
	{
		const RenamePlan plan = plan_rename(ProjectPaths::for_root(root), *view.project.scan, *view.findings.graph, "textures/cloud.pcx",
		                                    "sky.pcx");
		TEST_EXPECT(plan.ok() && plan.sites.size() == 3);
	}
	editor_test::handle_to_end(session, request::rename_asset("textures/cloud.pcx", "sky.pcx"));
	TEST_EXPECT(session.outcome().done());
	TEST_EXPECT(read_text(root + "/envs/day.env") == "sky_map1 sky.pcx ; the near layer\r\nsky_map2 sky.pcx\r\n");
	TEST_EXPECT(read_text(root + "/envs/night.env") == "sky_map1 sky.pcx\r\n");
	// A particle's graphic.
	editor_test::handle_to_end(session, request::rename_asset("textures/spark.tga", "fire.tga"));
	TEST_EXPECT(session.outcome().done());
	TEST_EXPECT(read_text(root + "/effects/fx.ptl").find("graphic1 = fire.tga, additive;") != std::string::npos);
	// The HUD's stance icon; and the way back rewrites it again.
	editor_test::handle_to_end(session, request::rename_asset("textures/stance.tga", "pose.tga"));
	TEST_EXPECT(session.outcome().done());
	TEST_EXPECT(read_text(root + "/hud/hudpos.def") == "HUDSTANCE 0 10 10 pose.tga STAND\r\n");
	editor_test::handle_to_end(session, request::rename_back());
	TEST_EXPECT(session.outcome().done());
	TEST_EXPECT(read_text(root + "/hud/hudpos.def") == hud);
	// A text is the game's code page (Windows-1252): a new name written in it as such (an e acute one byte),
	// never as UTF-8; one the code page has no character of refused as the rename is planned.
	editor_test::handle_to_end(session, request::rename_asset("textures/grain.tga", "caf\xC3\xA9.tga"));
	TEST_EXPECT(session.outcome().done());
	TEST_EXPECT(read_text(root + "/terrains/isle.trn").find("polytrn_detailmap caf\xE9.tga\r\n") != std::string::npos);
	{
		const RenamePlan plan = plan_rename(ProjectPaths::for_root(root), *view.project.scan, *view.findings.graph, "textures/fire.tga",
		                                    "\xE6\x97\xA5.tga");
		TEST_EXPECT(!plan.ok() && plan.refusals.front().code() == "rename.name" &&
		            plan.refusals.front().message.find("Windows-1252") != std::string::npos);
	}
	// A flipbook's frame is named from its graphic's name: no one token is the frame's, so the rename is
	// refused and says why.
	{
		const RenamePlan plan = plan_rename(ProjectPaths::for_root(root), *view.project.scan, *view.findings.graph, "textures/fx_01.tga",
		                                    "fx_09.tga");
		TEST_EXPECT(!plan.ok() && plan.refusals.front().code() == "rename.site" &&
		            plan.refusals.front().message.find("a frame of graphic1") != std::string::npos);
	}
	editor_test::handle_to_end(session, request::rename_asset("textures/fx_01.tga", "fx_09.tga"));
	TEST_EXPECT(view.findings.diagnostics.back().code() == "rename.site");
	TEST_EXPECT(read_text(root + "/textures/fx_01.tga").size() == tga_bytes().size() && read_text(root + "/textures/fx_09.tga").empty());
	TEST_EXPECT(read_text(root + "/effects/fx.ptl").find("graphic1 = fx.tga, blend;") != std::string::npos);
	// Every name the renames rewrote reaches its file.
	const std::vector<const GraphEdge *> missing = view.findings.graph->missing();
	for (const char *name : {"land.tga", "sky.pcx", "fire.tga", "stance.tga"})
		TEST_EXPECT(std::none_of(missing.begin(), missing.end(), [&](const GraphEdge *edge) { return edge->value == name; }));
	std::printf("native renames: a terrain's, an environment's (and one in the SCR form), a particle's, the HUD's; "
	            "the way back; a frame refused\n");
	return 0;
}

// Every name the install's native texts write (each terrain, environment, particle file and the HUD
// layout) found as its token: renamed alone in its file, the text reads it back as the new name and
// nothing else changed.
int test_retail() {
	const std::string install = retail::install();
	if (install.empty()) {
		retail::skip_leg("OPENNOVA_JO_DIR (the install's native texts renamed in)");
		return 0;
	}
	ProjectDocument project;
	project.target_game = "jo";
	InstallView install_view;
	std::string why;
	TEST_EXPECT(install_view.open(install_spec(install, project), why));
	const opennova::Vfs &mount = install_view.vfs();
	size_t files = 0, names = 0, found = 0;
	for (const opennova::VfsFileLocation &location : mount.list_files()) {
		const std::string &name = location.logical_name;
		const AssetKind kind = classify_asset(name, nullptr);
		std::vector<uint8_t> bytes;
		if (!native_text_kind(kind) || !mount.read_file_raw(name, bytes)) continue;
		opennova::vfs_decode_payload(bytes, opennova::VFS_SCR_FORCE_JO_DFX2);
		Extracted extracted;
		Diagnostic error;
		if (!extract_from_bytes(name, kind, bytes, "jo", extracted, error)) continue;
		++files;
		const std::string text(bytes.begin(), bytes.end());
		for (const GraphEdge &edge : extracted.edges) {
			if (!edge.rewritable) continue;
			++names;
			std::string renamed = text;
			std::vector<size_t> missed;
			if (rewrite_native_text(name, kind, "jo", renamed, {{edge.record, edge.field, edge.value, "q" + edge.value}}, missed) == 1)
				++found;
			else
				std::fprintf(stderr, "retail: %s: %s %s '%s' not found as one token\n", name.c_str(), edge.record.c_str(),
				             edge.field.c_str(), edge.value.c_str());
		}
	}
	std::printf("retail: %zu native texts, %zu of their %zu names found as one token\n", files, found, names);
	TEST_EXPECT(files > 100 && names > 1000 && found == names);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	int failures = test_rewrite_text();
	failures += test_native_text_place();
	failures += test_rename_in_native_texts();
	failures += test_retail();
	if (failures == 0) std::printf("editor_native_rename: all passed\n");
	return failures == 0 ? 0 : 1;
}
