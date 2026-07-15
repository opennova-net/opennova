#include <til/til.h>

#include <cstdio>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

} // namespace

int main() {
	opennova::TilFile file;
	file.entries.push_back(opennova::make_til_overlay_entry(-2, -3, 0, 0));

	// The entry occupies inclusive world AABB X [-32,-16], Z [-48,-32].
	// Retail tests a candidate square against that box with an inclusive
	// radius-2 overlap. These corner vectors also pin the stored-negated Z
	// conversion instead of accidentally treating z_fixed as world Z.
	if (!expect(opennova::til_blocks_foliage(file, -34.0f, -50.0f, 2.0f),
	            "candidate square touching the entry minimum corner should block")) {
		return 1;
	}
	if (!expect(opennova::til_blocks_foliage(file, -14.0f, -30.0f, 2.0f),
	            "candidate square touching the entry maximum corner should block")) {
		return 1;
	}
	if (!expect(opennova::til_blocks_foliage(file, -24.0f, -40.0f, 2.0f),
	            "candidate centered inside a negative-Z tile should block")) {
		return 1;
	}
	if (!expect(!opennova::til_blocks_foliage(file, -34.001f, -50.0f, 2.0f),
	            "candidate just beyond the entry minimum X edge should not block")) {
		return 1;
	}
	if (!expect(!opennova::til_blocks_foliage(file, -14.0f, -29.999f, 2.0f),
	            "candidate just beyond the entry maximum Z edge should not block")) {
		return 1;
	}

	opennova::TilOverlayEntry unsnapped;
	unsnapped.x_fixed = opennova::til_x_fixed_from_world(3.25);
	unsnapped.z_fixed = opennova::til_z_fixed_from_world(-5.5);
	file.entries.push_back(unsnapped);
	if (!expect(opennova::til_blocks_foliage(file, 19.25f, 10.5f, 0.0f),
	            "unsnapped entry maximum corner should remain inclusive")) {
		return 1;
	}

	if (!expect(!opennova::til_blocks_foliage(opennova::TilFile{}, 0.0f, 0.0f, 2.0f),
	            "an empty mission tile array should not block foliage")) {
		return 1;
	}

	std::printf("OK: retail mission TIL foliage overlap vectors match inclusive 16x16 AABBs\n");
	return 0;
}
