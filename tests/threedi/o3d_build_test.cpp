// opennova-3di `build` and `scene` over small scenes: what the scene text
// carries through build -> scene -> build byte for byte, and the malformed
// records build refuses (docs/threedi/o3d-scene-format.md "Validation").
// Drives the command handlers the CLI dispatches (opennova_3di_commands), and
// the executable itself where a refusal must say why.
//
//   o3d_build_test <scratch dir> <opennova-3di>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <formats/threedi/threedi_3di3.h>
#include <formats/threedi/threedi_build.h>

#include "common/run_command.h"
#include "threedi_cli.h"
#include "../common/file_io.h"

using namespace opennova::threedi;

namespace {

using test_io::read_file_text;

int failures = 0;

void check(bool ok, const std::string &what) {
	if (!ok) {
		std::fprintf(stderr, "FAIL: %s\n", what.c_str());
		++failures;
	}
}

std::filesystem::path dir;

std::string path_of(const std::string &name, const char *ext) { return (dir / (name + ext)).string(); }

// Build `text`; true when build accepted it.
bool build(const std::string &name, const std::string &text) {
	std::ofstream(path_of(name, ".o3d"), std::ios::binary) << text;
	return threedi_cli::cmd_build(path_of(name, ".o3d").c_str(), path_of(name, ".3di").c_str()) == 0;
}

// build -> scene -> build must re-mint the model byte for byte.
void round_trip(const std::string &name, const std::string &text) {
	if (!build(name, text)) {
		check(false, name + ": build");
		return;
	}
	const bool scened = threedi_cli::cmd_scene(path_of(name, ".3di").c_str(), path_of(name, ".rt.o3d").c_str()) == 0;
	check(scened, name + ": scene");
	if (!scened) return;
	const bool rebuilt =
			threedi_cli::cmd_build(path_of(name, ".rt.o3d").c_str(), path_of(name, ".rt.3di").c_str()) == 0;
	check(rebuilt, name + ": rebuild the scene");
	if (rebuilt) check(read_file_text(path_of(name, ".3di")) == read_file_text(path_of(name, ".rt.3di")), name + ": byte-exact round trip");
}

// Build must refuse `text` (one field changed from an accepted scene).
void refuses(const std::string &name, const std::string &text) { check(!build(name, text), name + ": refused"); }

std::string cli; // the opennova-3di executable

// Run the CLI's `build` over `text`: its exit status, and what it said on
// stderr in `said`.
int build_with_cli(const std::string &name, const std::string &text, std::string &said) {
	std::ofstream(path_of(name, ".o3d"), std::ios::binary) << text;
	const std::string said_path = path_of(name, ".err");
	const int status = test_cmd::run(test_cmd::quoted(cli) + " build " + test_cmd::quoted(path_of(name, ".o3d")) + " -o " +
			test_cmd::quoted(path_of(name, ".3di")) + " 2> " + test_cmd::quoted(said_path));
	said = read_file_text(said_path);
	return status;
}

// The CLI must refuse `text` and say `words` on stderr: the message an author
// reads is part of the contract.
void refuses_saying(const std::string &name, const std::string &text, const std::string &words) {
	std::string said;
	check(build_with_cli(name, text, said) != 0, name + ": refused");
	check(said.find(words) != std::string::npos, name + ": says \"" + words + "\" (it said: " + said.substr(0, 400) + ")");
}

// The CLI must build `text`, and say `words` on stderr (a note), or say
// nothing of `words` when `says` is false.
void builds_saying(const std::string &name, const std::string &text, const std::string &words, bool says = true) {
	std::string said;
	check(build_with_cli(name, text, said) == 0, name + ": built (it said: " + said.substr(0, 400) + ")");
	check((said.find(words) != std::string::npos) == says,
			name + (says ? ": says \"" : ": does not say \"") + words + "\" (it said: " + said.substr(0, 400) + ")");
}

const std::string kSkinned =
		"o3d 1\nmodel SKIN\nskinned 1\nmaterial VS_SKBASIC\ntexture skin.tga\nlod 128 gnrc\n"
		"part 0 0 0 0\npart 0 0 0 1\npart 0 0 0 -1\nstrip 0 0\nbones 0 1\n"
		"v 1 1 0 0 0 1 0 0 0 1 0 0 1 0 0\nv -1 1 0 0 0 1 0 1 0 1 0 0 0.5 0.5 0\n"
		"v -1 -1 1 0 0 1 1 1 1 0 0 0 1 0 0\nv 1 -1 1 0 0 1 1 0 1 0 0 0 0.75 0.25 0\nt 0 1 2\nt 0 2 3\n"
		"panm 0 0\npanm 1 0\npanm 2 0\n"
		"cobj 0 0 0 0\ncsphere 0 0 0 0.5\ncobj 0 0 0 1\ncsphere 0 0 1 0.25\n"
		"cobj 0 0 0 -1\ncv 1 1 0\ncv -1 1 0\ncv -1 -1 1\ncf 0 1 2 1\n";

} // namespace

int main(int argc, char **argv) {
	if (argc != 3) {
		std::fprintf(stderr, "usage: o3d_build_test <scratch dir> <opennova-3di>\n");
		return 2;
	}
	dir = std::filesystem::path(argv[1]) / "o3d-build";
	cli = argv[2];
	std::filesystem::create_directories(dir);

	// A skinned model's LOD with no parts (retail ships empty LODs) scenes and
	// rebuilds like any other.
	round_trip("skinned-empty-lod", kSkinned + "lod 0 gnrc\n");

	// NaN and infinity (retail J_bsh1's vertex normals, ChmLFP1's occlusion
	// planes carry both NaN signs) print as nan, -nan, inf and -inf, which
	// strtod and Python's float() read, and rebuild to the same bits.
	{
		const std::string text = "o3d 1\nmodel NAN\nmaterial FF_ST_OP\nlod 0\npart 0 0 0 0\nstrip 0\n"
				"v 0 0 0 nan -nan inf 0 0\nv 1 0 0 0 0 1 1 0\nv 0 1 0 -inf 0 1 0 1\nt 0 1 2\n"
				"occ 0 0 0\nov 0 0 0\nov 1 0 0\nov 0 1 0\nop 0 0 1 nan\nop 0 0 -1 -nan\nof 0 1 2 0\n";
		round_trip("nan", text);
		const std::string scene = read_file_text(path_of("nan", ".rt.o3d"));
		check(scene.find("nan(") == std::string::npos, "nan: no printf-specific NaN spelling");
		check(scene.find(" -nan") != std::string::npos && scene.find(" nan") != std::string::npos &&
						scene.find(" inf") != std::string::npos && scene.find(" -inf") != std::string::npos,
				"nan: both NaN signs and both infinities are written");
		// `nan` is the quiet NaN retail stores (0x7FC00000, 0xFFC00000 with
		// the sign), whatever the C library's strtod makes of the word.
		Threedi3di3 m{};
		if (threedi_3di3_read(path_of("nan", ".3di").c_str(), &m) == 0) {
			uint32_t bits[4] = {};
			const float values[4] = {m.lods[0].vertices.items[0].normal[0], m.lods[0].vertices.items[0].normal[2],
					m.occlusion_planes[0].radius, m.occlusion_planes[1].radius};
			std::memcpy(bits, values, sizeof(bits));
			// Model axes (-y, z, x): the mission -nan in y lands negated in x.
			check(bits[0] == 0x7FC00000u && bits[1] == 0x7FC00000u, "nan: vertex normals hold the quiet NaN");
			check(bits[2] == 0x7FC00000u && bits[3] == 0xFFC00000u, "nan: occlusion planes keep the NaN sign");
			threedi_3di3_free(&m);
		} else {
			check(false, "nan: read the model back");
		}
	}

	// CXLT: `cxlt` records are the table, in order, whatever the sections say
	// (retail Oiltnk2X's 15 rows sit near the origin while its sections reach
	// 24 m out); a bare `cxlt` is an empty table (Chair03X: 7 sections, no
	// row); without either, one row per non-root section at its offset.
	{
		const std::string rigid = "o3d 1\nmodel CXLT\nlod 0\npart 0 0 0 0\npart 0 1 0 0\npart 0 0 2 0\n"
				"cobj 0 0 0 0\ncobj 0 1 0 0\ncobj 0 0 2 0\n";
		const auto rows = [&](const std::string &name, std::vector<std::array<int32_t, 3>> want) {
			Threedi3di3 m{};
			if (threedi_3di3_read(path_of(name, ".3di").c_str(), &m) != 0) {
				check(false, name + ": read back");
				return;
			}
			bool same = m.collision != nullptr && m.collision->translation_count == want.size();
			for (size_t i = 0; same && i < want.size(); ++i)
				for (int k = 0; k < 3; ++k) same = same && m.collision->translations[i].translation[k] == want[i][k];
			check(same, name + ": CXLT rows");
			threedi_3di3_free(&m);
		};
		round_trip("cxlt-given", rigid + "cxlt 0.5 -0.25 0.125\ncxlt 0.1 0 -0.1\ncxlt 3 4 5\n");
		// 0.1 * 65536 = 6553.6: WriteCXLT truncates [5fc5b4f6a^ export_3di.cpp].
		rows("cxlt-given", {{32768, -16384, 8192}, {6553, 0, -6553}, {196608, 262144, 327680}});
		round_trip("cxlt-empty", rigid + "cxlt\n");
		rows("cxlt-empty", {});
		round_trip("cxlt-derived", rigid);
		rows("cxlt-derived", {{65536, 0, 0}, {0, 131072, 0}});
		refuses("cxlt-malformed", rigid + "cxlt 1 2\n");
	}

	// The derived words follow the exporter's quantization
	// (docs/threedi/o3d-scene-format.md, "derived collision values").
	{
		const std::string text =
				"o3d 1\nmodel RULES\nmaterial FF_ST_OP\nlod 0\npart 0 1 2 3\n"
				// A diamond: its farthest vertex (1) is not its box's corner (1.414).
				"strip 0\nv 1 0 0 0 0 1 0 0\nv 0 1 0 0 0 1 0 0\nv -1 0 0 0 0 1 0 0\nv 0 -1 0 0 0 1 0 0\n"
				"t 0 1 2\nt 0 2 3\n"
				// 1 + 3/262144 (a float): 65536.75 in 16.16, truncated to 65536.
				"part 0 1.00001144 2 3\nstrip 0\nv 1.00001144 0 0 0 0 1 0 0\nv 0 0.5 0 0 0 1 0 0\nv 0 0 0.5 0 0 0 0 0\nt 0 1 2\n"
				"userpoint p 0.1 0 0 0 0 1 0\n"
				"cobj 0 0.1 0 0\ncv 0 0 1\ncv 1 0 1\ncv 0 1 1\ncf 0 1 2\n"
				// x spans -2 .. 1 in 16.16: a midpoint of -0.5, which floors to -1.
				"cobj 0 0 0 0\ncvol 1 0 -0.000030517578125 0 0 0.0000152587890625 1 1\n";
		round_trip("rules", text);
		Threedi3di3 m{};
		if (threedi_3di3_read(path_of("rules", ".3di").c_str(), &m) == 0 && m.collision != nullptr &&
				m.collision->object_count == 2 && m.lod_count == 1 && m.lods[0].render_object_count == 2) {
			const ThreediRenderObject *ro = m.lods[0].render_objects;
			const ThreediCollisionObject *o = m.collision->objects;
			check(m.header.max_radius_fp16 == 65536, "rules: GHDR radius truncates [orig: WriteGHDR]");
			check(m.user_points[0].x == 6553, "rules: user points truncate");
			check(o[0].offset[0] == 6553, "rules: section offsets truncate");
			check(ro[0].bounding_radius == 1.0f, "rules: a part's radius is its farthest vertex");
			// The root stores rel (-0, 0, 0) whatever its pivot; a part whose
			// pivot shares its parent's y stores -0 in model x.
			uint32_t rel0[3], rel1;
			std::memcpy(rel0, ro[0].rel, sizeof(rel0));
			std::memcpy(&rel1, &ro[1].rel[0], sizeof(rel1));
			check(rel0[0] == 0x80000000u && rel0[1] == 0 && rel0[2] == 0, "rules: the root's rel is (-0, 0, 0)");
			check(rel1 == 0x80000000u, "rules: an equal pivot y stores -0");
			check(o[1].min[0] == -2 && o[1].max[0] == 1 && o[1].med[0] == -1,
					"rules: the section midpoint floors (-1 / 2 -> -1, not 0)");
			threedi_3di3_free(&m);
		} else {
			check(false, "rules: read back");
		}
		// The vertexless occlusion record's centre is the 0/0 NaN retail ChmLFP1
		// stores: (+nan, -nan, -nan) in model axes.
		round_trip("occ-empty", "o3d 1\nmodel OCC\nlod 0\npart 0 0 0 0\nocc 2 0 0\nop 1 0 0 0\nop -1 0 0 0\n"
				"op 0 1 0 0\nop 0 -1 0 0\nop 0 0 1 0\nop 0 0 -1 0\n");
		if (threedi_3di3_read(path_of("occ-empty", ".3di").c_str(), &m) == 0 && m.occlusion_object_count == 1) {
			uint32_t c[3];
			std::memcpy(c, m.occlusion_objects[0].position, sizeof(c));
			check(c[0] == 0x7FC00000u && c[1] == 0xFFC00000u && c[2] == 0xFFC00000u,
					"occ-empty: a vertexless record's centre is retail's NaN");
			threedi_3di3_free(&m);
		} else {
			check(false, "occ-empty: read back");
		}
	}

	// A spot light's cone survives the scene: whatever the half-angle, the
	// rebuilt cosine, byte and view_proj are the ones stored.
	for (int i = 0; i < 40; ++i) {
		char text[256];
		std::snprintf(text, sizeof(text),
				"o3d 1\nmodel L\nlod 0\npart 0 0 0 0\n"
				"light 0 1 0 1.5 0 6 24 0 0 255 255 255 0 0 0 0x48 0.3 0.1 -1 %.6f\n",
				1.0 + i * 0.4371);
		round_trip("spot-" + std::to_string(i), text);
	}

	// A skinned strip authored on a bone (dM1A1's hull has no mesh part)
	// comes back on that bone, so the bone keeps its bounds.
	round_trip("skinned-on-bones",
			"o3d 1\nmodel BONES\nskinned 1\nmaterial VS_SKBASIC\nlod 0\npart 0 0 0 0\n"
			"strip 0\nbones 0\nv 1 1 0 0 0 1 0 0 0 0 0 0 1 0 0\nv -1 1 0 0 0 1 0 1 0 0 0 0 1 0 0\n"
			"v -1 -1 1 0 0 1 1 1 0 0 0 0 1 0 0\nt 0 1 2\n"
			"part 0 3 0 0\n"
			"strip 0\nbones 1\nv 4 0 0 0 0 1 0 0 0 0 0 0 1 0 0\nv 3 1 0 0 0 1 0 1 0 0 0 0 1 0 0\n"
			"v 3 0 2 0 0 1 1 1 0 0 0 0 1 0 0\nt 0 1 2\n"
			"panm 0 0\npanm 1 0\n");

	// A skinned vertex carries four bone-table slots and three weights; slot 3
	// takes the rest, 1 - (w0 + w1 + w2), as retail's vertex shader blends.
	// The fourth slot comes back from `scene`, and build refuses a weight that
	// is no finite number from 0 to 1 and weights summing past 1 (retail's
	// four-decimal weights reach 1.0001 in float, ArmGlovD, which builds).
	{
		const std::string head = "o3d 1\nmodel FOUR\nskinned 1\nmaterial VS_SKBASIC\nlod 0 gnrc\n"
				"part 0 0 0 0\npart 0 0 0 1\npart 1 0 0 2\nstrip 0\nbones 0 1 2\n";
		const std::string tail = "v 1 0 0 0 0 1 1 0 0 1 2 2 0.25 0.25 0.25\nv 0 1 0 0 0 1 0 1 0 1 1 2 0.5 0.5 0\n"
				"t 0 1 2\npanm 0 0\npanm 1 0\npanm 2 1\n";
		round_trip("fourth-slot", head + "v 0 0 0 0 0 1 0 0 2 1 0 1 0.5 0.25 0\n" + tail);
		Threedi3di3 m{};
		if (threedi_3di3_read(path_of("fourth-slot", ".3di").c_str(), &m) == 0) {
			const ThreediVertex &v = m.lods[0].vertices.items[0];
			const ThreediTriangleStrip &st = m.lods[0].strips[0];
			check(v.bone_indices[0] == 2 && v.bone_indices[1] == 1 && v.bone_indices[2] == 0 && v.bone_indices[3] == 1 &&
							v.bone_weights[0] == 0.5f && v.bone_weights[1] == 0.25f && v.bone_weights[2] == 0.0f,
					"fourth-slot: the four slots and three weights are stored as given");
			ThreediSkinInfluence influences[4];
			threedi_skin_influences(&v, st.bone_table, st.bone_table_length, influences);
			check(influences[0].part == 2 && influences[3].slot == 1 && influences[3].part == 1 &&
							influences[3].weight == 0.25f,
					"fourth-slot: slot 3's part takes the rest");
			threedi_3di3_free(&m);
		} else {
			check(false, "fourth-slot: read back");
		}
		check(read_file_text(path_of("fourth-slot", ".rt.o3d")).find(" 2 1 0 1 0.5 0.25 0\n") != std::string::npos,
				"fourth-slot: scene writes the four slots");
		check(build("weights-retail-sum", head + "v 0 0 0 0 0 1 0 0 0 1 2 0 0.4487 0.3871 0.1643\n" + tail),
				"weights-retail-sum: retail's 1.0001 builds");
		refuses("weights-sum", head + "v 0 0 0 0 0 1 0 0 0 1 2 0 0.5 0.5 0.01\n" + tail);
		refuses("weights-negative", head + "v 0 0 0 0 0 1 0 0 0 1 2 0 1.25 -0.25 0\n" + tail);
		refuses("weights-above-one", head + "v 0 0 0 0 0 1 0 0 0 1 2 0 1.5 0 0\n" + tail);
		refuses("weights-nan", head + "v 0 0 0 0 0 1 0 0 0 1 2 0 nan 0 0\n" + tail);
		refuses("weights-infinite", head + "v 0 0 0 0 0 1 0 0 0 1 2 0 inf 0 0\n" + tail);
		refuses("weights-three-slots", head + "v 0 0 0 0 0 1 0 0 0 1 2 0.5 0.25 0.25\n" + tail);
	}

	// A strip with vertices and no triangle is carried as it is.
	round_trip("empty-strip", "o3d 1\nmodel E\nmaterial FF_ST_OP\nlod 0\npart 0 0 0 0\nstrip 0 0\nv 0 0 0 0 0 1 0 0\n");
	check(read_file_text(path_of("empty-strip", ".rt.o3d")).find("# dropped") == std::string::npos,
			"empty-strip: nothing is reported dropped");

	// A triangle that repeats a corner is refused: the scene of the model
	// could not carry it (the loader's decode drops it).
	refuses("repeated-corner", "o3d 1\nmodel R\nmaterial FF_ST_OP\nlod 0\npart 0 0 0 0\nstrip 0\n"
			"v 0 0 0 0 0 1 0 0\nv 1 0 0 0 0 1 1 0\nv 0 1 0 0 0 1 0 1\nt 0 0 1\n");

	// What the scene text cannot hold is reported: a nameless model and a
	// LOD without a type.
	{
		ThreediBuildModel m;
		const int lod = m.add_lod(0, "");
		m.add_part(lod, 0, ThreediBuildVec3{});
		std::vector<uint8_t> bytes;
		check(threedi_build_mint(m, bytes), "nameless: mint");
		std::ofstream(path_of("nameless", ".3di"), std::ios::binary)
				.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		check(threedi_cli::cmd_scene(path_of("nameless", ".3di").c_str(), path_of("nameless", ".rt.o3d").c_str()) == 0,
				"nameless: scene");
		const std::string text = read_file_text(path_of("nameless", ".rt.o3d"));
		check(text.find("# dropped: the model has no name") != std::string::npos &&
						text.find("# dropped: lod 0 has no type") != std::string::npos,
				"nameless: the empty name and LOD type are reported");
	}

	// Section bounds come from the section's source geometry, as WriteCOBJ
	// takes them: a rigid section from its part's render floats in the
	// collision LOD (0.1, not the 8.8 corner 0.09765625) and the occlusion
	// records it parents; a skinned mesh section, which no weight names,
	// keeps the empty sentinels.
	{
		const std::string rigid = "o3d 1\nmodel SRC\nmaterial FF_ST_OP\nlod 0\npart 0 0 0 0\nstrip 0\n"
				"v 0 0 0.1 0 0 1 0 0\nv 1 0 0.1 0 0 1 1 0\nv 0 1 0.1 0 0 1 0 1\nt 0 1 2\npanm 0 0\n"
				"occ 2 0 0\nov 0 0 -2\nov 1 0 -2\nov 0 1 -2\nop 0 0 -1 -2\nof 0 1 2 0\n"
				"cobj 0 0 0 0\ncv 0 0 0.1\ncv 1 0 0.1\ncv 0 1 0.1\ncf 0 1 2\n";
		round_trip("section-source", rigid);
		Threedi3di3 m{};
		if (threedi_3di3_read(path_of("section-source", ".3di").c_str(), &m) == 0 && m.collision != nullptr &&
				m.collision->object_count == 1) {
			const ThreediCollisionObject &o = m.collision->objects[0];
			check(o.max[2] == 6553, "section-source: bounds from the collision LOD's render floats");
			check(o.min[2] == -2 * 65536, "section-source: bounds cover the occlusion records the section parents");
			threedi_3di3_free(&m);
		} else {
			check(false, "section-source: read back");
		}
		round_trip("skinned-mesh-section", kSkinned);
		if (threedi_3di3_read(path_of("skinned-mesh-section", ".3di").c_str(), &m) == 0 && m.collision != nullptr &&
				m.collision->object_count == 3) {
			const ThreediCollisionObject &o = m.collision->objects[2];
			check(o.min[0] == 10000 * 65536 && o.max[0] == -10000 * 65536 && o.radius == 0,
					"skinned-mesh-section: the mesh section keeps the sentinels (no weight names its part)");
			threedi_3di3_free(&m);
		} else {
			check(false, "skinned-mesh-section: read back");
		}
	}

	// A part that draws nothing keeps the point its sphere sits on.
	{
		round_trip("seeded-part", "o3d 1\nmodel SEED\nlod 0\npart 0 0 0 0\npart 0 1 2 3 1.5 2.5 3.5\npanm 0 0\npanm 1 0\n");
		Threedi3di3 m{};
		if (threedi_3di3_read(path_of("seeded-part", ".3di").c_str(), &m) == 0 && m.lod_count == 1 &&
				m.lods[0].render_object_count == 2) {
			const ThreediRenderObject &ro = m.lods[0].render_objects[1];
			// Mission (1.5, 2.5, 3.5) in model axes (-y, z, x).
			check(ro.bounding_center[0] == -2.5f && ro.bounding_center[1] == 3.5f && ro.bounding_center[2] == 1.5f &&
							ro.bounding_radius == 0.0f,
					"seeded-part: the given centre, radius 0");
			threedi_3di3_free(&m);
		} else {
			check(false, "seeded-part: read back");
		}
		refuses("seeded-drawing-part", "o3d 1\nmodel SEED\nmaterial FF_ST_OP\nlod 0\npart 0 0 0 0 1 1 1\nstrip 0\n"
				"v 0 0 0 0 0 1 0 0\nv 1 0 0 0 0 1 1 0\nv 0 1 0 0 0 1 0 1\nt 0 1 2\n");
	}

	// PANM tables are canonical: row i transforms part i (every retail one).
	{
		const std::string head = "o3d 1\nmodel PANM\nlod 0\npart 0 0 0 0\npart 0 0 0 1\n";
		check(build("panm-canonical", head + "panm 0 0\npanm 1 0\n"), "panm-canonical: built");
		refuses("panm-order", head + "panm 1 0\npanm 0 0\n");
		refuses("panm-duplicate", head + "panm 0 0\npanm 0 0\n");
	}

	// What the Blender exporter writes: a `cxlt` row may carry its helper's
	// name as a comment, a row at a section's offset quantizes as the derived
	// one does, and an empty register name (IBlock02) builds.
	{
		const std::string two = "o3d 1\nmodel ROWS\nlod 0\npart 0 0 0 0\npart 0 0.1 0.2 0.3\n"
				"cobj 0 0 0 0\ncobj 0 0.1 0.2 0.3\n";
		check(build("cxlt-derived-two", two) && build("cxlt-helper", two + "cxlt 0.1 0.2 0.3  # ~PP02 attach\n"),
				"cxlt-helper: built");
		check(read_file_text(path_of("cxlt-derived-two", ".3di")) == read_file_text(path_of("cxlt-helper", ".3di")),
				"cxlt-helper: a row at the section's offset gives the derived bytes");
		round_trip("empty-register", "o3d 1\nmodel REG\nregister \"\"\nlod 0\npart 0 0 0 0\n");
	}

	// A light's rate and phase pack as WriteLGHT packs them: times 256 in
	// float, truncated (0.1 -> 25, 0.3 -> 76; rounding gave 26 and 77).
	{
		const std::string text = "o3d 1\nmodel LIGHT\nlod 0\npart 0 0 0 0\n"
				"light 0 1 0 1.5 0 6 24 0.1 0.3 255 255 255 0 0 0 0x40\n";
		round_trip("light-pack", text);
		Threedi3di3 m{};
		if (threedi_3di3_read(path_of("light-pack", ".3di").c_str(), &m) == 0) {
			check(m.light_count == 1 && m.lights[0].rate == 25 && m.lights[0].phase == 76, "light-pack: rate and phase truncate");
			threedi_3di3_free(&m);
		} else {
			check(false, "light-pack: read back");
		}
	}

	// Output is written whole or not at all: a refused build or an unwritable
	// target leaves the last good file and no partial one.
	{
		const std::string good = kSkinned;
		check(build("write-safety", good), "write-safety: build");
		const std::string before = read_file_text(path_of("write-safety", ".3di"));
		std::ofstream(path_of("write-safety-bad", ".o3d"), std::ios::binary) << good << "bogus 1\n";
		check(threedi_cli::cmd_build(path_of("write-safety-bad", ".o3d").c_str(), path_of("write-safety", ".3di").c_str()) != 0,
				"write-safety: the bad scene is refused");
		check(read_file_text(path_of("write-safety", ".3di")) == before, "write-safety: a refused build keeps the last model");
		const std::string scene_out = path_of("write-safety", ".rt.o3d");
		std::ofstream(scene_out, std::ios::binary) << "stale";
		check(threedi_cli::cmd_scene(path_of("write-safety", ".3di").c_str(), scene_out.c_str()) == 0 &&
						read_file_text(scene_out).rfind("o3d 1", 0) == 0,
				"write-safety: scene replaces an existing file");
		std::filesystem::create_directories(dir / "a-folder.o3d");
		check(threedi_cli::cmd_scene(path_of("write-safety", ".3di").c_str(), (dir / "a-folder.o3d").string().c_str()) != 0,
				"write-safety: scene onto a folder fails");
		check(!std::filesystem::exists(scene_out + ".part") && !std::filesystem::exists(dir / "a-folder.o3d.part") &&
						!std::filesystem::exists(path_of("write-safety", ".3di.part")),
				"write-safety: no partial file is left behind");
	}

	// The reader is strict (docs/threedi/o3d-scene-format.md "Validation"):
	// each case below changes one field of a scene build accepts.
	{
		const std::string head = "o3d 1\nmodel STRICT\nmaterial FF_ST_OP\ntexture skin.tga\nlod 0\npart 0 0 0 0\nstrip 0\n"
				"v 0 0 0 0 0 1 0 0\nv 1 0 0 0 0 1 1 0\nv 0 1 0 0 0 1 0 1\nt 0 1 2\npanm 0 0\n";
		const std::string face = "cobj 0\ncv 0 0 0\ncv 1 0 0\ncv 0 1 0\ncf 0 1 2 1 0x100\n";
		check(build("strict-base", head + face), "strict-base: built");
		Threedi3di3 m{};
		if (threedi_3di3_read(path_of("strict-base", ".3di").c_str(), &m) == 0 && m.collision != nullptr &&
				m.collision->face_count == 1) {
			check(m.collision->faces[0].material_flags == 0x100, "strict-base: cf flags read 0x hex");
			threedi_3di3_free(&m);
		} else {
			check(false, "strict-base: read back");
		}
		const auto swap = [](std::string s, const std::string &from, const std::string &to) {
			s.replace(s.find(from), from.size(), to);
			return s;
		};
		const std::string base = head + face;
		refuses("strict-cf-flags", swap(base, "cf 0 1 2 1 0x100", "cf 0 1 2 1 x100"));
		refuses("strict-cf-poly", swap(base, "cf 0 1 2 1 0x100", "cf 0 1 2 abc"));
		refuses("strict-texture-slot", swap(base, "texture skin.tga", "texture skin.tga x"));
		refuses("strict-lod", swap(base, "lod 0", "lod abc"));
		refuses("strict-matflags", swap(base, "texture skin.tga", "texture skin.tga\nmatflags 300"));
		refuses("strict-alphagen", swap(base, "texture skin.tga", "texture skin.tga\nalphagen 50 -1 200 120 70000"));
		refuses("strict-light-rate", base + "light 0 1 0 1.5 0 6 24 -1 0 255 255 255 0 0 0 0x40\n");
		refuses("strict-trailing", swap(base, "t 0 1 2", "t 0 1 2 9"));
		refuses("strict-unclosed", base + "userpoint \"abc 0 0 0 0 0 1 0\n");
		refuses("strict-quote-in-name", base + "userpoint ab\"c 0 0 0 0 0 1 0\n");
		refuses("strict-track-axis", base + "track trans 50 - 0 0 256 9\n");
		refuses("strict-cv-range", swap(base, "cv 1 0 0", "cv 128 0 0"));
		// Retail reads a bullet face's corners as signed 16-bit indices: a
		// section holds at most 32,768 vertices, and the refusal says so once.
		std::string full = head + "cobj 0\n";
		full.reserve(full.size() + 32770 * 9);
		for (int i = 0; i < 32770; ++i) full += "cv 0 0 0\n";
		refuses_saying("strict-cv-count", full,
				"collision section 0 exceeds 32,768 vertices: retail reads a bullet face's corners as signed 16-bit indices");
		const std::string said = read_file_text(path_of("strict-cv-count", ".err"));
		check(said.find("exceeds 32,768") == said.rfind("exceeds 32,768"), "strict-cv-count: said once");
		// A strip's triangles index its vertices with u16 words, said once too.
		std::string long_strip = "o3d 1\nmodel STRIP\nmaterial FF_ST_OP\nlod 0\npart 0 0 0 0\nstrip 0\n";
		long_strip.reserve(long_strip.size() + 65537 * 18);
		for (int i = 0; i < 65537; ++i) long_strip += "v 0 0 0 0 0 1 0 0\n";
		refuses_saying("strict-strip-count", long_strip, "strip exceeds 65,535 vertices: its triangles index them with u16 words");
		const std::string strip_said = read_file_text(path_of("strict-strip-count", ".err"));
		check(strip_said.find("exceeds 65,535") == strip_said.rfind("exceeds 65,535"), "strict-strip-count: said once");
		// A texture name is the MTRL row's 16-byte field, counted in bytes,
		// printable ASCII and a file name alone; retail fills all 16 bytes
		// (bo105blur.dds.tg), and the loader's name cut decides what loads.
		refuses_saying("strict-texture-bytes", swap(base, "texture skin.tga", "texture seventeen_chars.tga"),
				"texture name 'seventeen_chars.tga' is 19 bytes: the MTRL field holds 16");
		refuses_saying("strict-texture-utf8", swap(base, "texture skin.tga", "texture \xD1\x81\xD1\x82\xD0\xB2\xD0\xBE\xD0\xBB_c.tga"),
				"holds the byte 0xD1: a texture name is printable ASCII");
		refuses_saying("strict-texture-folder", swap(base, "texture skin.tga", "texture tex/skin.tga"),
				"texture name 'tex/skin.tga' names a folder");
		builds_saying("texture-sixteen-bytes", swap(base, "texture skin.tga", "texture bo105blur.dds.tg"), "note: texture", false);
		builds_saying("texture-png", swap(base, "texture skin.tga", "texture skin.png"),
				"note: texture 'skin.png' loads only as 'skin.dds'");
		builds_saying("texture-cut", swap(base, "texture skin.tga", "texture a.bmp.tga"),
				"note: texture 'a.bmp.tga' loads only as 'a.dds': the game opens 'a.bmp'");
		// Only the rows that loader reads: a chunk producer (type 16) reads the
		// name as written, and type 1 takes the plain path on the whole name.
		builds_saying("texture-chunk", swap(base, "texture skin.tga", "texture field.nq8 1 16"), "note: texture", false);
		builds_saying("texture-plain", swap(base, "texture skin.tga", "texture skin.png 1 1"), "note: texture", false);
		std::string planes = base + "occ 0 0 0\nov 0 0 0\nov 1 0 0\nov 0 1 0\n";
		for (int i = 0; i < 33; ++i) planes += "op 0 0 1 " + std::to_string(i) + "\n";
		refuses("strict-occ-planes", planes);
		std::string verts = base + "occ 0 0 0\n";
		for (int i = 0; i < 129; ++i) verts += "ov " + std::to_string(i) + " 0 0\n";
		refuses("strict-occ-verts", verts);
		// A ninth `sitex` seat (any case) takes the control seat and the game still
		// loads the model: a note, never a refusal; eight say nothing.
		std::string seats = base;
		for (int i = 0; i < 8; ++i) seats += "userpoint SiteX0" + std::to_string(i) + " 0 0 0 1 0 0 0\n";
		builds_saying("seats-eight", seats, "sitex seats", false);
		builds_saying("seats-nine", seats + "userpoint sitex08 0 0 0 1 0 0 0\n",
				"note: more than 8 sitex seats: the game takes the ninth for the control seat");
		refuses("strict-volume-planes", base + "cvolume 1 0 0 0 0 1 1 1\ncp 1 0 0 -1\ncp -1 0 0 0\ncp 0 1 0 -1\n");
		// Accepted: a UTF-8 byte order mark, and a name holding a vertical tab
		// (the scene quotes it, so it comes back).
		check(build("strict-bom", "\xEF\xBB\xBF" + base), "strict-bom: a byte order mark builds");
		round_trip("strict-vt-name", base + "userpoint \"a\vb\" 0 0 0 0 0 1 0\n");
	}

	// Retail lets strips share one vertex window, across parts too (Dblkhwk1's
	// rotor strips of parts 3 and 4, Armry01's part 3): `scene` writes such a
	// strip with the vertices its own triangles use, so each part's sphere
	// comes back over its own geometry rather than over its neighbour's too.
	{
		const std::string text = "o3d 1\nmodel SHARE\nmaterial FF_ST_OP\nlod 0\npart 0 0 0 0\nstrip 0\n"
				"v 0 0 0 0 0 1 0 0\nv 1 0 0 0 0 1 1 0\nv 0 1 0 0 0 1 0 1\nt 0 1 2\n"
				"part 0 10 0 0\nstrip 0\nv 10 0 0 0 0 1 0 0\nv 13 0 0 0 0 1 1 0\nv 10 3 0 0 0 1 0 1\nt 0 1 2\n";
		check(build("shared-window", text), "shared-window: build");
		Threedi3di3 m{};
		const bool loaded = threedi_3di3_read(path_of("shared-window", ".3di").c_str(), &m) == 0;
		const bool read = loaded && m.lod_count == 1 && m.lods[0].strip_count == 2 && m.lods[0].render_object_count == 2;
		check(read, "shared-window: read the built model");
		if (loaded && !read) threedi_3di3_free(&m);
		if (read) {
			// Pool the two strips' windows into one, as retail's exporter does.
			ThreediLod &lod = m.lods[0];
			ThreediTriangleStrip &a = lod.strips[0], &b = lod.strips[1];
			const int32_t shift = b.start_vertex - a.start_vertex;
			for (int i = 0; i < b.num_indices; ++i)
				lod.indices.indices[b.index_offset + i] = static_cast<uint16_t>(lod.indices.indices[b.index_offset + i] + shift);
			a.num_vertices = b.num_vertices = a.num_vertices + b.num_vertices;
			b.start_vertex = a.start_vertex;
			float centre[2][3], radius[2];
			for (int p = 0; p < 2; ++p) {
				std::memcpy(centre[p], lod.render_objects[p].bounding_center, sizeof(centre[p]));
				radius[p] = lod.render_objects[p].bounding_radius;
			}
			check(threedi_3di3_write(path_of("shared-window", ".pooled.3di").c_str(), &m) == 0,
					"shared-window: write the pooled model");
			threedi_3di3_free(&m);
			check(threedi_cli::cmd_scene(path_of("shared-window", ".pooled.3di").c_str(),
								  path_of("shared-window", ".pooled.o3d").c_str()) == 0 &&
							threedi_cli::cmd_build(path_of("shared-window", ".pooled.o3d").c_str(),
									path_of("shared-window", ".rt.3di").c_str()) == 0,
					"shared-window: scene and build the pooled model");
			Threedi3di3 back{};
			if (threedi_3di3_read(path_of("shared-window", ".rt.3di").c_str(), &back) == 0) {
				for (int p = 0; p < 2; ++p)
					check(std::memcmp(back.lods[0].render_objects[p].bounding_center, centre[p], sizeof(centre[p])) == 0 &&
									back.lods[0].render_objects[p].bounding_radius == radius[p],
							"shared-window: part " + std::to_string(p) + " keeps the sphere of its own vertices");
				threedi_3di3_free(&back);
			} else {
				check(false, "shared-window: read the rebuilt model");
			}
		}
	}

	// A bullet face names its normal by a signed 16-bit index: a section with
	// more distinct face normals is refused, naming its part and what to
	// change (a rough 130 x 130 height field: 33,282 faces, nearly every one
	// facing its own way).
	{
		std::string rough = "o3d 1\nmodel ROUGH\nlod 0\npart 0 0 0 0\ncobj 0\n";
		uint32_t seed = 12345;
		for (int j = 0; j < 130; ++j)
			for (int i = 0; i < 130; ++i) {
				seed = seed * 1664525u + 1013904223u;
				rough += "cv " + std::to_string(i * 0.25) + " " + std::to_string(j * 0.25) + " " +
						std::to_string(static_cast<double>((seed >> 8) % 2048) / 256.0) + "\n";
			}
		for (int j = 0; j < 129; ++j)
			for (int i = 0; i < 129; ++i) {
				const int a = j * 130 + i, b = a + 1, c = a + 130, d = c + 1;
				rough += "cf " + std::to_string(a) + " " + std::to_string(b) + " " + std::to_string(c) + "\n";
				rough += "cf " + std::to_string(b) + " " + std::to_string(d) + " " + std::to_string(c) + "\n";
			}
		std::string said;
		check(build_with_cli("collision-normals", rough, said) != 0, "collision-normals: refused");
		check(said.find("collision section 0 (part 0) has ") != std::string::npos &&
						said.find(" distinct bullet-face normals, past the 32,768 a face's signed 16-bit normal index "
								  "reaches") != std::string::npos,
				"collision-normals: names the part and the limit (it said: " + said.substr(0, 400) + ")");
	}

	// A 3DI3 chunk says its payload length in 24 bits: the writer refuses a
	// model one of whose chunks outgrows that and says which chunk, and how
	// large, so the CLI can tell the author (a 262,200-vertex LOD with
	// tangents holds 16,780,812 bytes of VERT).
	{
		ThreediBuildModel m;
		m.name = "BIG";
		m.tangents = true; // 64-byte vertices
		const int lod = m.add_lod();
		const int part = m.add_part(lod, 0, ThreediBuildVec3{});
		m.add_material("FF_ST_OP", "big.tga");
		ThreediBuildStrip strip;
		strip.vertices.resize(262200);
		m.lods[lod].parts[part].strips.push_back(std::move(strip));
		m.add_panm(lod, part, 0);
		std::vector<uint8_t> bytes;
		ThreediChunkOverflow overflow{};
		check(!threedi_build_mint(m, bytes, &overflow), "overflow: refused");
		check(std::string(overflow.chunk) == "ROOT/RDTA/RLOD/VERT" && overflow.bytes == 12u + 262200u * 64u,
				std::string("overflow: names the VERT chunk and its size (") + overflow.chunk + ", " +
						std::to_string(overflow.bytes) + ")");
	}

	std::printf("o3d_build_test: %d failures\n", failures);
	return failures == 0 ? 0 : 1;
}
