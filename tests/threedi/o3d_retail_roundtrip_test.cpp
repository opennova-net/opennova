// Retail models through opennova-3di and back: `scene` writes each one as .o3d,
// `build` mints it again, and `compare` must call the two the same model
// (strip layout and float noise aside). The models cover what the scene text
// carries: Armry01 (bldg LODs, detail textures, lights, occlusion, blink
// boxes, a register-driven rgbgen), Dblkhwk1 (register-driven rotors in MTRX
// frames, an empty LOD, tangents), US01 (the skinned person layout), ArmsG
// and Mp5b_1st (the first-person rig; collision faces the 8.8 grid collapses).
// Gated on OPENNOVA_JO_ASSETS (docs/asset-gated-tests.md).
//
//   o3d_retail_roundtrip_test <opennova-3di> <scratch dir>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "common/retail_paths.h"

namespace {

int run(const std::string &cmd) {
	// cmd.exe strips one pair of outer quotes: wrap the whole command.
	const std::string line = "\"" + cmd + "\"";
	std::fflush(stdout);
	return std::system(line.c_str());
}

std::string quoted(const std::string &s) { return "\"" + s + "\""; }

} // namespace

int main(int argc, char **argv) {
	if (argc != 3) {
		std::fprintf(stderr, "usage: o3d_retail_roundtrip_test <opennova-3di> <scratch dir>\n");
		return 2;
	}
	const std::string cli = quoted(argv[1]);
	const std::string dir = argv[2];
	int failures = 0, ran = 0;
	for (const char *name : {"Armry01", "Dblkhwk1", "US01", "ArmsG", "Mp5b_1st"}) {
		const std::string model = retail::asset_file((std::string(name) + ".3di").c_str());
		if (model.empty()) {
			std::printf("SKIP-LEG: needs %s.3di in OPENNOVA_JO_ASSETS\n", name);
			continue;
		}
		++ran;
		const std::string o3d = dir + "/" + name + ".rt.o3d";
		const std::string rebuilt = dir + "/" + name + ".rt.3di";
		if (run(cli + " scene " + quoted(model) + " -o " + quoted(o3d)) != 0 ||
				run(cli + " build " + quoted(o3d) + " -o " + quoted(rebuilt)) != 0 ||
				run(cli + " compare " + quoted(model) + " " + quoted(rebuilt)) != 0) {
			std::fprintf(stderr, "%s: the scene round trip is not the same model\n", name);
			++failures;
		}
	}
	if (ran == 0) return retail::skip("OPENNOVA_JO_ASSETS with Armry01.3di, Dblkhwk1.3di, US01.3di, ArmsG.3di, Mp5b_1st.3di");
	if (failures == 0) std::printf("o3d_retail_roundtrip_test: ok (%d models)\n", ran);
	return failures == 0 ? 0 : 1;
}
