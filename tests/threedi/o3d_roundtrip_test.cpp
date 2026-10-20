// Models through opennova-3di and back: `scene` writes each one as .o3d,
// `build` mints it again, and `compare` must call the two the same model
// (strip layout and float noise aside). The models are every committed .3di
// under fixtures/threedi (tests/fixtures/minimal_3di_gen.cpp), standing for the
// shipped models this row read until 2026-10-19 (Armry01, Dblkhwk1, US01,
// ArmsG, Mp5b_1st, FSldr03, SASBODY1, sasmg): the armory's bldg LOD, detail
// textures, lights, occlusion, blink boxes and register-driven rgbgen, the
// skinned person and the first-person viewmodel, the vehicles' register-driven
// parts, and the variants beside them.
// The CXLT table the scene carries must also come back row for row.
// The rebuild is a fixed point: build(scene(x)) byte for byte
// build(scene(build(scene(x)))).
//
// Nothing is excused. Any line `compare` calls a difference fails the model,
// and so does DRIFT (a value within its tolerance) outside the categories
// below: the collision words the builder derives from the stored 8.8 corners
// where retail took them from the authored floats the file does not keep
// (docs/threedi/o3d-scene-format.md). Everything else the scene carries
// exactly, the tangent frames included (`vt`), so drift there (a part pivot,
// a render corner, a tangent, a skin weight, a part sphere) is a regression,
// not noise.
//
//   o3d_roundtrip_test <opennova-3di> <scratch dir>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <formats/threedi/threedi_3di3.h>

#include "common/run_command.h"
#include "common/test_paths.h"

namespace {

using test_cmd::quoted;
using test_cmd::run;

// The DRIFT categories a round trip may show (compare.cpp's names).
const char *const kDerivedDrift[] = {
		"bullet-face corners (m, 8.8)",
		"bullet-face normals (degrees)",
		"bullet-face planes at the face (m)",
		"bullet-face boxes (m)",
		"dominant axes of diagonal bullet faces",
		"section boxes and midpoints (m)",
		"section radii (m)",
		"CMDL box (m)",
		"CMDL radii (m)",
};

bool derived_drift(const std::string &category) {
	for (const char *allowed : kDerivedDrift)
		if (category.compare(0, std::string(allowed).size(), allowed) == 0) return true;
	return false;
}

// Whether `compare`'s report (its stdout) holds only derived drift: prints
// every line that is a difference or drift in another category.
bool only_derived_drift(const std::string &name, const std::string &report_path) {
	std::ifstream in(report_path);
	if (!in) {
		std::fprintf(stderr, "%s: no compare report\n", name.c_str());
		return false;
	}
	bool ok = true;
	std::string line;
	while (std::getline(in, line)) {
		if (line.empty() || line.rfind("same model", 0) == 0 || line.rfind("different", 0) == 0) continue;
		if (line.rfind("drift: ", 0) == 0) {
			const size_t end = line.find(": ", 7);
			if (derived_drift(line.substr(7, end == std::string::npos ? std::string::npos : end - 7))) continue;
		}
		std::fprintf(stderr, "%s: %s\n", name.c_str(), line.c_str());
		ok = false;
	}
	return ok;
}

// The two models' CXLT tables, row for row.
bool same_cxlt(const std::string &a_path, const std::string &b_path) {
	using namespace opennova::threedi;
	Threedi3di3 a{}, b{};
	const bool read = threedi_3di3_read(a_path.c_str(), &a) == 0 && threedi_3di3_read(b_path.c_str(), &b) == 0;
	bool same = read && (a.collision == nullptr) == (b.collision == nullptr);
	if (same && a.collision != nullptr) {
		same = a.collision->translation_count == b.collision->translation_count;
		for (size_t i = 0; same && i < a.collision->translation_count; ++i)
			for (int k = 0; k < 3; ++k)
				same = same && a.collision->translations[i].translation[k] == b.collision->translations[i].translation[k];
	}
	threedi_3di3_free(&a);
	threedi_3di3_free(&b);
	return same;
}

// A file's bytes ("" where it does not read).
std::string bytes_of(const std::string &path) {
	std::ifstream in(path, std::ios::binary);
	return in ? std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()) : std::string();
}

} // namespace

int main(int argc, char **argv) {
	if (argc != 3) {
		std::fprintf(stderr, "usage: o3d_roundtrip_test <opennova-3di> <scratch dir>\n");
		return 2;
	}
	const std::string cli = quoted(argv[1]);
	const std::string dir = argv[2];
	int failures = 0, ran = 0;
	const std::filesystem::path root = test_paths_fixture("threedi");
	std::vector<std::filesystem::path> models;
	std::error_code ec;
	// The GUT suite's runtime witnesses written past what an author can state: a
	// PANM row out of part order or a light on a part LOD 0 lacks, which `build`
	// refuses by name (threedi_o3d_build pins the refusals).
	const char *const kUnauthorable[] = {"armory_special1_slide_part1", "armory_special2_slide_part1",
		"house_lod0_sine_rotx", "house_lod0_sine_rotx_uv1", "mount_heat_glow_slide_part1",
		"shed_lght0_sub2_origin_atten100", "tank_special1_slide_ewep01"};
	for (const auto &entry : std::filesystem::recursive_directory_iterator(root, ec)) {
		if (!entry.is_regular_file(ec) || entry.path().extension() != ".3di") continue;
		const std::string stem = entry.path().stem().string();
		if (std::find(std::begin(kUnauthorable), std::end(kUnauthorable), stem) != std::end(kUnauthorable)) continue;
		models.push_back(entry.path());
	}
	std::sort(models.begin(), models.end());
	for (const std::filesystem::path &path : models) {
		const std::string model = path.string();
		const std::string stem = path.stem().string();
		const char *name = stem.c_str();
		++ran;
		const std::string o3d = dir + "/" + name + ".rt.o3d";
		const std::string rebuilt = dir + "/" + name + ".rt.3di";
		const std::string report = dir + "/" + name + ".rt.compare.txt";
		if (run(cli + " scene " + quoted(model) + " -o " + quoted(o3d)) != 0 ||
				run(cli + " build " + quoted(o3d) + " -o " + quoted(rebuilt)) != 0) {
			std::fprintf(stderr, "%s: the scene does not build\n", name);
			++failures;
			continue;
		}
		// The fixed point: the rebuild's scene builds the rebuild again, byte for byte.
		const std::string again_o3d = dir + "/" + name + ".rt2.o3d", again = dir + "/" + name + ".rt2.3di";
		if (run(cli + " scene " + quoted(rebuilt) + " -o " + quoted(again_o3d)) != 0 ||
				run(cli + " build " + quoted(again_o3d) + " -o " + quoted(again)) != 0 || bytes_of(again).empty() ||
				bytes_of(again) != bytes_of(rebuilt)) {
			std::fprintf(stderr, "%s: build(scene(x)) is not a fixed point\n", name);
			++failures;
			continue;
		}
		const int compared = run(cli + " compare " + quoted(model) + " " + quoted(rebuilt) + " > " + quoted(report));
		const bool derived = only_derived_drift(name, report);
		if (compared != 0 || !derived) {
			std::fprintf(stderr, "%s: the scene round trip is not the same model\n", name);
			++failures;
		} else if (!same_cxlt(model, rebuilt)) {
			std::fprintf(stderr, "%s: the rebuilt CXLT table is not the model's\n", name);
			++failures;
		}
	}
	if (ran == 0) failures = 1;
	if (failures == 0) std::printf("o3d_roundtrip_test: ok (%d models)\n", ran);
	return failures == 0 ? 0 : 1;
}
