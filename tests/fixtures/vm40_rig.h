// The 40-bone first-person rig the synthetic viewmodel (minimal_3di_gen.cpp,
// fixtures/threedi/synth/vm40.3di) and its clip set (minimal_bad_gen.cpp,
// fixtures/anim/vm40*) share, in the stock first-person rigs' size (mp5_1st's
// and 357_1st's 40 bones): 0 the root; per arm (right from 1, left from 19)
// the upper arm, the forearm, the hand and five fingers of three joints; 37 the
// receiver, 38 the magazine and 39 the bolt under it. Ours, each bone named by
// its BN## tag.
//
// Frames: the pivots are absolute, in MISSION axes (x forward, y left, z up),
// the eye at the origin.
#pragma once

#include <cstdio>
#include <string>
#include <vector>

namespace vm40_rig {

constexpr int kBones = 40;
constexpr int kReceiver = 37, kMagazine = 38, kBolt = 39;

struct Bone {
	std::string name;
	int parent = -1; // -1 for the root
	double pivot[3] = {0.0, 0.0, 0.0};
	double length = 0.0;
};

inline std::vector<Bone> bones() {
	std::vector<Bone> out(kBones);
	char name[32];
	out[0].name = "BN01 Root";
	for (int side = 0; side < 2; ++side) {
		const int base = 1 + side * 18;
		const double y = side == 0 ? -1.0 : 1.0;
		const char *letter = side == 0 ? "R" : "L";
		const double reach = side == 0 ? 0.0 : 0.125;
		const double joints[3][3] = {{0.0, 0.25 * y, -0.25}, {0.25 + reach, 0.1875 * y, -0.3125},
			{0.5 + reach, 0.125 * y, -0.25}};
		const char *roles[3] = {"UpperArm", "Forearm", "Hand"};
		for (int j = 0; j < 3; ++j) {
			Bone &b = out[static_cast<size_t>(base + j)];
			std::snprintf(name, sizeof(name), "BN%02d %s %s", base + j + 1, letter, roles[j]);
			b.name = name;
			b.parent = j == 0 ? 0 : base + j - 1;
			for (int k = 0; k < 3; ++k) b.pivot[k] = joints[j][k];
			b.length = 0.25;
		}
		const double *hand = joints[2];
		for (int f = 0; f < 5; ++f) {
			for (int k = 0; k < 3; ++k) {
				const int index = base + 3 + f * 3 + k;
				Bone &b = out[static_cast<size_t>(index)];
				std::snprintf(name, sizeof(name), "BN%02d %s Finger%d%d", index + 1, letter, f + 1, k + 1);
				b.name = name;
				b.parent = k == 0 ? base + 2 : index - 1;
				b.pivot[0] = hand[0] + 0.0625 + 0.03125 * k;
				b.pivot[1] = hand[1] + 0.015625 * (f - 2) * y;
				b.pivot[2] = hand[2];
				b.length = 0.03125;
			}
		}
	}
	const double gun[3][3] = {{0.375, -0.125, -0.1875}, {0.4375, -0.125, -0.25}, {0.3125, -0.125, -0.15625}};
	const char *gun_names[3] = {"BN38 Receiver", "BN39 Magazine", "BN40 Bolt"};
	for (int g = 0; g < 3; ++g) {
		Bone &b = out[static_cast<size_t>(kReceiver + g)];
		b.name = gun_names[g];
		b.parent = g == 0 ? 0 : kReceiver;
		for (int k = 0; k < 3; ++k) b.pivot[k] = gun[g][k];
		b.length = 0.125;
	}
	return out;
}

} // namespace vm40_rig
