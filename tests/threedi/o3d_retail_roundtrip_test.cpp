// Retail models through opennova-3di and back: `scene` writes each one as .o3d,
// `build` mints it again, and `compare` must call the two the same model
// (strip layout and float noise aside). The models cover what the scene text
// carries: Armry01 (bldg LODs, detail textures, lights, occlusion, blink
// boxes, a register-driven rgbgen, strips sharing a vertex window), Dblkhwk1
// (register-driven rotors in MTRX frames, an empty LOD, tangents, rotor strips
// of two parts sharing one window), US01 (the skinned person layout), ArmsG
// and Mp5b_1st (the first-person rig; collision faces the 8.8 grid collapses).
// The CXLT table the scene carries must also come back row for row (US01,
// ArmsG and Mp5b_1st ship rows that are not their sections' offsets).
//
// Nothing is excused. Any line `compare` calls a difference fails the model,
// and so does DRIFT (a value within its tolerance) outside the categories
// below: the words the builder derives by a rule of ours where retail's tool is
// unwitnessed (docs/threedi/o3d-scene-format.md): the tangent frames, and the
// collision words it derives from the stored 8.8 corners where retail took
// them from the authored floats the file does not keep. Everything else the
// scene carries exactly, so drift there (a part pivot, a render corner, a part
// sphere) is a regression, not noise.
// Gated on OPENNOVA_JO_ASSETS (docs/asset-gated-tests.md).
//
//   o3d_retail_roundtrip_test <opennova-3di> <scratch dir>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include <formats/threedi/threedi_3di3.h>

#include "common/retail_paths.h"

namespace {

int run(const std::string &cmd) {
	std::fflush(stdout);
#ifdef _WIN32
	// cmd.exe strips one pair of outer quotes: wrap the whole command.
	const std::string line = "\"" + cmd + "\"";
	return std::system(line.c_str());
#else
	return std::system(cmd.c_str());
#endif
}

std::string quoted(const std::string &s) { return "\"" + s + "\""; }

// The DRIFT categories a retail round trip may show (compare.cpp's names).
const char *const kDerivedDrift[] = {
		"tangent/bitangent values",
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
		const std::string report = dir + "/" + name + ".rt.compare.txt";
		if (run(cli + " scene " + quoted(model) + " -o " + quoted(o3d)) != 0 ||
				run(cli + " build " + quoted(o3d) + " -o " + quoted(rebuilt)) != 0) {
			std::fprintf(stderr, "%s: the scene does not build\n", name);
			++failures;
			continue;
		}
		const int compared = run(cli + " compare " + quoted(model) + " " + quoted(rebuilt) + " > " + quoted(report));
		const bool derived = only_derived_drift(name, report);
		if (compared != 0 || !derived) {
			std::fprintf(stderr, "%s: the scene round trip is not the same model\n", name);
			++failures;
		} else if (!same_cxlt(model, rebuilt)) {
			std::fprintf(stderr, "%s: the rebuilt CXLT table is not the shipped one\n", name);
			++failures;
		}
	}
	if (ran == 0) return retail::skip("OPENNOVA_JO_ASSETS with Armry01.3di, Dblkhwk1.3di, US01.3di, ArmsG.3di, Mp5b_1st.3di");
	if (failures == 0) std::printf("o3d_retail_roundtrip_test: ok (%d models)\n", ran);
	return failures == 0 ? 0 : 1;
}
