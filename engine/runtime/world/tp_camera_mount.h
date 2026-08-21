#pragma once

#include <cstdint>

namespace opennova::world {

// THE THIRD-PERSON CAMERA'S MOUNTED AND CLEARANCE LEGS.
//
// player_view.h already carries the on-foot chase: the quarter-step anchor
// ease, the pivot nudge and the collision-march landing. What lives here is
// what changes when the followed entity is a VEHICLE (mount state +0x168 == 2
// or 5 with a carrier at +0x16C), plus the clearances that keep the eye out
// of terrain and water.
// [orig: ThirdPersonCamera_Update @0x437AF0 (the anchor); Camera_ComputeThirdPersonView
//  @0x437D10 (the eye) — the clearances @0x438409..0x438456, the mounted slope
//  march @0x43846E..0x438619, the aircraft drop @0x43861D..0x43864C]
//
// STAGED, NOT WIRED: player_view_compose_camera's mode 1 is the on-foot leg;
// the mounted branch is the follow-up that consumes these constants.
//
// Camera_RaycastCollisionOffset @0x4378B0 is deliberately NOT here: its only
// callers sit in Camera_ComputeThirdPersonPositions @0x438B80, the mode-4
// death/overview intro. It belongs to the death camera, not the chase.

// ---------------------------------------------------------------------------
// Seeds. The camera starts from DIFFERENT values depending on how it was
// entered, which is why these are pairs rather than one.
// ---------------------------------------------------------------------------

// Following a new tracked entity [orig: Camera_SetTrackedEntity @0x4391D0 —
// orbit yaw 0 @0x439209, orbit pitch 0x04000000 BAM (5.625 deg) @0x439213,
// distance 0x30000 (3.0) @0x43921D; a DEAD target (Flags & 2) starts at
// 0xA0000 (10.0) @0x439273..0x439275 and ThirdPersonCamera_Update reels it
// back to 3.0 @0x437CC0..0x437D02].
inline constexpr float kTpTrackedDistance = 3.0f;
inline constexpr float kTpTrackedOrbitPitchDeg = 5.625f;
inline constexpr float kTpDeadTargetDistance = 10.0f;

// Entering play as the local player [orig: Camera_ResetToLocalPlayer
// @0x4A3D30 — distance 0x10000 @0x4A3D4C, orbit yaw/pitch 0 @0x4A3D56..0x4A3D5B]
// — the tight over-the-shoulder view, orbit zeroed.
inline constexpr float kTpPlayDistance = 1.0f;
inline constexpr float kTpPlayOrbitPitchDeg = 0.0f;

// ---------------------------------------------------------------------------
// Mounted anchor. A vehicle is not framed like a soldier: the anchor lifts and
// the camera backs off, both scaled by the hull's bounding radius, so a
// helicopter and a jeep are both fully in frame.
// ---------------------------------------------------------------------------

// The anchor lift: 0.375 of the carrier's bound radius (carrier +0), with a
// FLOOR of 1.0 [orig: ThirdPersonCamera_Update @0x437B3B..0x437B46 —
// `(24576 * r + 0x8000) >> 16`, then `< 0x10000 -> 0x10000`]. The floor is
// what stops a small hull from putting the camera inside itself.
inline constexpr float kMountAnchorMinLift = 1.0f;
inline constexpr float kMountAnchorRadiusScale = 0.375f; // 24576 / 65536
inline float mount_anchor_lift(float bound_radius) {
	const float scaled = kMountAnchorRadiusScale * bound_radius;
	return scaled > kMountAnchorMinLift ? scaled : kMountAnchorMinLift;
}

// The mounted anchor EASES SLOWER than the on-foot quarter step: a sixteenth
// per tick horizontally and a thirty-second vertically, each with retail's
// half-step rounding [orig: `(target - anchor + 8) >> 4` @0x437C56/@0x437C6A,
// `(target_z - anchor_z + 16) >> 5` @0x437C79; the on-foot `+ 4) >> 2`
// @0x437C8D..0x437CB0 is player_view's].
inline constexpr int kMountAnchorEaseShiftXY = 4;
inline constexpr int kMountAnchorEaseShiftZ = 5;
inline int32_t mount_anchor_ease_q16(int32_t anchor, int32_t target, int shift) {
	return anchor + ((target - anchor + (1 << (shift - 1))) >> shift);
}

// The chase distance: 1.0 + 1.5 * bound radius
// [orig: Camera_ComputeThirdPersonView @0x438121..0x438136 — ftol(r *
//  dbl_7C56B0 (-1.5)) subtracted from 0x10000]. Unlike the lift this has no
// floor — the base 1.0 is the floor.
inline constexpr float kMountDistanceBase = 1.0f;
inline constexpr float kMountDistanceRadiusScale = 1.5f;
inline float mount_distance(float bound_radius) {
	return kMountDistanceBase + kMountDistanceRadiusScale * bound_radius;
}

// LOOK YAW IS DAMPED TO A QUARTER, not followed: the camera yaw is the
// vehicle's plus a quarter of the difference to where the rider is looking
// [orig: `(riderYaw - carrierYaw) >> 2 + carrierYaw` @0x438138..0x43814A — an
//  arithmetic shift of the BAM difference]. Following the look yaw outright
// would swing the camera around the hull instead of letting the vehicle lead.
inline constexpr float kMountLookYawScale = 0.25f;
inline float mount_look_yaw(float vehicle_yaw, float look_yaw) {
	return vehicle_yaw + (look_yaw - vehicle_yaw) * kMountLookYawScale;
}
inline int32_t mount_look_yaw_bam(int32_t vehicle_yaw, int32_t look_yaw) {
	return vehicle_yaw + ((look_yaw - vehicle_yaw) >> 2);
}

// The mounted view pitches DOWN by a fixed amount [orig: `mov esi, 0F8000000h`
// @0x438150 = -0x08000000 BAM] — a vehicle is looked down on, not level with.
inline constexpr float kMountPitchDeg = -11.25f;

// An AIRCRAFT drops its eye by half the carrier's radius
// [orig: @0x43861D..0x43864C — `itemDef+0x196 - 3 <= 1` (class byte 3 or 4)
//  gates `carrierRadius >> 1` off both the eye z and the look-at z]. Which of
// 3/4 is helicopter vs plane is not witnessed here.
inline constexpr float kAircraftEyeDropScale = 0.5f;
inline bool vehicle_class_byte_is_aircraft(uint8_t class_byte) {
	return class_byte == 3 || class_byte == 4;
}
inline float aircraft_eye_drop(float carrier_radius) {
	return carrier_radius * kAircraftEyeDropScale;
}

// The mounted look-at point sits 6 units along the TRACKED ENTITY's yaw, on
// the terrain + 1.0 or the entity's own z, whichever is higher
// [orig: @0x438767..0x4387C9 — 6.0 * cos / sin from the BAM tables added to
//  entity x/y, Terrain_SampleHeightBilinear + 0x10000 floored against
//  entity z; a second 6.0 seeds the carrier-relative point @0x438835].
inline constexpr float kMountLookaheadDistance = 6.0f;
inline constexpr float kMountLookaheadTerrainLift = 1.0f;

// ---------------------------------------------------------------------------
// Clearances. Applied after the march, these are what keep the eye out of the
// ground and the water rather than clipping through them.
// ---------------------------------------------------------------------------

// Both clearances are a quarter unit [orig: +0x4000 @0x43840E for water,
// @0x438447 for terrain]. They are separate constants in the original even
// though they share a value, and they are kept separate here so a future
// divergence in one does not silently move the other.
inline constexpr float kWaterClearance = 0.25f;
inline constexpr float kTerrainClearance = 0.25f;

// Raise the eye to clear a surface, if it is below it.
inline float raise_above(float eye_z, float surface_z, float clearance) {
	const float floor_z = surface_z + clearance;
	return eye_z < floor_z ? floor_z : eye_z;
}

// The water floor applies ONLY while the tracked entity itself is above
// water + clearance [orig: `cmp [esi+0Ch], water + 0x4000; jle skip`
// @0x438413..0x438416] — a swimming or submerged entity keeps an
// underwater eye.
inline float raise_above_water(float eye_z, float water_z, float entity_z) {
	if (entity_z <= water_z + kWaterClearance) return eye_z;
	return raise_above(eye_z, water_z, kWaterClearance);
}

// The terrain floor is skipped for an entity carrying Flags bit 0x800000
// [orig: `test [esi+24h], 800000h; jz sample` @0x438422..0x43842D — the
//  floor is then 0 (the height-0 plane) rather than terrain + 0.25]. The
// flag's producer is not witnessed here.
inline constexpr uint32_t kEntityFlagCameraSkipsTerrainFloor = 0x800000u;
inline float raise_above_terrain(float eye_z, float terrain_z, uint32_t entity_flags) {
	if ((entity_flags & kEntityFlagCameraSkipsTerrainFloor) != 0u)
		return eye_z < 0.0f ? 0.0f : eye_z;
	return raise_above(eye_z, terrain_z, kTerrainClearance);
}

// THE MOUNTED SLOPE RAISE [orig: @0x43846E..0x438619, mount states 2/5 only
// @0x43845A..0x438468]. Retail marches from the ANCHOR toward the eye in
// HALF-unit steps [orig: 0x8000 @0x438508, accumulated @0x4385BA], samples
// the terrain at each step [orig: Terrain_SampleHeightBilinear @0x4385C5] and
// keeps the STEEPEST rise-over-run seen [orig: `(h - anchor_z) / run` with the
// running max @0x4385D0..0x4385EA]. The eye is then floored at the anchor
// height plus that slope times the horizontal distance, PLUS 0.333 per unit
// of distance [orig: `maxSlope * dist + dist * flt_7C56A4` @0x4385F8..0x438619].
// The literal is 0.333 — NOT one third: 1.0f / 3.0f is 0.33333433 and moves
// the floor on every mounted frame.
inline constexpr float kSlopeStep = 0.5f;
inline constexpr float kSlopeRaiseMargin = 0.333f;
inline float slope_raise_floor(float anchor_z, float horizontal_distance, float max_slope) {
	return anchor_z + max_slope * horizontal_distance +
			horizontal_distance * kSlopeRaiseMargin;
}

} // namespace opennova::world
