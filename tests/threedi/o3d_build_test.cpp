// opennova-3di `build` and `scene` over small scenes: what the scene text
// carries through build -> scene -> build byte for byte, and the malformed
// records build refuses (docs/threedi/o3d-scene-format.md "Validation").
// Drives the command handlers the CLI dispatches (opennova_3di_commands).
//
//   o3d_build_test <scratch dir>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <formats/threedi/threedi_3di3.h>

#include "threedi_cli.h"

using namespace opennova::threedi;

namespace {

int failures = 0;

void check(bool ok, const std::string &what) {
	if (!ok) {
		std::fprintf(stderr, "FAIL: %s\n", what.c_str());
		++failures;
	}
}

std::filesystem::path dir;

std::string slurp(const std::filesystem::path &path) {
	std::ifstream in(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

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
	if (rebuilt) check(slurp(path_of(name, ".3di")) == slurp(path_of(name, ".rt.3di")), name + ": byte-exact round trip");
}

// Build must refuse `text` (one field changed from an accepted scene).
void refuses(const std::string &name, const std::string &text) { check(!build(name, text), name + ": refused"); }

const std::string kSkinned =
		"o3d 1\nmodel SKIN\nskinned 1\nmaterial VS_SKBASIC\ntexture skin.tga\nlod 128 gnrc\n"
		"part 0 0 0 0\npart 0 0 0 1\npart 0 0 0 -1\nstrip 0 0\nbones 0 1\n"
		"v 1 1 0 0 0 1 0 0 0 1 0 1 0 0\nv -1 1 0 0 0 1 0 1 0 1 0 0.5 0.5 0\n"
		"v -1 -1 1 0 0 1 1 1 1 0 0 1 0 0\nv 1 -1 1 0 0 1 1 0 1 0 0 0.75 0.25 0\nt 0 1 2\nt 0 2 3\n"
		"panm 0 0\npanm 1 0\npanm 2 0\n"
		"cobj 0 0 0 0\ncsphere 0 0 0 0.5\ncobj 0 0 0 1\ncsphere 0 0 1 0.25\n"
		"cobj 0 0 0 -1\ncv 1 1 0\ncv -1 1 0\ncv -1 -1 1\ncf 0 1 2 1\n";

} // namespace

int main(int argc, char **argv) {
	if (argc != 2) {
		std::fprintf(stderr, "usage: o3d_build_test <scratch dir>\n");
		return 2;
	}
	dir = std::filesystem::path(argv[1]) / "o3d-build";
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
		const std::string scene = slurp(path_of("nan", ".rt.o3d"));
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

	// Output is written whole or not at all: a refused build or an unwritable
	// target leaves the last good file and no partial one.
	{
		const std::string good = kSkinned;
		check(build("write-safety", good), "write-safety: build");
		const std::string before = slurp(path_of("write-safety", ".3di"));
		std::ofstream(path_of("write-safety-bad", ".o3d"), std::ios::binary) << good << "bogus 1\n";
		check(threedi_cli::cmd_build(path_of("write-safety-bad", ".o3d").c_str(), path_of("write-safety", ".3di").c_str()) != 0,
				"write-safety: the bad scene is refused");
		check(slurp(path_of("write-safety", ".3di")) == before, "write-safety: a refused build keeps the last model");
		const std::string scene_out = path_of("write-safety", ".rt.o3d");
		std::ofstream(scene_out, std::ios::binary) << "stale";
		check(threedi_cli::cmd_scene(path_of("write-safety", ".3di").c_str(), scene_out.c_str()) == 0 &&
						slurp(scene_out).rfind("o3d 1", 0) == 0,
				"write-safety: scene replaces an existing file");
		std::filesystem::create_directories(dir / "a-folder.o3d");
		check(threedi_cli::cmd_scene(path_of("write-safety", ".3di").c_str(), (dir / "a-folder.o3d").string().c_str()) != 0,
				"write-safety: scene onto a folder fails");
		check(!std::filesystem::exists(scene_out + ".part") && !std::filesystem::exists(dir / "a-folder.o3d.part") &&
						!std::filesystem::exists(path_of("write-safety", ".3di.part")),
				"write-safety: no partial file is left behind");
	}

	std::printf("o3d_build_test: %d failures\n", failures);
	return failures == 0 ? 0 : 1;
}
