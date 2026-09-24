// Exercise the same command handlers the CLI dispatches, including negative
// cases: successful readback alone does not establish semantic equivalence.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "../../apps/threedi_cli/threedi_cli.h"

namespace {
int failures = 0;
void check(bool ok, const char *what) {
	if (!ok) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		++failures;
	}
}

const std::string prefix = "o3d 1\nmodel CHECK\nskinned 1\nuv1 1\nmaterial VS_SKBASIC\n"
		"lod 0 gnrc\npart 0 0 0 0\npart 0 0 0 1\npart 0 0 0 0\nstrip 0\nbones 0 1\n";
const std::string verts =
		"v 0 0 0 0 0 1 0 0 0 0 0 1 0 0.75 0.25 0\n"
		"v 1 0 0 0 0 1 1 0 1 0 0 1 0 0.5 0.5 0\n"
		"v 0 1 0 0 0 1 0 1 0 1 0 1 0 0.25 0.75 0\n";
const std::string suffix = "t 0 1 2\npanm 0 0\npanm 1 0\npanm 2 0\n";

std::string replace(std::string s, const std::string &from, const std::string &to) {
	s.replace(s.find(from), from.size(), to);
	return s;
}
} // namespace

int main(int argc, char **argv) {
	if (argc != 2) return 2;
	const auto dir = std::filesystem::path(argv[1]) / "o3d-commands";
	std::filesystem::create_directories(dir);
	const auto build = [&](const char *name, const std::string &text, bool valid = true) {
		const auto scene = (dir / (std::string(name) + ".o3d")).string();
		const auto model = (dir / (std::string(name) + ".3di")).string();
		std::ofstream(scene) << text;
		const int result = threedi_cli::cmd_build(scene.c_str(), model.c_str());
		check(valid ? result == 0 : result != 0, name);
		return model;
	};
	const auto original = build("original", prefix + verts + suffix);
	const auto compare = [&](const char *name, const std::string &text, bool same) {
		const auto path = build(name, text);
		check(threedi_cli::cmd_compare(original.c_str(), path.c_str()) == (same ? 0 : 1), name);
	};
	compare("identical", prefix + verts + suffix, true);
	compare("cyclic-corners", prefix + verts + replace(suffix, "t 0 1 2", "t 1 2 0"), true);
	compare("winding", prefix + verts + replace(suffix, "t 0 1 2", "t 0 2 1"), false);
	compare("weights", prefix + replace(verts, "0.75 0.25 0", "0.25 0.75 0") + suffix, false);
	compare("bone-table", replace(prefix, "bones 0 1", "bones 1 0") + verts + suffix, false);
	// UV swaps preserve each mean, but move the texture to different corners.
	compare("uv0", prefix + replace(replace(verts,
			"v 1 0 0 0 0 1 1 0", "v 1 0 0 0 0 1 0 1"),
			"v 0 1 0 0 0 1 0 1", "v 0 1 0 0 0 1 1 0") + suffix, false);
	compare("uv1", prefix + replace(replace(verts,
			"v 1 0 0 0 0 1 1 0 1 0", "v 1 0 0 0 0 1 1 0 0 1"),
			"v 0 1 0 0 0 1 0 1 0 1", "v 0 1 0 0 0 1 0 1 1 0") + suffix, false);
	compare("remapped-bones", replace(prefix, "bones 0 1", "bones 1 0") +
			replace(replace(replace(verts, "0.75 0.25 0", "0.25 0.75 0"),
					"v 1 0 0 0 0 1 1 0 1 0 0 1 0", "v 1 0 0 0 0 1 1 0 1 0 1 0 0"),
					"v 0 1 0 0 0 1 0 1 0 1 0 1 0", "v 0 1 0 0 0 1 0 1 0 1 1 0 0") + suffix, true);
	build("undeclared-track-register", prefix + verts + suffix + "track rotx 113 0 0 0 100\n", false);
	build("wrapped-panm-part", prefix + verts + "t 0 1 2\npanm 256 0\n", false);
	build("wrapped-panm-parent", prefix + verts + "t 0 1 2\npanm 0 256\n", false);
	build("undeclared-flipbook-register", replace(prefix, "material VS_SKBASIC\n",
			"material VS_SKBASIC\ntexanim 2 1 0\n") + verts + suffix, false);
	const std::string flipbook = replace(replace(prefix, "material VS_SKBASIC\n",
			"register TEX_CAMO1\nregister TEX_CAMO2\nmaterial VS_SKBASIC\ntexanim 2 1 1\n"),
			"model CHECK", "model FLIPBOOK") + verts + suffix;
	const auto flip_a = build("flipbook", flipbook);
	const auto flip_b = build("flipbook-remapped", replace(replace(flipbook,
			"register TEX_CAMO1\nregister TEX_CAMO2", "register TEX_CAMO2\nregister TEX_CAMO1"),
			"texanim 2 1 1", "texanim 2 1 0"));
	check(threedi_cli::cmd_compare(flip_a.c_str(), flip_b.c_str()) == 0, "remapped flipbook register");
	std::string nearby = "o3d 1\nmodel NEARBY\nmaterial FF_ST_OP\nlod 0\npart 0 0 0 0\n";
	for (double x : {0.0, 0.0015, -0.0015})
		nearby += "strip 0\nv " + std::to_string(x) + " 0 0 0 0 1 0 0\nv " + std::to_string(x + 1) +
				" 0 0 0 0 1 1 0\nv " + std::to_string(x) + " 1 0 0 0 1 0 1\nt 0 1 2\n";
	const auto nearby_path = build("nearby", nearby);
	check(threedi_cli::cmd_compare(nearby_path.c_str(), nearby_path.c_str()) == 0, "nearly coincident faces");
	const std::string plain = "o3d 1\nmodel FACES\nlod 0\npart 0 0 0 0\n";
	const std::string collision = "cobj 0\ncv 0 0 0\ncv 1 0 0\ncv 0 1 0\ncf 0 1 2\n";
	const auto collision_a = build("collision", plain + collision);
	const auto collision_b = build("collision-moved", plain +
			"cobj 0\ncv 10 0 0\ncv 11 0 0\ncv 10 1 0\ncf 0 1 2\n");
	check(threedi_cli::cmd_compare(collision_a.c_str(), collision_b.c_str()) == 1, "moved collision faces");
	const auto normal_a = build("stored-normal", plain + replace(collision, "cf 0 1 2", "cf 0 1 2 14 0 0.5 0 0.75"));
	const auto normal_scene = (dir / "stored-normal.rt.o3d").string();
	const auto normal_b = (dir / "stored-normal.rt.3di").string();
	check(threedi_cli::cmd_scene(normal_a.c_str(), normal_scene.c_str()) == 0, "scene with stored normal");
	check(threedi_cli::cmd_build(normal_scene.c_str(), normal_b.c_str()) == 0, "rebuild stored normal");
	check(threedi_cli::cmd_compare(normal_a.c_str(), normal_b.c_str()) == 0, "preserve stored collision normal");
	const std::string occlusion = "occ 0 0 0\nov 0 0 0\nov 1 0 0\nov 0 1 0\nop 0 0 1 0\nof 0 1 2 0\n";
	const auto occ_a = build("occlusion", plain + occlusion);
	const auto occ_b = build("occlusion-reversed", plain + replace(occlusion, "of 0 1 2", "of 0 2 1"));
	check(threedi_cli::cmd_compare(occ_a.c_str(), occ_b.c_str()) == 1, "reversed occlusion faces");
	std::string overflow = plain + "cobj 0\n";
	for (int i = 0; i < 32769; ++i) overflow += "cv 0 0 0\n";
	build("collision-index-overflow", overflow, false);
	std::printf("o3d_commands: %d failures\n", failures);
	return failures ? 1 : 0;
}
