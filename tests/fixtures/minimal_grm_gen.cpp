// Generator + guard for fixtures/grm/face108.grm (fixtures/README.md): a facial
// texture mesh at the scale of the faces Black Hawk Down ships (JO ships none;
// BHD's loader is JO's), minted through our own GRM writer (formats/grm, the
// witnessed writer's form [orig: GRM writer @0x588320]) from small integer data,
// so no retail bytes live in the tree (ADR 0003). The shape is Delta01.grm's as
// the retired grm_roundtrip_retail row pinned it (2026-10-19): 108 vertices,
// 143 triangles, nine gestures of 22 parameters each, the first named "Sad",
// saved in 2002, and the tool's absolute texture paths (a drive and
// backslashes, which the game strips: grm::texture_load_name). Every index past
// 9 and 99 takes the writer's wider columns, which a small face never reaches.
// The vertices and offsets are ours: a 12 x 9 grid of texture points, four
// groups, each gesture moving the groups by its own eighth.
// Default: rebuild the file in memory and byte-compare the committed copy;
// `--write` (re)writes it.
#include <formats/grm/grm.h>

#include "common/file_io.h"
#include "common/test_paths.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace opennova;

namespace {

int failures = 0;

bool expect(bool cond, const std::string &msg) {
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", msg.c_str());
		++failures;
	}
	return cond;
}

const char *const kGroups[4] = {"Static", "MouthLeft", "MouthRight", "Brow"};
const char *const kGestures[9] = {"Sad", "Happy", "Angry", "Pain", "Talk1", "Talk2", "Blink", "Shout", "Normal"};

grm::File make_face() {
	grm::File f;
	f.saved = grm::SaveInfo{9, 20, 2002, 17, 5, 30, "OpenNova"};
	f.base_texture = "C:\\faces\\face01.tga";
	f.eye_textures = {"C:\\faces\\eye01.tga", "C:\\faces\\eyemask1.tga"};
	// A 12 x 9 grid over the texture, quarter-unit steps of 1/16: column c, row r.
	for (int r = 0; r < 9; ++r)
		for (int c = 0; c < 12; ++c) {
			grm::Vertex v;
			v.uv = grm::Point{0.125f + c / 16.0f, 0.25f + r / 16.0f};
			v.group = r < 3 ? kGroups[3] : (r > 5 ? (c < 6 ? kGroups[1] : kGroups[2]) : kGroups[0]);
			f.vertices.push_back(v);
		}
	// 143 triangles: the grid's cells split in two, row by row, until the count.
	for (int r = 0; r < 8 && f.triangles.size() < 143; ++r)
		for (int c = 0; c < 11 && f.triangles.size() < 143; ++c) {
			const int a = r * 12 + c, b = a + 1, d = a + 12, e = d + 1;
			f.triangles.push_back({a, b, e});
			if (f.triangles.size() < 143) f.triangles.push_back({a, e, d});
		}
	for (int g = 0; g < 9; ++g) {
		grm::Gesture gesture;
		gesture.name = kGestures[g];
		for (int p = 0; p < 22; ++p) {
			grm::Parameter parm;
			const float step = (g - 4) / 128.0f;
			parm.offset = grm::Point{step * ((p % 3) - 1), -step * ((p % 5) - 2) / 2.0f};
			parm.group = kGroups[p % 4];
			gesture.parameters.push_back(parm);
		}
		f.gestures.push_back(gesture);
	}
	return f;
}

} // namespace

int main(int argc, char **argv) {
	bool write_mode = false;
	for (int i = 1; i < argc; ++i)
		if (std::strcmp(argv[i], "--write") == 0) write_mode = true;
	const std::string path = test_paths_fixture("grm/face108.grm");
	std::vector<uint8_t> bytes;
	std::string error;
	if (!expect(grm::write(make_face(), bytes, error), "the face writes: " + error)) return 1;
	if (write_mode) {
		std::ofstream o(path, std::ios::binary);
		o.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		std::printf("wrote %s (%zu bytes)\n", path.c_str(), bytes.size());
	} else {
		std::vector<uint8_t> committed;
		if (expect(test_io::read_file(path, committed), path + " missing; run with --write"))
			expect(committed == bytes, path + " differs from the generator output; regenerate with --write");
	}
	grm::File back;
	expect(grm::parse(bytes.data(), bytes.size(), back, error), "the face reads back: " + error);
	expect(back.vertices.size() == 108 && back.triangles.size() == 143 && back.gestures.size() == 9 &&
					back.gestures[0].name == "Sad" && back.gestures[0].parameters.size() == 22 &&
					back.saved.year == 2002,
			"the face's shape");
	if (failures == 0 && !write_mode) std::printf("OK: fixtures/grm/face108.grm byte-reproducible\n");
	return failures == 0 ? 0 : 1;
}
