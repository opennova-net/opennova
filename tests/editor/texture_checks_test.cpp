// ADR 0046 S18, the texture checks: a texture file's own findings (validate_texture_file, from its
// header: a TGA form the game's reader leaves unset or zeroes, rows stored top first, a colour map read
// as texels, an odd-width PCX's rows overrunning, a file the reader refuses) and its uses' (the texture use
// check, graph/texture_checks: a terrain's colour map not 1024 x 1024, a foliage map overrun or of the
// wrong shape, a tile atlas not in 64-texel cells, a cut-out material over a PCX, a normal map halved, a
// height map whose side is no power of two, a particle graphic too wide, a colour map of a format its
// loader does not read, a .tga beside the .dds its loader opens, the default loading screen's size); which
// of them refuse a build. Over a minted project, every texture written here. The retail leg
// (OPENNOVA_JO_DIR): every texture the install ships, its own findings none that refuse a build, and every
// shipped terrain's colour map 1024 x 1024.
#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <base/vfs/vfs.h>
#include <editor/assets/install_view.h>
#include <editor/assets/asset_import.h>
#include <editor/assets/asset_type_registry.h>
#include <editor/documents/texture_document.h>
#include <editor/graph/reference_kinds.h>
#include <editor/graph/texture_checks.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/tga/tga.h>
#include <formats/trn/trn_io.h>

#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;

namespace {

std::vector<uint8_t> tga(uint32_t w, uint32_t h, uint8_t alpha = 255) {
	const std::vector<uint8_t> rgba(size_t(w) * h * 4, alpha);
	std::vector<uint8_t> out;
	std::string error;
	opennova::tga::tga_write_rgba32(rgba.data(), w, h, out, error);
	return out;
}

// An 18-byte TGA header of a type, a depth and a descriptor, a colour map of `map` entries, and texels.
std::vector<uint8_t> tga_header(uint8_t type, uint8_t bits, uint8_t descriptor, uint16_t map = 0) {
	std::vector<uint8_t> out(18, 0);
	out[1] = map ? 1 : 0;
	out[2] = type;
	out[5] = uint8_t(map);
	out[6] = uint8_t(map >> 8);
	out[7] = map ? 24 : 0;
	out[12] = 2;
	out[14] = 2;
	out[16] = bits;
	out[17] = descriptor;
	out.resize(out.size() + size_t(map) * 3 + 64, 0);
	return out;
}

// A PCX header of `w` x `h` at 8 bits in one plane, `line` bytes a line, then texels and a palette.
std::vector<uint8_t> pcx(uint16_t w, uint16_t h, uint16_t line) {
	std::vector<uint8_t> out(128, 0);
	out[0] = 0x0A;
	out[1] = 5;
	out[2] = 1;
	out[3] = 8;
	out[8] = uint8_t(w - 1);
	out[9] = uint8_t((w - 1) >> 8);
	out[10] = uint8_t(h - 1);
	out[11] = uint8_t((h - 1) >> 8);
	out[65] = 1;
	out[66] = uint8_t(line);
	out[67] = uint8_t(line >> 8);
	out.resize(out.size() + size_t(line) * h, 0);
	out.push_back(0x0C);
	out.resize(out.size() + 768, 0);
	return out;
}

std::string trn(const std::string &keys) {
	return "polytrn_detailmap detail.tga\npolytrn_polydata isle.cpt\npolytrn_sectorcount 1\npolytrn_sectors 0\n" + keys;
}

struct Found {
	std::string code, asset, field;
	DiagnosticSeverity severity;
	bool blocks;
	std::string message;
};

int test_checks() {
	editor_test::TempProjectDir dir{"opennova_editor_texture_checks"};
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Checks"));
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	const std::string root = view.project.root;
	const std::string t = root + "/textures/";
	// The files' own: a top-left TGA, a 16-bit grey one, a 16-bit run-length one, a true-colour one with
	// a colour map, an odd-width PCX, a 4-bit PCX.
	std::vector<uint8_t> top = tga(4, 4);
	top[17] |= 0x20;
	TEST_EXPECT(editor_test::write_bytes(t + "top.tga", top) && editor_test::write_bytes(t + "grey16.tga", tga_header(3, 16, 0)) &&
	            editor_test::write_bytes(t + "rle16.tga", tga_header(10, 16, 0)) &&
	            editor_test::write_bytes(t + "mapped.tga", tga_header(2, 24, 0, 4)) &&
	            editor_test::write_bytes(t + "odd.pcx", pcx(5, 4, 6)));
	std::vector<uint8_t> four = pcx(4, 4, 4);
	four[3] = 4;
	// A 24-bit one (three planes) the reader takes, its odd width no overrun: no finding.
	std::vector<uint8_t> rgb = pcx(5, 4, 6);
	rgb[65] = 3;
	TEST_EXPECT(editor_test::write_bytes(t + "four.pcx", four) && editor_test::write_bytes(t + "rgb.pcx", rgb));
	// A TGA whose file ends before its texels do, as raw true colour and as run-length packets; a PCX whose
	// rows are shorter than its width.
	std::vector<uint8_t> cut = tga(4, 4);
	cut.resize(18 + 10);
	std::vector<uint8_t> cut_rle = tga_header(10, 32, 0);
	cut_rle.resize(18);
	cut_rle.insert(cut_rle.end(), {0x81, 1, 2, 3, 4});
	TEST_EXPECT(editor_test::write_bytes(t + "cut.tga", cut) && editor_test::write_bytes(t + "cutrle.tga", cut_rle) &&
	            editor_test::write_bytes(t + "short.pcx", pcx(6, 4, 4)));
	// The uses': terrains, a model, a particle file, the default loading screen.
	TEST_EXPECT(editor_test::write_bytes(t + "detail.tga", tga(8, 8)) && editor_test::write_bytes(t + "small.tga", tga(512, 512)) &&
	            editor_test::write_bytes(t + "wide.tga", tga(2048, 512)) && editor_test::write_bytes(t + "tall.tga", tga(512, 2048)) &&
	            editor_test::write_bytes(t + "colour.tga", tga(1024, 1024)) && editor_test::write_bytes(t + "map.pcx", pcx(8, 8, 8)) &&
	            editor_test::write_bytes(t + "fol.pcx", pcx(64, 32, 64)) && editor_test::write_bytes(t + "fol48.pcx", pcx(48, 48, 48)) &&
	            editor_test::write_bytes(t + "fol2k.pcx", pcx(2048, 1024, 2048)) &&
	            editor_test::write_bytes(t + "blendw.tga", tga(64, 32)) && editor_test::write_bytes(t + "blendt.tga", tga(32, 64)) &&
	            editor_test::write_bytes(t + "tiles.tga", tga(100, 64)) && editor_test::write_bytes(t + "skin.pcx", pcx(8, 8, 8)) &&
	            editor_test::write_bytes(t + "big.mdt", tga(1024, 16)) && editor_test::write_bytes(t + "bump.tga", tga(100, 64)) &&
	            editor_test::write_bytes(t + "wall.tga", tga(8, 8)) && editor_test::write_bytes(t + "wall.dds", tga(8, 8)) &&
	            editor_test::write_bytes(t + "huge.tga", tga(1024, 8)) && editor_test::write_bytes(t + "page.tga", tga(256, 16)) &&
	            editor_test::write_bytes(t + "fits.tga", tga(255, 256)) &&
	            editor_test::write_bytes(t + "loadscrn.pcx", pcx(640, 480, 640)));
	// A VS_DOT3DIFF2 material cuts out by its normal map's alpha: its PCX diffuse is no finding, its 24-bit
	// normal map is.
	{
		const std::vector<uint8_t> rgba(16 * 4, 200);
		std::vector<uint8_t> rgb;
		std::string error;
		opennova::tga::tga_write_rgb24(rgba.data(), 4, 4, rgb, error);
		TEST_EXPECT(editor_test::write_bytes(t + "dotskin.pcx", pcx(8, 8, 8)) && editor_test::write_bytes(t + "dotdet.tga", tga(8, 8)) &&
		            editor_test::write_bytes(t + "dotnorm.mdt", rgb));
	}
	TEST_EXPECT(editor_test::write_text(root + "/terrains/a.trn", trn("polytrn_colormap small.tga\n")) &&
	            editor_test::write_text(root + "/terrains/b.trn", trn("polytrn_colormap wide.tga\npolytrn_foliagemap fol.pcx\n")) &&
	            editor_test::write_text(root + "/terrains/c.trn", trn("polytrn_colormap tall.tga\npolytrn_foliagemap fol48.pcx\n")) &&
	            editor_test::write_text(root + "/terrains/d.trn", trn("polytrn_colormap map.pcx\npolytrn_tilestrip tiles.tga\n")) &&
	            editor_test::write_text(root + "/terrains/e.trn", trn("polytrn_colormap colour.tga\n")) &&
	            editor_test::write_text(root + "/terrains/f.trn", trn("polytrn_colormap colour.tga\npolytrn_detailblendmap blendw.tga\n"
	                                                                  "polytrn_foliagemap fol2k.pcx\n")) &&
	            editor_test::write_text(root + "/terrains/g.trn", trn("polytrn_colormap colour.tga\npolytrn_detailblendmap blendt.tga\n")) &&
	            // A splat detail read by STAGE (its .dds) and by its own name through the TGA reader: no "never
	            // read" on its .tga. A detail map whose .tga the project lacks: no coefficient made of its .dds.
	            editor_test::write_text(root + "/terrains/h.trn",
	                                    "polytrn_detailmap cdet.tga\npolytrn_polydata isle.cpt\npolytrn_sectorcount 1\npolytrn_sectors 0\n"
	                                    "polytrn_colormap colour.tga\npolytrn_detailmap_c1 det.tga\n") &&
	            editor_test::write_bytes(t + "det.tga", tga(8, 8)) && editor_test::write_bytes(t + "det.dds", tga(8, 8)) &&
	            editor_test::write_bytes(t + "cdet.dds", tga(8, 8)) &&
	            editor_test::write_text(root + "/fx.ptl",
	                                    "[effectdef]\n{\n\tid = BOOM;\n\tpdefs = puff, dent, ring;\n}\n\n[particledef]\n{\n\tid = "
	                                    "puff;\n\tgraphic1 = huge.tga, additive;\n}\n\n[particledef]\n{\n\tid = dent;\n"
	                                    "\tgraphic1 = page.tga, bump;\n}\n\n[particledef]\n{\n\tid = ring;\n"
	                                    "\tgraphic1 = fits.tga, distort;\n}\n"));
	const std::string scene = dir.file("scene");
	TEST_EXPECT(editor_test::write_text(scene + "/thing.o3d",
	                                    "o3d 2\nmodel THING\nmaterial VS_SKBASIC\nmatflags 1\nalphatest 128\n"
	                                    "texture skin.pcx 1 0\ntexture wall.tga 2 0\nmaterial FF_ST_OP\ntexture big.mdt 3 4\n"
	                                    "texture bump.tga 3 5\nmaterial VS_DOT3DIFF2\nmatflags 1\nalphatest 64\n"
	                                    "texture dotskin.pcx 1 0\ntexture dotdet.tga 2 0\ntexture dotnorm.mdt 3 4\n"
	                                    "lod 0\npart 0 0 0 0\nmesh 0 0\n"
	                                    "v 0 0 0 0 0 1 0 0\nv 1 0 0 0 0 1 1 0\nv 0 1 0 0 0 1 0 1\nt 0 1 2\n"));
	ImportChoice model;
	model.path = scene + "/thing.o3d";
	const ImportResult imported = import_assets({model}, ProjectPaths::for_root(root), *view.project.document, false);
	TEST_EXPECT(imported.imported == std::vector<std::string>({"models/thing.3di"}));
	editor_test::handle_to_end(session, request::rescan());
	session.run_operations();
	std::vector<Found> found;
	for (const Diagnostic &d : view.findings.diagnostics)
		if (d.code().rfind("texture.", 0) == 0) found.push_back({d.code(), d.asset, d.field, d.severity, blocks_build(d), d.message});
	const auto has = [&](const char *code, const std::string &asset, DiagnosticSeverity severity, bool blocks) {
		for (const Found &f : found)
			if (f.code == code && f.asset == asset && f.severity == severity && f.blocks == blocks) return true;
		std::fprintf(stderr, "no %s on %s\n", code, asset.c_str());
		return false;
	};
	using S = DiagnosticSeverity;
	// The files' own.
	TEST_EXPECT(has("texture.tga_upside_down", "textures/top.tga", S::Warning, false));
	TEST_EXPECT(has("texture.tga_unfilled", "textures/grey16.tga", S::Error, true));
	TEST_EXPECT(has("texture.tga_zeroed", "textures/rle16.tga", S::Warning, false));
	TEST_EXPECT(has("texture.tga_colour_map_skipped", "textures/mapped.tga", S::Warning, false));
	TEST_EXPECT(has("texture.pcx_overrun", "textures/odd.pcx", S::Error, true));
	TEST_EXPECT(has("texture.pcx_short_rows", "textures/short.pcx", S::Warning, false));
	TEST_EXPECT(has("texture.tga_truncated", "textures/cut.tga", S::Error, true));
	TEST_EXPECT(has("texture.tga_truncated", "textures/cutrle.tga", S::Error, true));
	TEST_EXPECT(has("texture.unloadable", "textures/four.pcx", S::Warning, false));
	for (const Found &f : found) TEST_EXPECT(f.asset != "textures/rgb.pcx");
	// The odd width's words say how many bytes spill, not that the width is odd.
	for (const Found &f : found)
		if (f.code == "texture.pcx_overrun") TEST_EXPECT(f.message.find("each row's 1 extra byte") != std::string::npos);
	// The uses'. A colour map: read short (512 x 512), wider than tall (the quadrant split overruns it and its
	// buffer), taller than wide (neither: only its first rows matter).
	TEST_EXPECT(has("texture.colormap_size", "terrains/a.trn", S::Error, true));
	TEST_EXPECT(has("texture.colormap_size", "terrains/b.trn", S::Error, true));
	TEST_EXPECT(has("texture.colormap_size", "terrains/c.trn", S::Warning, false));
	TEST_EXPECT(has("texture.wrong_reader", "terrains/d.trn", S::Error, true));
	TEST_EXPECT(has("texture.foliage_map_overrun", "terrains/b.trn", S::Error, true));
	TEST_EXPECT(has("texture.foliage_map_shape", "terrains/c.trn", S::Warning, false));
	// A foliage map wider than 1024 reads its first texel everywhere: its shape, no overrun.
	TEST_EXPECT(has("texture.foliage_map_shape", "terrains/f.trn", S::Warning, false));
	// A blend map wider than tall overruns the split; a taller one does not.
	TEST_EXPECT(has("texture.blend_map_size", "terrains/f.trn", S::Error, true));
	for (const Found &f : found)
		TEST_EXPECT(f.code != "texture.foliage_map_overrun" || f.asset != "terrains/f.trn");
	for (const Found &f : found) TEST_EXPECT(f.asset != "terrains/g.trn");
	TEST_EXPECT(has("texture.tile_atlas_cells", "terrains/d.trn", S::Warning, false));
	TEST_EXPECT(has("texture.alpha_not_loaded", "models/thing.3di", S::Warning, false));
	TEST_EXPECT(has("texture.normal_map_halved", "models/thing.3di", S::Info, false));
	TEST_EXPECT(has("texture.height_wrap", "models/thing.3di", S::Warning, false));
	// Particle graphics no atlas page holds hang the game: 1024 wide for an additive one, 256 for a bump;
	// a distort 255 x 256 fits its page.
	TEST_EXPECT(has("texture.particle_too_big", "fx.ptl", S::Error, true));
	size_t too_big = 0;
	for (const Found &f : found)
		if (f.code == "texture.particle_too_big") {
			++too_big;
			TEST_EXPECT(f.message.find("it hangs loading the effects") != std::string::npos);
			TEST_EXPECT(f.message.find("fits.tga") == std::string::npos);
		}
	TEST_EXPECT(too_big == 2);
	TEST_EXPECT(has("texture.not_read", "textures/wall.tga", S::Warning, false));
	for (const Found &f : found) TEST_EXPECT(f.asset != "textures/det.tga");
	TEST_EXPECT(has("texture.wrong_reader", "terrains/h.trn", S::Warning, false));
	for (const Found &f : found)
		if (f.code == "texture.wrong_reader" && f.asset == "terrains/h.trn")
			TEST_EXPECT(f.message.find("made into the terrain's detail coefficient by its own name") != std::string::npos &&
			            f.message.find("the project lacks it") != std::string::npos);
	TEST_EXPECT(has("texture.loading_screen_size", "textures/loadscrn.pcx", S::Warning, false));
	// Nothing of the 1024 x 1024 colour map.
	for (const Found &f : found) TEST_EXPECT(f.asset != "terrains/e.trn");
	// A use's finding sits on its field, in the game's words.
	for (const Found &f : found) {
		if (f.code == "texture.colormap_size" && f.asset == "terrains/a.trn")
			TEST_EXPECT(f.field == "polytrn_colormap" &&
			            f.message.find("the game reads 1024 x 1024 texels (4 MB) from it") != std::string::npos);
		if (f.code == "texture.colormap_size" && f.asset == "terrains/b.trn")
			TEST_EXPECT(f.message.find("copying 2048 x 2048 texels out of it") != std::string::npos);
	}
	// The cut-out: the alpha-tested VS_SKBASIC's PCX diffuse keeps every texel; the VS_DOT3DIFF2 material cuts
	// out by its normal map's alpha, which its 24-bit .mdt lacks, never by its PCX diffuse.
	size_t cut_outs = 0;
	for (const Found &f : found)
		if (f.code == "texture.alpha_not_loaded") {
			++cut_outs;
			TEST_EXPECT(f.message.find("dotskin.pcx") == std::string::npos);
			TEST_EXPECT(f.message.find("every texel is kept, and nothing is cut out") != std::string::npos);
		}
	TEST_EXPECT(cut_outs == 2);
	bool normal_cut = false;
	for (const Found &f : found) normal_cut = normal_cut || (f.code == "texture.alpha_not_loaded" && f.message.find("dotnorm.mdt") != std::string::npos);
	TEST_EXPECT(normal_cut);
	std::printf("checks: %zu texture findings, %zu of them refusing a build\n", found.size(),
	            size_t(std::count_if(found.begin(), found.end(), [](const Found &f) { return f.blocks; })));
	return 0;
}

// Every texture the install ships, read by the reader its name picks: no finding of its own refuses a
// build; and every terrain's colour map, through the TGA reader, is 1024 x 1024.
int test_retail() {
	const std::string install = retail::install();
	if (install.empty()) {
		retail::skip_leg("OPENNOVA_JO_DIR (the install's textures checked)");
		return 0;
	}
	ProjectDocument project;
	project.target_game = "jo";
	InstallView install_view;
	std::string why;
	TEST_EXPECT(install_view.open(install_spec(install, project), why));
	const opennova::Vfs &mount = install_view.vfs();
	std::map<std::string, size_t> codes;
	size_t textures = 0, gating = 0, maps = 0;
	for (const opennova::VfsFileLocation &location : mount.list_files()) {
		const std::string &name = location.logical_name;
		const AssetKind kind = classify_asset(name, nullptr);
		std::vector<uint8_t> bytes;
		if (kind == AssetKind::Texture && mount.read_file_raw(name, bytes)) {
			++textures;
			std::unique_ptr<DocumentBase> document = make_texture_document();
			Diagnostic error;
			TEST_EXPECT(document->load_bytes(bytes, name, AssetKind::Texture, "jo", error));
			for (const Diagnostic &d : validate_texture_file(*document)) {
				++codes[d.code()];
				std::printf("retail: %s %s: %s\n", d.code().c_str(), name.c_str(), d.message.c_str());
				if (blocks_build(d)) {
					++gating;
					std::fprintf(stderr, "retail: %s: %s\n", name.c_str(), d.message.c_str());
				}
			}
		} else if (kind == AssetKind::Terrain && mount.read_file_raw(name, bytes)) {
			std::istringstream input(std::string(bytes.begin(), bytes.end()));
			opennova::TrnConfig config;
			std::string message;
			std::vector<uint8_t> map;
			if (!opennova::load_trn(input, config, message) || !mount.read_file_raw(config.colormap, map)) continue;
			++maps;
			check_texture_role(TextureRoleId::TerrainColourMap, config.colormap, texture_header_as(TextureReader::Tga, map), name,
			                   [&](CoreFinding, DiagnosticSeverity, const std::string &words) {
				                   std::fprintf(stderr, "retail: %s\n", words.c_str());
				                   ++gating;
			                   });
		}
	}
	for (const auto &[code, count] : codes) std::printf("retail: %-32s %zu\n", code.c_str(), count);
	std::printf("retail: %zu textures and %zu terrains' colour maps checked, %zu findings refusing a build\n", textures, maps,
	            gating);
	// Every one loads (the two 24-bit loading screens too, CP06.pcx and ASX_G4A.pcx).
	TEST_EXPECT(textures > 1000 && maps > 20 && gating == 0 && codes.count("texture.unloadable") == 0);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	int failures = 0;
	failures += test_checks();
	failures += test_retail();
	if (failures == 0) std::printf("editor_texture_checks: all passed\n");
	return failures == 0 ? 0 : 1;
}
