// The person render callback's item overlays: beside the body itself and the
// held weapon (entity_pose.h), retail draws up to four more models inside the
// same callback — the parachute canopy (draw 1), the night-vision goggles
// (draw 3), the binoculars (draw 4) and the carried object (draw 6). Each is
// drawn RIGID: one matrix is stamped into every bone slot of the item's own
// model, so an overlay's whole appearance is its gate plus the placement
// calibration below. This header owns both; presentation builds the models
// and poses them from the body's skeleton with these constants.
// [orig: BoneCallback_org0_World @ 0x4e3940 (draws 1/3/4/6); the frames are
//  built by Entity_BuildBoneTransformMatrices @ 0x4b1290]
#pragma once

#include <runtime/anim/aim_overlay.h>
#include <runtime/world/parachute.h>

#include <cstdint>

namespace opennova::world {

// The three special items the overlays draw, resolved once per mission by
// runtime type id. [orig: Entity_PreloadSpecialItems @ 0x43c220 — 185 ->
//  dword_A892A0 @ 0x43c22b, 1904 -> g_NightVissionGoggleItemIndex
//  @ 0x43c240, 1424 -> g_BinocItemIndex @ 0x43c255]
inline constexpr int32_t kParachuteItemTypeId = 185;
inline constexpr int32_t kNightVisionGogglesItemTypeId = 1904;
inline constexpr int32_t kBinocularsItemTypeId = 1424;

// Draw 1, the canopy: the matrix is a turn about the up axis alone, placed at
// the TRANSLATION of model bone 0's posed matrix (where the model origin
// lands, not the hip joint). The turn is fed (-cos, +sin) of the BODY
// heading, which is the builder's form for heading + pi.
// [orig: BoneCallback_org0_World @ 0x4e39d6..0x4e3a0a (the axis-2 rotation),
//  @ 0x4e3a0f..0x4e3a48 (bone 0's translation row copied in)]
inline constexpr int kCanopyBoneIndex = 0;

// Draw 3, the goggles: model bone 14 (BN15 Head) posed matrix, its pivot
// nudged by (0, +0.15, +0.10) before the bone carries it. The nudge's X term
// is zero, so the loader's X negation leaves it unchanged in the Godot frame.
// [orig: Entity_BuildBoneTransformMatrices @ 0x4b24e3..0x4b258e — bone slot
//  +0x380, pivot row +0x3A4, flt_7C6FA4 = 0.15 @ 0x4b2511, flt_7C69F4 = 0.10
//  @ 0x4b2528]
inline constexpr int kNightVisionBoneIndex = 14;
inline constexpr float kNightVisionNudgeX = 0.0f;
inline constexpr float kNightVisionNudgeY = 0.15f;
inline constexpr float kNightVisionNudgeZ = 0.10f;

// Draw 4, the binoculars: model bone 15 (BN16 L Hand), its pivot nudged by
// (-0.12, -0.03, +0.03) in the x-negated render frame (so +0.12 in Godot's),
// then turned by a fixed calibration composed row-major on the LEFT of the
// bone matrix: Ry(dbl_7C9B78) * Rx(dbl_7C9B80) * Rz(dbl_7C9B88) * M15, each
// block built with the sine negated. In Godot's column form that is the bone
// basis times Rz(+z) * Rx(-x) * Ry(+y): a Z or Y block keeps its authored
// sign (the negated sine and the loader's X negation cancel, as in the held
// weapon's hand frame), an X block is left unchanged by the X negation, so its
// negated sine survives.
// [orig: Entity_BuildBoneTransformMatrices @ 0x4b2308..0x4b24d2 — bone slot
//  +0x3C0, pivot row +0x3E4, flt_7C9B94 = 0.12 @ 0x4b232e, flt_7C9B90 = 0.03
//  @ 0x4b233a; Rz @ 0x4b2394..0x4b23d0, Rx @ 0x4b23d5..0x4b242b, Ry
//  @ 0x4b2430..0x4b2486 via Math_BuildRotationMatrix4x4_ByAxis @ 0x611db0]
inline constexpr int kBinocularsBoneIndex = 15;
inline constexpr float kBinocularsNudgeX = 0.12f;
inline constexpr float kBinocularsNudgeY = -0.03f;
inline constexpr float kBinocularsNudgeZ = 0.03f;
inline constexpr double kBinocularsFrameZRad = -2.356266256975834;
inline constexpr double kBinocularsFrameXRad = -0.2618073618862038;
inline constexpr double kBinocularsFrameYRad = 1.7453824125746917;

// Draw 6, the carried object: the CARRIER's own entity matrix (its position
// and the entity yaw/pitch/roll, restored after the bone build) times a fixed
// Z rotation of flt_7CD438 = 205887.421875 radians (pi * 65536 stored as a
// float; about 0.328 degrees once reduced), then translated to the
// binocular frame's point (the left hand).
// [orig: BoneCallback_org0_World @ 0x4e3dbd..0x4e3e26 —
//  Math_BuildFixedPointToFloatMatrix4x4 @ 0x4e3dc6,
//  Math_BuildRotationMatrixZ_Float @ 0x4e3ddf, Math_MultiplyMatrix4x4_Float
//  @ 0x4e3df1, the binocular translation copied @ 0x4e3df6..0x4e3e26;
//  the entity triple restored @ 0x4b1eba..0x4b1ec0]
inline constexpr float kCarriedFrameZRad = 205887.421875f;

// One body's overlay state, presentation-ready.
struct PersonOverlays {
	// Draw 1: the canopy CTRL registers PARA / PARA_O (ordinals 14/15) are the
	// two canopy words doubled; both zero means no canopy.
	// [orig: gate @ 0x4e3988..0x4e399a; stores @ 0x4e3a16..0x4e3a56]
	int32_t canopy_para = 0;
	int32_t canopy_para_o = 0;
	// The canopy's up-axis turn as a mission yaw (degrees).
	float canopy_yaw_deg = 0.0f;
	// Draw 3 and its NVG_FLIP register (ordinal 7): 0xFFFF while dead.
	// [orig: gate @ 0x4e3b4f..0x4e3b54; flip @ 0x4e3b90..0x4e3b9d]
	bool nvg = false;
	int32_t nvg_flip = 0;
	// Draw 4. [orig: gate @ 0x4e3bec..0x4e3c04]
	bool binoculars = false;
	// Draw 6: the carried object's runtime type (0 = none) and the carrier's
	// entity triple as mission euler degrees.
	// [orig: gate @ 0x4e3d9e..0x4e3db7]
	int32_t carried_type_id = 0;
	float carried_pitch_deg = 0.0f;
	float carried_yaw_deg = 0.0f;
	float carried_roll_deg = 0.0f;

	bool canopy() const { return canopy_para != 0 || canopy_para_o != 0; }
};

struct PersonOverlayInputs {
	// The entity Flags dword (entity+0x24).
	uint32_t flags = 0;
	// The body pose: aim_state is the anim-state flag 0x40 test the
	// binocular gate shares with the bone build, body_yaw the body heading
	// (entity+0x8C), aim_yaw / aim_pitch / roll the entity triple
	// (entity+0x10 / +0x14 / +0x18).
	anim::AimOverlayInputs pose;
	// The canopy words (entity+0x378 / +0x37A).
	ParachuteState chute;
	// The mounted child's runtime type (entity+0x268 with an item def), 0 when
	// nothing is carried.
	int32_t carried_type_id = 0;
};

PersonOverlays person_overlays(const PersonOverlayInputs &in);

// The PF_* row tail carrying one body's overlays (runtime/world/present_rows.h).
void write_present_person_overlays(float *row, const PersonOverlays &overlays);
PersonOverlays read_present_person_overlays(const float *row);

} // namespace opennova::world
