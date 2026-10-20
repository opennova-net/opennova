// The JO person rig the synthetic person model (minimal_3di_gen.cpp,
// fixtures/threedi/synth/person.3di) and its clip set (minimal_bad_gen.cpp,
// fixtures/anim/rig19*.bad) share: nineteen bones in the retail person rig
// order, each named by its BN## tag (the runtime pairs a clip bone with a model
// part by that index: anim::model_bone_index_from_name). The order, the
// parents and the names are the stock 19-bone body rig's (BINOC.bad, US01's
// DT1*.BAD: bone 14 the head under the neck, 15/16 the hands); the pivots are
// ours, a soldier about 1.8 units tall with its origin at the pelvis.
//
// Frames (docs/threedi/3di-gp-format-re.md): the pivots are absolute, in
// MISSION axes (x forward, y left, z up).
#pragma once

namespace person_rig {

constexpr int kBones = 19;

// Each part's parent; part 0, the root, names itself (the model's form; a
// clip's root parent is -1).
constexpr int kParents[kBones] = {0, 0, 1, 2, 2, 3, 4, 0, 0, 5, 6, 7, 8, 2, 13, 10, 9, 11, 12};

constexpr double kPivots[kBones][3] = {{0, 0, 0}, {0, 0, 0.125}, {0, 0, 0.375}, {0, -0.0625, 0.625},
	{0, 0.0625, 0.625}, {0, -0.25, 0.625}, {0, 0.25, 0.625}, {0, -0.125, 0}, {0, 0.125, 0},
	{0, -0.4375, 0.4375}, {0, 0.4375, 0.4375}, {0, -0.125, -0.4375}, {0, 0.125, -0.4375},
	{0, 0, 0.6875}, {0.03125, 0, 0.75}, {0, 0.6875, 0.3125}, {0, -0.6875, 0.3125},
	{-0.0625, -0.125, -0.875}, {-0.0625, 0.125, -0.875}};

constexpr const char *kNames[kBones] = {"BN01 Hips", "BN02 Lower Spine", "BN03 Upper Spine", "BN04 R Clavicle",
	"BN05 L Clavicle", "BN06 R UpperArm", "BN07 L UpperArm", "BN08 R Thigh", "BN09 L Thigh", "BN10 R Forearm",
	"BN11 L Forearm", "BN12 R Calf", "BN13 L Calf", "BN14 Neck", "BN15 Head", "BN16 L Hand", "BN17 R Hand",
	"BN18 R Foot", "BN19 L Foot"};

} // namespace person_rig
