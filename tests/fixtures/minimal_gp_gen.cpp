// Generator + guard for the Black Hawk Down GP models fixtures/threedi_gp
// (fixtures/README.md): one model per GP kind, between them every construct the
// 677 GP models BHD ships use (witnessed 2026-10-19 by the retired
// threedi_gp_retail row: 610 GPM, 1 GPS, 66 GPP, and the migration notes they
// raised), minted through our own GP writer (formats/threedi_gp/threedi_gp_write.h,
// the tool's layout [orig: GP_WriteModel @ 0x4490EF (df4oed.exe)]) from the
// byte-built synthetic models in common/gp_model_bytes.h, each edited to carry its
// constructs, so no retail bytes live in the tree (ADR 0003):
//   fixtures/threedi_gp/synth_gpm.3di  a rigid model whose LOD holds one material per
//                                      state BHD draws that JO has no form for (a
//                                      clamped texture, a flat colour, an environment
//                                      pass, a lit material with a colour generator,
//                                      alpha-blended and alpha-tested self-lit ones,
//                                      an additive one, ones that ignored their
//                                      texture's alpha, a texture by mission region),
//                                      flipbooks on counter 3 (no JO control) and 4
//                                      (the team), a second stage, the bump techniques
//                                      (1, 1 paired, 2 paired, 4, 11, 14) over a
//                                      tangent stream, a spinning part, a lens-flare
//                                      user point, a face BHD's rounds pass through
//                                      (flag 0x2), an occlusion record narrowest
//                                      across x and z, and a second LOD sharing all
//                                      but one material with view-aligned rows and a
//                                      three-axis scale
//   fixtures/threedi_gp/synth_gps.3di  the GPS kind (Oicw_1R.3di, BHD's one): two
//                                      bones a primitive, one weight a vertex
//   fixtures/threedi_gp/synth_gpp.3di  the GPP kind: a skinned model on a global batch
//                                      over a two-part palette, a tangent stream and
//                                      the skinned bump techniques (11, 8, 8 paired, 16)
// Default: rebuild every file in memory and byte-compare the committed copies;
// `--write` (re)writes them.
#include <formats/threedi_gp/threedi_gp.h>
#include <formats/threedi_gp/threedi_gp_write.h>

#include "common/file_io.h"
#include "common/gp_model_bytes.h"
#include "common/test_paths.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace gp = opennova::threedi_gp;

namespace {

int failures = 0;

bool expect(bool cond, const std::string &msg) {
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", msg.c_str());
		++failures;
	}
	return cond;
}

gp::File parsed(const std::vector<uint8_t> &bytes) {
	gp::File file;
	std::string error;
	expect(gp::parse(bytes.data(), bytes.size(), file, error), "the synthetic model parses: " + error);
	return file;
}

// Append `m` (and `extra` records after it: a second stage or flipbook frames)
// to LOD 0's materials, drawn by a copy of the opaque list primitive 0 at the
// end of part 0's opaque run.
void add_drawn(gp::File &f, const gp::Material &m, const std::vector<gp::Material> &extra = {}) {
	gp::RenderModel &rm = f.lods[0];
	const int32_t index = static_cast<int32_t>(rm.materials.size());
	rm.materials.push_back(m);
	rm.materials.insert(rm.materials.end(), extra.begin(), extra.end());
	gp::Primitive p = rm.primitives[0];
	p.material = index;
	rm.primitives.insert(rm.primitives.begin() + rm.batches[0].opaque_count, p);
	++rm.batches[0].opaque_count;
}

// Material 0 of the synthetic model as a plain lit, textured, opaque material.
gp::Material plain(const gp::File &f) {
	gp::Material m = f.lods[0].materials[0];
	m.attributes = 0;
	m.blend = 1;
	m.colour_source = 2;
	m.alpha_source = 2;
	m.rgb = gp::RgbGenerator{};
	return m;
}

gp::File make_gpm() {
	gp::File f = parsed(gp_test::gpm_model());
	f.header.name = "synth_gpm";
	// --- the states BHD draws that JO has no form for, one material each ---
	gp::Material m = plain(f);
	m.attributes = gp::kMaterialClamp; // a clamped texture
	add_drawn(f, m);
	m = plain(f);
	m.colour_source = 1; // the region's flat colour, no texture
	for (size_t k = 0; k < m.colours.size(); ++k) m.colours[k] = static_cast<uint8_t>(0x20 + 0x10 * k);
	add_drawn(f, m);
	m = plain(f);
	m.env_pass = 1; // an environment-map pass
	m.env_tint = 0x00808080u;
	add_drawn(f, m);
	m = plain(f);
	m.rgb.style = 0x18; // a lit material with a running colour generator
	m.rgb.rate = 256;
	m.rgb.end = {0xFF, 0x80, 0x40, 0};
	add_drawn(f, m);
	m = plain(f);
	m.attributes = gp::kMaterialUnlit; // alpha-blended and self-lit
	m.blend = 2;
	add_drawn(f, m);
	m = plain(f);
	m.attributes = gp::kMaterialUnlit | gp::kMaterialAlphaTest; // self-lit and alpha-tested
	m.alpha_ref = 128;
	add_drawn(f, m);
	m = plain(f);
	m.blend = 4; // additive
	add_drawn(f, m);
	m = plain(f);
	m.blend = 2; // alpha-blended on the material's own constant alpha, not the texture's
	m.alpha_source = 1;
	m.alpha = 160;
	add_drawn(f, m);
	m = plain(f);
	m.blend = 2; // ... and on a running alpha generator scaled by it
	m.alpha_source = 1;
	m.alpha = 128;
	m.alpha_gen = gp::Generator{0x18, 0, 256, 255, 64};
	add_drawn(f, m);
	m = plain(f);
	m.region_textures = {0, 1, 0}; // a texture by mission region
	add_drawn(f, m);
	// --- flipbooks: on counter 3, which JO has no control for; on counter 4, the player's team ---
	gp::Material frame = plain(f);
	frame.region_textures = {1, 1, 1};
	m = plain(f);
	m.attributes = gp::kMaterialFlipbook;
	m.frame_count = 2;
	m.frame_counter = 3;
	m.frame_rate = 8;
	add_drawn(f, m, {frame});
	m.frame_counter = 4;
	add_drawn(f, m, {frame});
	// --- a fixed-function material with its second stage ---
	m = plain(f);
	m.paired = 1;
	add_drawn(f, m, {frame});
	// --- bump materials over the model's tangent stream, by BHD technique ---
	m = plain(f);
	m.attributes = gp::kMaterialBump;
	m.shader = 1; // VS_DOT3DIFF
	add_drawn(f, m);
	m.paired = 1; // VS_DOT3DIFF2, with its second stage
	add_drawn(f, m, {frame});
	m.paired = 0;
	m.shader = 2; // VS_PHONGT, its second stage dropped
	m.paired = 1;
	add_drawn(f, m, {frame});
	m.paired = 0;
	m.shader = 4; // VS_PHONGT
	add_drawn(f, m);
	m.shader = 11; // VS_PHONGO
	add_drawn(f, m);
	m.shader = 14; // VS_TRACER
	add_drawn(f, m);
	// --- parts: part 1 spins (three rates a second, Y, X, Z); a lens-flare user point ---
	gp::PartAnimation &row = f.lods[0].part_animations[1];
	row.flags = gp::kPanmAnimated | gp::kPanmSpinner;
	row.spin = {0.0f, 2.0f, 0.0f};
	gp::UserPoint flare = f.user_points[0];
	flare.type = 'L';
	flare.name = "flare";
	f.user_points.push_back(flare);
	f.header.user_point_count = static_cast<int32_t>(f.user_points.size());
	// --- the occlusion record narrowest across x and z (its sphere's radius the xz one) ---
	f.occlusion[0].vertices = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 2.0f, 0.0f}};
	// --- a second LOD: the first's, its additive material's texture swapped (so the
	// migration shares every material but that one), its rows the view-aligned forms
	// and a three-axis scale ---
	gp::RenderModel lod1 = f.lods[0];
	lod1.materials[1].region_textures = {0, 0, 0};
	lod1.part_animations[0].flags = gp::kPanmAnimated | gp::kPanmViewA | gp::kPanmScale3;
	lod1.part_animations[0].tracks[3] = gp::Generator{0, 0, 0, 256, 256};
	lod1.part_animations[0].tracks[4] = gp::Generator{0x18, 0, 256, 256, 512};
	lod1.part_animations[0].tracks[5] = gp::Generator{0, 0, 0, 256, 256};
	lod1.part_animations[1].flags = gp::kPanmAnimated | gp::kPanmViewB;
	lod1.part_animations[1].spin = {};
	f.lods.push_back(lod1);
	f.header.lod_count = 2;
	f.header.thresholds[1] = 0;
	f.header.thresholds[0] = 64 << 16;
	f.header.tags[1] = f.header.tags[0];
	return f;
}

gp::File make_gps() {
	gp::File f = parsed(gp_test::gpp_model());
	f.header.kind = gp::Kind::Gps;
	f.header.name = "synth_gps";
	// One weight a vertex, bone A's: three quarters, then a quarter.
	for (size_t i = 0; i < f.vertices.size(); ++i) f.vertices[i].blend = i % 2 == 0 ? 0.75f : 0.25f;
	// A primitive's two bones are the low bytes of the words at +24 and +28.
	for (gp::Primitive &p : f.lods[0].primitives) {
		p.palette.fill(0);
		p.palette[0] = 0;
		p.palette[4] = 1;
		p.palette_length = 0;
		p.vertex_word = static_cast<int32_t>(f.vertices.size()) - 1; // GPS stores the last index
	}
	return f;
}

gp::File make_gpp() {
	gp::File f = parsed(gp_test::gpp_model());
	f.header.name = "synth_gpp";
	// A tangent stream (S then T a vertex) and the skinned bump techniques BHD ships.
	f.header.flags |= gp::kFlagVertexStream;
	f.vertex_stream.assign(f.vertices.size() * 24, 0);
	for (size_t v = 0; v < f.vertices.size(); ++v) {
		const float st[6] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
		std::memcpy(f.vertex_stream.data() + v * 24, st, sizeof(st));
	}
	gp::Material m = plain(f);
	m.attributes = gp::kMaterialBump;
	m.shader = 11; // VS_SKBUMPPHONGOBJ
	add_drawn(f, m);
	m.shader = 8; // VS_SKBUMPDIFFT
	add_drawn(f, m);
	m.paired = 1; // VS_SKBUMPDIFFT2, with its second stage
	add_drawn(f, m, {plain(f)});
	m.paired = 0;
	m.shader = 16; // VS_SKBUMPPHONGT
	add_drawn(f, m);
	return f;
}

struct Recipe {
	const char *file;
	gp::File (*make)();
};

const Recipe kRecipes[] = {{"synth_gpm", make_gpm}, {"synth_gps", make_gps}, {"synth_gpp", make_gpp}};

} // namespace

int main(int argc, char **argv) {
	bool write_mode = false;
	for (int i = 1; i < argc; ++i)
		if (std::strcmp(argv[i], "--write") == 0) write_mode = true;
	const std::string dir = test_paths_fixture("threedi_gp");
	for (const Recipe &r : kRecipes) {
		const std::string path = dir + "/" + r.file + ".3di";
		std::vector<uint8_t> bytes;
		std::string error;
		if (!expect(gp::write(r.make(), bytes, error), std::string(r.file) + ": the writer accepts it: " + error)) continue;
		// The bytes read back.
		gp::File back;
		expect(gp::parse(bytes.data(), bytes.size(), back, error), std::string(r.file) + ": reads back: " + error);
		if (write_mode) {
			test_io::write_file(path, bytes);
			std::printf("wrote %s (%zu bytes)\n", path.c_str(), bytes.size());
			continue;
		}
		std::vector<uint8_t> committed;
		if (!expect(test_io::read_file(path, committed), path + " missing; run with --write")) continue;
		if (test_io::is_lfs_pointer(committed)) {
			std::printf("[skip] %s is an unpulled LFS pointer\n", path.c_str());
			continue;
		}
		expect(committed == bytes, path + " differs from the generator output; regenerate with --write");
		std::printf("%-12s %6zu bytes\n", r.file, committed.size());
	}
	if (failures == 0 && !write_mode) std::printf("OK: fixtures/threedi_gp byte-reproducible\n");
	return failures == 0 ? 0 : 1;
}
