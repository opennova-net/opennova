// ADR 0046 S18, the texture checks: a texture file's own findings (validate_texture_file, from its
// header: a TGA form the game's reader leaves unset or zeroes, rows stored top first, a colour map read
// as texels, an odd-width PCX's rows overrunning, a file the reader refuses, a DDS the reader pads to
// powers of two) and its uses' (the texture use check, graph/texture_checks: a terrain's colour map not
// 1024 x 1024, a foliage map overrun or of the
// wrong shape, a tile atlas not in 64-texel cells, a cut-out material over a PCX, a normal map halved, a
// height map whose side is no power of two, a particle graphic too wide, a colour map of a format its
// loader does not read, a .tga beside the .dds its loader opens, the default loading screen's size); which
// of them refuse a build. Over a minted project, every texture written here. And every form of texture JO:CA
// ships (editor/texture_forms.h) raises no finding of its own, and the shipped terrains' maps, in the forms they
// take, none of their roles' (each colour map 1024 x 1024 through the TGA reader).
#include <algorithm>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include <base/io/json.h>
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
#include <formats/dds/dds.h>
#include <formats/pcx/pcx_io.h>
#include <formats/tga/tga.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"
#include "editor/texture_forms.h"

using namespace opennova::editor;
namespace renderer = opennova::renderer;

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
	test_temp::TempDir dir{"opennova_editor_texture_checks"};
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
	TEST_EXPECT(test_io::write_file(t + "top.tga", top) && test_io::write_file(t + "grey16.tga", tga_header(3, 16, 0)) &&
	            test_io::write_file(t + "rle16.tga", tga_header(10, 16, 0)) &&
	            test_io::write_file(t + "mapped.tga", tga_header(2, 24, 0, 4)) &&
	            test_io::write_file(t + "odd.pcx", pcx(5, 4, 6)) && test_io::write_file(t + "oddfull.pcx", pcx(5, 16, 6)) &&
	            test_io::write_file(t + "oddwide.pcx", pcx(6, 16, 16)));
	// Rows of exactly its width, the last ending with a run of 63 at texel 63: the reader writes the run whole, 62
	// texels past the buffer [orig: Texture_LoadPCXFromPFF32 @ 0x56EDC0..0x56EDD0].
	{
		std::vector<uint8_t> runs = pcx(64, 64, 64);
		runs.resize(128);
		for (int row = 0; row < 63; ++row) runs.insert(runs.end(), {0xFF, 5, 5});
		runs.insert(runs.end(), {0xFF, 5, 0xFF, 5});
		runs.push_back(0x0C);
		runs.resize(runs.size() + 768, 0);
		TEST_EXPECT(test_io::write_file(t + "runs.pcx", runs));
		std::vector<uint8_t> narrow = pcx(1, 4, 30);
		narrow[4] = 1; // xmin 1 over xmax 0: a width of 0
		TEST_EXPECT(test_io::write_file(t + "narrow.pcx", narrow));
	}
	std::vector<uint8_t> four = pcx(4, 4, 4);
	four[3] = 4;
	// A 24-bit one (three planes) the reader takes, its odd width no overrun: no finding.
	std::vector<uint8_t> rgb = pcx(5, 4, 6);
	rgb[65] = 3;
	TEST_EXPECT(test_io::write_file(t + "four.pcx", four) && test_io::write_file(t + "rgb.pcx", rgb));
	// A DDS of 100 x 64, which the DDS reader pads to 128 x 64; one of 64 x 32, which it takes as it is.
	{
		std::vector<uint8_t> odd_dds, even_dds;
		std::string error;
		const std::vector<uint8_t> texels(size_t(100) * 64 * 4, 128);
		TEST_EXPECT(opennova::dds::dds_write_a8r8g8b8(texels.data(), 100, 64, odd_dds, error) &&
		            opennova::dds::dds_write_a8r8g8b8(texels.data(), 64, 32, even_dds, error));
		TEST_EXPECT(test_io::write_file(t + "padded.dds", odd_dds) && test_io::write_file(t + "even.dds", even_dds));
	}
	// A TGA whose file ends before its texels do, as raw true colour and as run-length packets; a PCX whose
	// rows are shorter than its width.
	std::vector<uint8_t> cut = tga(4, 4);
	cut.resize(18 + 10);
	std::vector<uint8_t> cut_rle = tga_header(10, 32, 0);
	cut_rle.resize(18);
	cut_rle.insert(cut_rle.end(), {0x81, 1, 2, 3, 4});
	TEST_EXPECT(test_io::write_file(t + "cut.tga", cut) && test_io::write_file(t + "cutrle.tga", cut_rle) &&
	            test_io::write_file(t + "short.pcx", pcx(6, 4, 4)));
	// The uses': terrains, a model, a particle file, the default loading screen.
	TEST_EXPECT(test_io::write_file(t + "detail.tga", tga(8, 8)) && test_io::write_file(t + "small.tga", tga(512, 512)) &&
	            test_io::write_file(t + "wide.tga", tga(2048, 512)) && test_io::write_file(t + "tall.tga", tga(512, 2048)) &&
	            test_io::write_file(t + "colour.tga", tga(1024, 1024)) && test_io::write_file(t + "map.pcx", pcx(8, 8, 8)) &&
	            test_io::write_file(t + "fol.pcx", pcx(64, 32, 64)) && test_io::write_file(t + "fol48.pcx", pcx(48, 48, 48)) &&
	            test_io::write_file(t + "fol2k.pcx", pcx(2048, 1024, 2048)) &&
	            test_io::write_file(t + "blendw.tga", tga(64, 32)) && test_io::write_file(t + "blendt.tga", tga(32, 64)) &&
	            test_io::write_file(t + "tiles.tga", tga(100, 64)) && test_io::write_file(t + "skin.pcx", pcx(8, 8, 8)) &&
	            test_io::write_file(t + "big.mdt", tga(1024, 16)) && test_io::write_file(t + "bump.tga", tga(100, 64)) &&
	            test_io::write_file(t + "wall.tga", tga(8, 8)) && test_io::write_file(t + "wall.dds", tga(8, 8)) &&
	            test_io::write_file(t + "huge.tga", tga(1024, 8)) && test_io::write_file(t + "page.tga", tga(256, 16)) &&
	            test_io::write_file(t + "fits.tga", tga(255, 256)) &&
	            test_io::write_file(t + "loadscrn.pcx", pcx(640, 480, 640)));
	// A VS_DOT3DIFF2 material cuts out by its normal map's alpha: its PCX diffuse is no finding, its 24-bit
	// normal map is.
	{
		const std::vector<uint8_t> rgba(16 * 4, 200);
		std::vector<uint8_t> rgb;
		std::string error;
		opennova::tga::tga_write_rgb24(rgba.data(), 4, 4, rgb, error);
		TEST_EXPECT(test_io::write_file(t + "dotskin.pcx", pcx(8, 8, 8)) && test_io::write_file(t + "dotdet.tga", tga(8, 8)) &&
		            test_io::write_file(t + "dotnorm.mdt", rgb));
	}
	// A char map holding classes past the twenty the tile set tables name (S25): listed, the build going on.
	{
		opennova::IndexedImage8 classes;
		classes.width = classes.height = 64;
		classes.indices.assign(size_t(64) * 64, 1);
		classes.indices[0] = 22;
		classes.indices[1] = 30;
		classes.indices[70] = 22;
		std::vector<uint8_t> bytes;
		std::string error;
		TEST_EXPECT(opennova::encode_pcx_indexed(classes, bytes, error) && test_io::write_file(t + "classes.pcx", bytes) &&
		            test_io::write_text(root + "/terrains/l.trn",
		                                    trn("polytrn_colormap colour.tga\npolytrn_charmap classes.pcx\n")));
	}
	// Terrain maps of no TGA through the game's TGA reader, which sizes the texels by bytes 12 to 15 (S26 A3):
	// a PCX whose DPI words read 8 x 8 (image type 1, entries of no 24 bits: cleared black); one of type 5
	// (no arm: its texels unwritten); a PNG, read 18505 x 21060, past the pool; a PCX whose words read 23171 x 23171,
	// whose 4 x w x h wraps the int the buffer is asked in.
	{
		std::vector<uint8_t> dpi = pcx(8, 8, 8);
		dpi[12] = 8;
		dpi[14] = 8;
		std::vector<uint8_t> stale = dpi;
		stale[2] = 5;
		std::vector<uint8_t> wrap = pcx(8, 8, 8);
		wrap[12] = wrap[14] = 0x83;
		wrap[13] = wrap[15] = 0x5A;
		const std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 0x0D, 'I', 'H', 'D', 'R',
		                                  0, 0, 0, 8, 0, 0, 0, 8, 8, 6, 0, 0, 0, 0, 0, 0, 0};
		TEST_EXPECT(test_io::write_file(t + "dpi.pcx", dpi) && test_io::write_file(t + "stale.pcx", stale) &&
		            test_io::write_file(t + "wrap.pcx", wrap) && test_io::write_file(t + "big.png", png) &&
		            test_io::write_text(root + "/terrains/m.trn", trn("polytrn_colormap dpi.pcx\n")) &&
		            test_io::write_text(root + "/terrains/n.trn", trn("polytrn_colormap stale.pcx\n")) &&
		            test_io::write_text(root + "/terrains/o.trn", trn("polytrn_colormap big.png\n")) &&
		            test_io::write_text(root + "/terrains/q.trn",
		                                    trn("polytrn_colormap colour.tga\npolytrn_detailblendmap big.png\n")) &&
		            test_io::write_text(root + "/terrains/r.trn", trn("polytrn_colormap wrap.pcx\n")));
	}
	TEST_EXPECT(test_io::write_text(root + "/terrains/a.trn", trn("polytrn_colormap small.tga\n")) &&
	            test_io::write_text(root + "/terrains/b.trn", trn("polytrn_colormap wide.tga\npolytrn_foliagemap fol.pcx\npolytrn_charmap fol.pcx\n")) &&
	            test_io::write_text(root + "/terrains/c.trn", trn("polytrn_colormap tall.tga\npolytrn_foliagemap fol48.pcx\npolytrn_charmap fol48.pcx\n")) &&
	            test_io::write_text(root + "/terrains/d.trn", trn("polytrn_colormap map.pcx\npolytrn_tilestrip tiles.tga\n")) &&
	            test_io::write_text(root + "/terrains/e.trn", trn("polytrn_colormap colour.tga\n")) &&
	            test_io::write_text(root + "/terrains/f.trn", trn("polytrn_colormap colour.tga\npolytrn_detailblendmap blendw.tga\n"
	                                                                  "polytrn_foliagemap fol2k.pcx\n")) &&
	            test_io::write_text(root + "/terrains/g.trn", trn("polytrn_colormap colour.tga\npolytrn_detailblendmap blendt.tga\n")) &&
	            // A blend map the TGA reader cannot read: skipped, the terrain loading on.
	            test_io::write_text(root + "/terrains/k.trn", trn("polytrn_colormap colour.tga\npolytrn_detailblendmap map.pcx\n")) &&
	            // A splat detail read by STAGE (its .dds) and by its own name through the TGA reader: no "never
	            // read" on its .tga. A detail map whose .tga the project lacks: no coefficient made of its .dds.
	            test_io::write_text(root + "/terrains/h.trn",
	                                    "polytrn_detailmap cdet.tga\npolytrn_polydata isle.cpt\npolytrn_sectorcount 1\npolytrn_sectors 0\n"
	                                    "polytrn_colormap colour.tga\npolytrn_detailmap_c1 det.tga\n") &&
	            test_io::write_file(t + "det.tga", tga(8, 8)) && test_io::write_file(t + "det.dds", tga(8, 8)) &&
	            test_io::write_file(t + "cdet.dds", tga(8, 8)) &&
	            test_io::write_text(root + "/fx.ptl",
	                                    "[effectdef]\n{\n\tid = BOOM;\n\tpdefs = puff, dent, ring;\n}\n\n[particledef]\n{\n\tid = "
	                                    "puff;\n\tgraphic1 = huge.tga, additive;\n}\n\n[particledef]\n{\n\tid = dent;\n"
	                                    "\tgraphic1 = page.tga, bump;\n}\n\n[particledef]\n{\n\tid = ring;\n"
	                                    "\tgraphic1 = fits.tga, distort;\n}\n"));
	const std::string scene = dir.file("scene");
	TEST_EXPECT(test_io::write_text(scene + "/thing.o3d",
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
	// A form the reader has no case for: the game loads it unset and goes on, so it refuses no build.
	TEST_EXPECT(has("texture.tga_unfilled", "textures/grey16.tga", S::Warning, false));
	TEST_EXPECT(has("texture.tga_zeroed", "textures/rle16.tga", S::Warning, false));
	TEST_EXPECT(has("texture.tga_colour_map_skipped", "textures/mapped.tga", S::Warning, false));
	// An odd width's last row writes its extra texel into the 48 spare bytes its 80-byte buffer's block rounds up to:
	// a warning. Sixteen rows fill the block (320 bytes): the texel lands 4 bytes past it, on the next block's pool
	// pointer where the allocator split the block it gave, within a block it gave whole (36 bytes more at most): a
	// warning. Rows of 16 bytes for 6 texels write 40 bytes past a full block, past any block the allocator gives, and
	// the game faults (S26 A4).
	TEST_EXPECT(has("texture.pcx_overrun", "textures/odd.pcx", S::Warning, false));
	TEST_EXPECT(has("texture.pcx_overrun", "textures/oddfull.pcx", S::Warning, false));
	TEST_EXPECT(has("texture.pcx_overrun", "textures/oddwide.pcx", S::Error, true));
	// Rows of its width, the last row's final run crossing its end: 248 bytes past a full block (64 x 64 x 4), past any
	// block, gating (it was no finding: the header's rows fit).
	TEST_EXPECT(has("texture.pcx_overrun", "textures/runs.pcx", S::Error, true));
	for (const Found &f : found)
		if (f.code == "texture.pcx_overrun" && f.asset == "textures/runs.pcx")
			TEST_EXPECT(f.message.find("its rows reach 62 texels past the image's buffer") != std::string::npos &&
			            f.message.find("past its buffer's block by 248 bytes") != std::string::npos);
	// A width of 0: a request of no bytes is one 64-byte block [orig: FastMem_Alloc @ 0x7697C1..0x7697C6], and every
	// row writes its 30 texels from the buffer's start, 56 bytes past it: past any block, gating.
	TEST_EXPECT(has("texture.pcx_overrun", "textures/narrow.pcx", S::Error, true));
	for (const Found &f : found)
		if (f.code == "texture.pcx_overrun" && f.asset == "textures/narrow.pcx")
			TEST_EXPECT(f.message.find("past its buffer's block by 56 bytes") != std::string::npos);
	TEST_EXPECT(has("texture.pcx_short_rows", "textures/short.pcx", S::Warning, false));
	// Read on past the file, no fault witnessed: listed (S26 A4).
	TEST_EXPECT(has("texture.tga_truncated", "textures/cut.tga", S::Error, false));
	TEST_EXPECT(has("texture.tga_truncated", "textures/cutrle.tga", S::Error, false));
	TEST_EXPECT(has("texture.unloadable", "textures/four.pcx", S::Warning, false));
	for (const Found &f : found) TEST_EXPECT(f.asset != "textures/rgb.pcx");
	TEST_EXPECT(has("texture.dds_not_pow2", "textures/padded.dds", S::Warning, false));
	for (const Found &f : found) {
		TEST_EXPECT(f.asset != "textures/even.dds");
		if (f.code == "texture.dds_not_pow2")
			TEST_EXPECT(f.message.find("Its sides, 100 x 64, are not powers of two: the game's DDS reader makes a texture of 128 x 64") !=
			            std::string::npos);
	}
	// The odd width's words say how many bytes spill, not that the width is odd.
	for (const Found &f : found)
		if (f.code == "texture.pcx_overrun" && f.asset != "textures/runs.pcx" && f.asset != "textures/narrow.pcx")
			TEST_EXPECT(f.message.find(f.asset == "textures/oddwide.pcx" ? "each row's 10 extra texels" : "each row's 1 extra texel") !=
			            std::string::npos);
	for (const Found &f : found) {
		if (f.code == "texture.pcx_overrun" && f.asset == "textures/oddfull.pcx")
			TEST_EXPECT(f.message.find("4 bytes past the spare bytes its buffer's block rounds up to") != std::string::npos);
		if (f.code == "texture.pcx_overrun" && f.asset == "textures/oddwide.pcx")
			TEST_EXPECT(f.message.find("past its buffer's block by 40 bytes, past any block the allocator gives") !=
			            std::string::npos);
	}
	// The uses'. A colour map: read short (512 x 512), wider than tall (the quadrant split overruns it and its
	// buffer), taller than wide (neither: only its first rows matter). The overruns are listed, no failure of
	// them witnessed (S26 A4).
	TEST_EXPECT(has("texture.colormap_size", "terrains/a.trn", S::Error, false));
	TEST_EXPECT(has("texture.colormap_size", "terrains/b.trn", S::Error, false));
	TEST_EXPECT(has("texture.colormap_size", "terrains/c.trn", S::Warning, false));
	// A colour map or a blend map that is no TGA: the terrain's TGA reader takes it by its header's words, the
	// terrain loading on with what the type's arm wrote (S26 A3): a PCX whose DPI words are 0, no
	// texel; one whose words read 8 x 8 (type 1, entries of no 24 bits), black; one of type 5, texels never written.
	const auto said = [&](const char *asset, const char *words) {
		for (const Found &f : found)
			if (f.code == "texture.wrong_reader" && f.asset == asset) {
				if (f.message.find(words) == std::string::npos) std::fprintf(stderr, "%s: %s\n", asset, f.message.c_str());
				return f.message.find(words) != std::string::npos;
			}
		return false;
	};
	TEST_EXPECT(has("texture.wrong_reader", "terrains/d.trn", S::Warning, false) && said("terrains/d.trn", "as 0 x 0, no texel"));
	TEST_EXPECT(has("texture.wrong_reader", "terrains/k.trn", S::Warning, false) && said("terrains/k.trn", "as 0 x 0, no texel"));
	TEST_EXPECT(has("texture.wrong_reader", "terrains/m.trn", S::Warning, false) &&
	            said("terrains/m.trn", "takes it as 8 x 8 black texels (its image type 1"));
	TEST_EXPECT(has("texture.wrong_reader", "terrains/n.trn", S::Warning, false) &&
	            said("terrains/n.trn", "takes it as 8 x 8 texels it never writes (its image type 5"));
	// A PNG as the colour map: read as 18505 x 21060, whose texels the pool can never give: the read fails and the
	// mission aborts, an error that gates the build; as the blend map the terrain loads without a blend, a warning.
	TEST_EXPECT(has("texture.wrong_reader", "terrains/o.trn", S::Error, true) &&
	            said("terrains/o.trn", "as a 18505 x 21060 picture") && said("terrains/o.trn", "the mission aborts"));
	TEST_EXPECT(has("texture.wrong_reader", "terrains/q.trn", S::Warning, false) &&
	            said("terrains/q.trn", "without a detail blend"));
	// Words whose 4 x w x h wraps the int: written past the buffer, the game faults.
	TEST_EXPECT(has("texture.wrong_reader", "terrains/r.trn", S::Error, true) &&
	            said("terrains/r.trn", "as a 23171 x 23171 picture") && said("terrains/r.trn", "the game faults"));
	TEST_EXPECT(has("texture.foliage_map_overrun", "terrains/b.trn", S::Error, false));
	TEST_EXPECT(has("texture.foliage_map_shape", "terrains/c.trn", S::Warning, false));
	// A char map is sampled as the foliage map is (S25 B5): the same files as char maps, the same rule.
	TEST_EXPECT(has("texture.char_map_overrun", "terrains/b.trn", S::Error, false));
	TEST_EXPECT(has("texture.char_map_shape", "terrains/c.trn", S::Warning, false));
	// Classes past the twenty .TSD names: the game reads them as they stand, so a warning (S25).
	TEST_EXPECT(has("texture.char_map_class", "terrains/l.trn", S::Warning, false));
	for (const Found &f : found)
		if (f.code == "texture.char_map_class")
			TEST_EXPECT(f.message.find("holds 3 texels of a class at or past 20 (the highest 30)") != std::string::npos);
	// A foliage map wider than 1024 reads its first texel everywhere: its shape, no overrun.
	TEST_EXPECT(has("texture.foliage_map_shape", "terrains/f.trn", S::Warning, false));
	// A blend map wider than tall overruns the split; a taller one does not.
	TEST_EXPECT(has("texture.blend_map_size", "terrains/f.trn", S::Error, false));
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

// Every form of texture JO:CA ships (editor/texture_forms.h, the census of the install's 2,579 textures), opened as
// the texture document opens it: none raises a finding of its own (not one of the 2,579 does), so none refuses a
// build, and each loads (the two 24-bit loading screens too, CP06.pcx and ASX_G4A.pcx, the three-plane form).
int test_shipped_forms() {
	size_t forms = 0;
	for (const texture_forms::TextureForm &form : texture_forms::shipped_texture_forms()) {
		std::unique_ptr<DocumentBase> document = make_texture_document();
		Diagnostic error;
		TEST_EXPECT(document->load_bytes(form.bytes, form.name, AssetKind::Texture, "jo", error));
		const std::vector<Diagnostic> found = validate_texture_file(*document);
		for (const Diagnostic &d : found)
			std::fprintf(stderr, "%s (%s): %s: %s\n", form.name.c_str(), form.what, d.code().c_str(), d.message.c_str());
		TEST_EXPECT(found.empty());
		++forms;
	}
	TEST_EXPECT(forms > 20);
	std::printf("shipped forms: %zu forms of texture the install ships, none with a finding of its own\n", forms);
	return 0;
}

// The shipped terrains' maps in the forms they take (the census of JO:CA's 30 terrains), each read by its role's
// reader: every colour map a BFC1-stored 1024 x 1024 24-bit TGA; the detail blend maps 1024 x 1024, BFC1-stored, of
// 24 bits (7) or 32 bits of descriptor 8 (22); the char maps 8-bit PCX squares of 512 (28) or 256 (2), the foliage maps
// squares of 256; the tile atlases 512 x 512 32-bit TGA and the detail maps 512 x 512 TGA of 32 bits (29) or 24 (1),
// BFC1-stored. No role's rule finds anything of one, and each colour map, through the TGA reader, is 1024 x 1024.
int test_shipped_terrain_maps() {
	using R = renderer::TextureRoleId;
	using texture_forms::bfc1_stored;
	using texture_forms::tga_true_colour;
	struct Map {
		R role;
		const char *file;
		TextureReader reader;
		std::vector<uint8_t> bytes;
	};
	const auto pcx8 = [](int side) {
		std::vector<uint8_t> out;
		std::string error;
		opennova::encode_pcx_indexed(texture_forms::indexed_pattern(side, side), out, error);
		return out;
	};
	std::vector<Map> maps;
	maps.push_back({R::TerrainColourMap, "isle_c.tga", TextureReader::Tga, bfc1_stored(tga_true_colour(1024, 1024, 24, 0, true))});
	maps.push_back({R::TerrainBlendMap, "isle_d1.tga", TextureReader::Tga, bfc1_stored(tga_true_colour(1024, 1024, 24, 0, true))});
	maps.push_back({R::TerrainBlendMap, "cove_d1.tga", TextureReader::Tga, bfc1_stored(tga_true_colour(1024, 1024, 32, 8, true))});
	maps.push_back({R::TerrainCharMap, "isle_m.pcx", TextureReader::Pcx, pcx8(512)});
	maps.push_back({R::TerrainCharMap, "reef_m.pcx", TextureReader::Pcx, pcx8(256)});
	maps.push_back({R::TerrainFoliageMap, "isle_f.pcx", TextureReader::Pcx, pcx8(256)});
	maps.push_back({R::TerrainTileAtlas, "trntile10.tga", TextureReader::Tga, bfc1_stored(tga_true_colour(512, 512, 32, 8, true))});
	maps.push_back({R::TerrainDetailCoefficient, "det_d1.tga", TextureReader::Tga, bfc1_stored(tga_true_colour(512, 512, 32, 8, true))});
	maps.push_back({R::TerrainDetailCoefficient, "det_dd1.tga", TextureReader::Tga, bfc1_stored(tga_true_colour(512, 512, 24, 0, true))});
	size_t found = 0;
	for (const Map &map : maps) {
		const TextureHeader header = texture_header_as(map.reader, map.bytes);
		TEST_EXPECT(header.read);
		if (map.role == R::TerrainColourMap) TEST_EXPECT(header.bfc1 && header.width == 1024 && header.height == 1024);
		check_texture_role(map.role, map.file, header, "the shipped form", [&](CoreFinding, DiagnosticSeverity, const std::string &words) {
			std::fprintf(stderr, "%s: %s\n", map.file, words.c_str());
			++found;
		});
	}
	TEST_EXPECT(found == 0);
	std::printf("shipped terrain maps: %zu maps in the forms the shipped terrains take, no role's finding\n", maps.size());
	return 0;
}

} // namespace

int main() {
	int failures = 0;
	failures += test_checks();
	failures += test_shipped_forms();
	failures += test_shipped_terrain_maps();
	if (failures == 0) std::printf("editor_texture_checks: all passed\n");
	return failures == 0 ? 0 : 1;
}
