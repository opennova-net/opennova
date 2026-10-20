// ADR 0046 S18: a file's rename rewrites the names a native text writes, a terrain's, an environment's, a
// particle file's and the HUD layout's, as text (graph/native_text_sites.h): only the token the game reads
// at each site, every other byte kept (a comment naming it, a line the game reads over, the line ends); a
// particle flipbook's frame refuses the rename (its name is the graphic's); a file stored in the SCR form
// is written back as plain text; the way back rewrites the same tokens. Every name a native text writes is
// found as its one token, over a text of each kind holding every form the shipped texts write a name in and
// over the native texts of a game install minted from OpenNova's own game.
#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
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
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/minted_install.h"
#include "editor/test_platform.h"

using namespace opennova::editor;

namespace {

using editor_test::tga_bytes;

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
// over and another record naming the same file are not it: two vehicle panels' one icon), found without case;
// two records on one line, each its own column; a particle file's effect by its id; nothing for a name the
// loader derives, a record or a field the text does not have, or the file alone.
int test_native_text_place() {
	size_t line = 0, column = 0;
	using test_io::crlf;
	const std::string trn = "; polytrn_colormap map.tga, the old map\r\npolytrn_colormap \"map.tga\"\r\npolytrn_detailmap grain.tga\r\n"
	                        "polytrn_polydata isle.cpt\r\npolytrn_colormap MAP.tga ; the one read\r\n";
	TEST_EXPECT(native_text_place("isle.trn", AssetKind::Terrain, "jo", trn, "Terrain", "polytrn_colormap", line, column) &&
	            line == 5 && column == 18);
	TEST_EXPECT(native_text_place("isle.trn", AssetKind::Terrain, "jo", trn, "Terrain", "polytrn_detailmap", line, column) &&
	            line == 3 && column == 19);
	// Two vehicle panels name one icon: each record's own line.
	const std::string hudpos = crlf("VEHICLE_HUD\n  sid BUGGY1\n  icon v_icon.tga\n  statictexture v_static.tga\nVEHICLE_END\n\n"
	                                "VEHICLE_HUD\n  sid BUGGY2\n  icon v_icon.tga\n  statictexture v_static2.tga\nVEHICLE_END\n");
	TEST_EXPECT(native_text_place("hudpos.def", AssetKind::HudPosDefs, "jo", hudpos, "VEHICLE_HUD BUGGY2", "icon", line, column) &&
	            line == 9 && column == 8);
	TEST_EXPECT(native_text_place("hudpos.def", AssetKind::HudPosDefs, "jo", hudpos, "VEHICLE_HUD BUGGY1", "icon", line, column) &&
	            line == 3 && column == 8);
	TEST_EXPECT(native_text_place("hudpos.def", AssetKind::HudPosDefs, "jo", hudpos, "VEHICLE_HUD BUGGY2", "statictexture", line,
	                              column) &&
	            line == 10 && column == 17);
	// (A face animation is a record document since round S23: a Go to lands on its face's field, not a line.)
	// A particle file's effect (a symbol the record names): its id's line.
	const std::string ptl = "[particledef]\n{\n\tid = spark;\n\tgraphic1 = spark.tga;\n}\n[effectdef]\n{\n\tid = Hit;\n"
	                        "\tpdefs = spark;\n}\n";
	Extracted read;
	Diagnostic error;
	TEST_EXPECT(extract_from_bytes("fx.ptl", AssetKind::Particles, std::vector<uint8_t>(ptl.begin(), ptl.end()), "jo", read, error) &&
	            read.symbols.size() == 1);
	TEST_EXPECT(native_text_place("fx.ptl", AssetKind::Particles, "jo", ptl, "Hit", "", line, column) && line == 8 && column == 7);
	// Nowhere: a record or field the text has not, the file alone.
	TEST_EXPECT(!native_text_place("hudpos.def", AssetKind::HudPosDefs, "jo", hudpos, "VEHICLE_HUD C", "icon", line, column));
	TEST_EXPECT(!native_text_place("isle.trn", AssetKind::Terrain, "jo", trn, "Terrain", "polytrn_charmap", line, column));
	TEST_EXPECT(!native_text_place("isle.trn", AssetKind::Terrain, "jo", trn, "", "", line, column));
	return 0;
}

// Through the session: each kind's sites rewritten by a file's rename, the frame refused, the SCR form.
int test_rename_in_native_texts() {
	test_temp::TempDir dir{"opennova_editor_native_rename"};
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Native"));
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	const std::string root = view.project.root;
	for (const char *name : {"map.tga", "grain.tga", "spark.tga", "fx_01.tga", "fx_02.tga", "stance.tga", "cloud.pcx", "logo.tga"})
		TEST_EXPECT(test_io::write_file(root + "/textures/" + name, tga_bytes()));
	const std::string trn = "; map.tga\r\npolytrn_colormap map.tga\r\npolytrn_detailmap grain.tga\r\npolytrn_polydata isle.cpt\r\n";
	const std::string env = "sky_map1 cloud.pcx ; the near layer\r\nsky_map2 cloud.pcx\r\n";
	const std::string dusk = "sky_map2 logo.tga\r\n";
	const std::string ptl = "[particledef]\r\n{\r\n\tid = a;\r\n\tgraphic1 = spark.tga, additive;\r\n}\r\n[particledef]\r\n{\r\n"
	                        "\tid = b;\r\n\tgraphic1 = fx.tga, blend;\r\n\tg1_flip_frames = 2;\r\n}\r\n";
	const std::string hud = "HUDSTANCE 0 10 10 stance.tga STAND\r\n";
	TEST_EXPECT(test_io::write_text(root + "/terrains/isle.trn", trn) && test_io::write_text(root + "/envs/day.env", env) &&
	            test_io::write_text(root + "/envs/dusk.env", dusk) &&
	            test_io::write_text(root + "/effects/fx.ptl", ptl) && test_io::write_text(root + "/hud/hudpos.def", hud));
	// The night's environment stored in the SCR form, as a retail archive may hold it.
	{
		std::string payload = "sky_map1 cloud.pcx\r\n";
		std::vector<uint8_t> stored{'S', 'C', 'R', 1};
		std::vector<uint8_t> body(payload.begin(), payload.end());
		opennova::scr::scr_encrypt(body.data(), body.size(), opennova::scr::SCR_KEY_JO_DFX2);
		stored.insert(stored.end(), body.begin(), body.end());
		TEST_EXPECT(test_io::write_file(root + "/envs/night.env", stored));
	}
	editor_test::handle_to_end(session, request::rescan());
	session.run_operations();

	// The terrain's colour map: the line read, the comment kept.
	editor_test::handle_to_end(session, request::rename_asset("textures/map.tga", "land.tga"));
	TEST_EXPECT(session.outcome().done());
	TEST_EXPECT(test_io::read_file_text(root + "/terrains/isle.trn") ==
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
	TEST_EXPECT(test_io::read_file_text(root + "/envs/day.env") == "sky_map1 sky.pcx ; the near layer\r\nsky_map2 sky.pcx\r\n");
	TEST_EXPECT(test_io::read_file_text(root + "/envs/night.env") == "sky_map1 sky.pcx\r\n");
	// A sky map's name is made .pcx as the game parses it, so an environment naming a TGA uses no TGA: the
	// TGA's rename leaves it as it is.
	editor_test::handle_to_end(session, request::rename_asset("textures/logo.tga", "logo2.tga"));
	TEST_EXPECT(session.outcome().done() && test_io::read_file_text(root + "/textures/logo2.tga").size() == tga_bytes().size());
	TEST_EXPECT(test_io::read_file_text(root + "/envs/dusk.env") == dusk);
	// A particle's graphic.
	editor_test::handle_to_end(session, request::rename_asset("textures/spark.tga", "fire.tga"));
	TEST_EXPECT(session.outcome().done());
	TEST_EXPECT(test_io::read_file_text(root + "/effects/fx.ptl").find("graphic1 = fire.tga, additive;") != std::string::npos);
	// The HUD's stance icon; and the way back rewrites it again.
	editor_test::handle_to_end(session, request::rename_asset("textures/stance.tga", "pose.tga"));
	TEST_EXPECT(session.outcome().done());
	TEST_EXPECT(test_io::read_file_text(root + "/hud/hudpos.def") == "HUDSTANCE 0 10 10 pose.tga STAND\r\n");
	editor_test::handle_to_end(session, request::rename_back());
	TEST_EXPECT(session.outcome().done());
	TEST_EXPECT(test_io::read_file_text(root + "/hud/hudpos.def") == hud);
	// A text is the game's code page (Windows-1252): a new name written in it as such (an e acute one byte),
	// never as UTF-8; one the code page has no character of refused as the rename is planned.
	editor_test::handle_to_end(session, request::rename_asset("textures/grain.tga", "caf\xC3\xA9.tga"));
	TEST_EXPECT(session.outcome().done());
	TEST_EXPECT(test_io::read_file_text(root + "/terrains/isle.trn").find("polytrn_detailmap caf\xE9.tga\r\n") != std::string::npos);
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
	TEST_EXPECT(test_io::read_file_text(root + "/textures/fx_01.tga").size() == tga_bytes().size() && test_io::read_file_text(root + "/textures/fx_09.tga").empty());
	TEST_EXPECT(test_io::read_file_text(root + "/effects/fx.ptl").find("graphic1 = fx.tga, blend;") != std::string::npos);
	// Every name the renames rewrote reaches its file.
	const std::vector<const GraphEdge *> missing = view.findings.graph->missing();
	for (const char *name : {"land.tga", "sky.pcx", "fire.tga", "stance.tga"})
		TEST_EXPECT(std::none_of(missing.begin(), missing.end(), [&](const GraphEdge *edge) { return edge->value == name; }));
	std::printf("native renames: a terrain's, an environment's (and one in the SCR form), a particle's, the HUD's; "
	            "the way back; a frame refused\n");
	return 0;
}

// The texts each form of name the shipped native texts write is written in, one text of each kind (the 3,016
// names of JO:CA's 126 terrains, environments, particle files and HUD layout, 3,144 with its expansion's,
// read once, 2026-10-19, each found as its token; and the forms of its particle files' curve keys, names
// since): an environment's sky maps and celestial models, each padded
// by spaces to a comment, its star model's key left without one (no default named); the HUD layout's fonts and
// frame after a tab, its icons before their place, a stance's icon
// among its numbers, a vehicle panel's interface at its line's end and before trailing spaces; a particle
// file's graphics after `=` and a tab, before a comma, one named by two definitions (the first token of it
// another site), and its curves after a tab and `=`, before the `;` or a `reverse` or `inverse` word, one table
// named by two definitions; a terrain's maps padded by spaces or a tab, each at its line's end or before a tab and a
// comment, and a foliage block's model before its comment, two blocks naming one model. Every line ends CR LF,
// as every shipped one does.
std::vector<std::pair<std::string, std::string>> corpus_forms() {
	using test_io::crlf;
	return {
	        {"forms.env", crlf("sky_map1          cloud01.pcx           ;cloud pcx - the full range layer\n"
	                           "sky_map2          cloud01b.pcx          ;cloud pcx, its modulation layer\n"
	                           "sun_3di             sun.3di              ;sun object\n"
	                           "moon_3di          moon4.3di            ;moon object\n"
	                           "glare_3di          glare.3di            ;glare object\n"
	                           "star_3di                                ;star object, none\n")},
	        {"hudpos.def", crlf("fonthud1_hi\tbig18b.fnt\nfonthud1_lo\tsmall14.fnt\n"
	                            "StaticFrame\tframe.tga  2,570\t\nParachuteIcon\tchute.tga  10,659\t\n"
	                            "ArmorIcon\t\tarmor.tga    10,659\t\nHUDSTANCE 0\t0 0 stance_1.tga STAND\t\n"
	                            "VEHICLE_HUD \n  sid Dbuggy1  \n  interface h_buggy.tga\n  driver 51,103\nVEHICLE_END\n\n"
	                            "VEHICLE_HUD \n  sid Dtruck1 \n  interface h_truck.tga  \n  driver 51,103\nVEHICLE_END\n")},
	        {"forms.ptl", crlf("[particledef]\n{\n\tid = puff_a;\n\tscale_func\t= grow_t;\n\tgraphic1 =\tpuff.tga, blend;\n"
	                           "\tg1_alpha_func\t= fade_t reverse;\n\tgraphic2 =\tthin.tga, blend;\n}\n"
	                           "[particledef]\n{\n\tid = puff_b;\n\talpha_func\t= fade_t;\n\tgraphic1 =\tpuff.tga, blend;\n}\n"
	                           "[particledef]\n{\n\tid = flash;\n\tgraphic1 =\tflash.tga, additive;\n\tg1_scale_func\t= grow_t inverse;\n}\n")},
	        {"forms.trn", crlf("terrain_name     \"Forms\"\n"
	                           "polytrn_colormap         forms_c.tga\n"
	                           "polytrn_detailmap        det_a.tga\t; grayscale base detail map\n"
	                           "polytrn_detailmap_c1     det_g.tga\t; color grass\n"
	                           "polytrn_detailmap_c2     det_d.tga\n"
	                           "polytrn_detailmap_c3     det_r.tga\n"
	                           "polytrn_detailmap2       det_b.tga\t; grayscale distance detail map\n"
	                           "polytrn_detailmapdist    det_c.tga\n"
	                           "polytrn_detailblendmap   forms_m2.tga\n"
	                           "polytrn_polydata     \tforms.cpt\n"
	                           "polytrn_tilestrip   \ttiles10.tga\n"
	                           "polytrn_charmap     \tforms_m.pcx\n"
	                           "polytrn_foliagemap  \tforms_f.pcx\n"
	                           "foliage\n  graphic         veg5.3di                 ; the model it places\n  match 254\nend\n"
	                           "foliage\n  graphic         veg5.3di                 ; the same model again\n  match 253\nend\n"
	                           "foliage\n  graphic         veg6.3di\n  match 252\nend\n")},
	};
}

// Every name a native text writes (a terrain, an environment, a particle file, the HUD layout) found as its one
// token: renamed alone in its file, the text reads it back as the new name and nothing else changed. Over the
// corpus' forms (corpus_forms, each kind's text naming as many names as it writes forms, the environment's also
// stored in the SCR form, as an archive may hold it) and the native texts of a game install minted from
// OpenNova's own game (minted_install.h, its terrain the importer's .trn), each read as the game reads it (its
// payload decoded).
int test_every_name_one_token() {
	// Each text, and how many names its forms write (0: a text of the install's, its names its own).
	struct Text {
		std::string name;
		std::vector<uint8_t> bytes;
		size_t forms;
	};
	std::vector<Text> texts;
	const size_t written[] = {5, 8, 8, 15};
	for (size_t i = 0; i < corpus_forms().size(); ++i) {
		const std::string text = corpus_forms()[i].second;
		texts.push_back({corpus_forms()[i].first, std::vector<uint8_t>(text.begin(), text.end()), written[i]});
	}
	{
		const std::string env = corpus_forms()[0].second;
		std::vector<uint8_t> stored{'S', 'C', 'R', 1};
		std::vector<uint8_t> body(env.begin(), env.end());
		opennova::scr::scr_encrypt(body.data(), body.size(), opennova::scr::SCR_KEY_JO_DFX2);
		stored.insert(stored.end(), body.begin(), body.end());
		texts.push_back({"stored.env", stored, written[0]});
	}
	test_temp::TempDir dir("opennova_editor_native_rename_install");
	const std::string install = dir.file("install");
	std::string why;
	TEST_EXPECT(minted_install::mint(install, why));
	ProjectDocument project;
	project.target_game = "jo";
	InstallView install_view;
	TEST_EXPECT(install_view.open(install_spec(install, project), why));
	const opennova::Vfs &mount = install_view.vfs();
	size_t from_install = 0;
	for (const opennova::VfsFileLocation &location : mount.list_files()) {
		std::vector<uint8_t> bytes;
		if (!native_text_kind(classify_asset(location.logical_name, nullptr)) || !mount.read_file_raw(location.logical_name, bytes))
			continue;
		texts.push_back({location.logical_name, bytes, 0});
		++from_install;
	}
	TEST_EXPECT(from_install >= 8); // its environments, particle files, HUD layout and terrain
	size_t names = 0, found = 0;
	for (Text &each : texts) {
		const std::string &name = each.name;
		std::vector<uint8_t> &bytes = each.bytes;
		const AssetKind kind = classify_asset(name, nullptr);
		opennova::vfs_decode_payload(bytes, opennova::VFS_SCR_FORCE_JO_DFX2);
		Extracted extracted;
		Diagnostic error;
		TEST_EXPECT(native_text_kind(kind) && extract_from_bytes(name, kind, bytes, "jo", extracted, error));
		const std::string text(bytes.begin(), bytes.end());
		size_t sites = 0;
		for (const GraphEdge &edge : extracted.edges) {
			if (!edge.rewritable) continue;
			++sites;
			++names;
			std::string renamed = text;
			std::vector<size_t> missed;
			if (rewrite_native_text(name, kind, "jo", renamed, {{edge.record, edge.field, edge.value, "q" + edge.value}}, missed) == 1)
				++found;
			else
				std::fprintf(stderr, "%s: %s %s '%s' not found as one token\n", name.c_str(), edge.record.c_str(), edge.field.c_str(),
				             edge.value.c_str());
		}
		if (each.forms != 0 && sites != each.forms)
			std::fprintf(stderr, "%s: %zu names read where its forms write %zu\n", name.c_str(), sites, each.forms);
		TEST_EXPECT(each.forms == 0 || sites == each.forms);
	}
	std::printf("native texts: %zu (%zu of the minted install's), %zu names, %zu found as one token\n", texts.size(), from_install,
	            names, found);
	TEST_EXPECT(names > 50 && found == names);
	return 0;
}

} // namespace

int main() {
	int failures = 0;
	failures += test_rewrite_text();
	failures += test_native_text_place();
	failures += test_rename_in_native_texts();
	failures += test_every_name_one_token();
	if (failures == 0) std::printf("editor_native_rename: all passed\n");
	return failures == 0 ? 0 : 1;
}
