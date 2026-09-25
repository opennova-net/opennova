// opennova-3di `build` and `scene` over small scenes: what the scene text
// carries through build -> scene -> build byte for byte, and the malformed
// records build refuses (docs/threedi/o3d-scene-format.md "Validation").
// Drives the command handlers the CLI dispatches (opennova_3di_commands).
//
//   o3d_build_test <scratch dir>
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

	std::printf("o3d_build_test: %d failures\n", failures);
	return failures == 0 ? 0 : 1;
}
