#pragma once

namespace opennova::world {

// THE THIRD-PERSON CAMERA'S MOUNTED AND CLEARANCE LEGS.
//
// player_view.h already carries the on-foot chase: the quarter-step anchor
// ease, the pivot nudge and the collision-march landing. What lives here is
// what changes when the followed entity is a VEHICLE, plus the clearances that
// keep the eye out of terrain and water.
// [orig: ThirdPersonCamera_Update @0x437AF0; Camera_ComputeThirdPersonView
//  @0x437D10; the clamps and slope raise @0x438409..0x438619]
//
// Camera_RaycastCollisionOffset @0x4378B0 is deliberately NOT here: its only
// callers sit in Camera_ComputeThirdPersonPositions @0x438B80, the mode-4
// death/overview intro. It belongs to the death camera, not the chase.

// ---------------------------------------------------------------------------
// Seeds. The camera starts from DIFFERENT values depending on how it was
// entered, which is why these are two pairs rather than one.
// ---------------------------------------------------------------------------

// Following a tracked entity [orig: Camera_SetTrackedEntity @0x4391D0 — the
// 0x04000000 BAM pitch seed].
inline constexpr float kTpTrackedDistance = 3.0f;
inline constexpr float kTpTrackedOrbitPitchDeg = 5.625f;

// Entering play as the local player [orig: Camera_ResetToLocalPlayer
// @0x4A3D30] — the tight over-the-shoulder view, orbit zeroed.
inline constexpr float kTpPlayDistance = 1.0f;
inline constexpr float kTpPlayOrbitPitchDeg = 0.0f;

// ---------------------------------------------------------------------------
// Mounted. A vehicle is not framed like a soldier: the anchor lifts and the
// camera backs off, both scaled by the hull's bounding radius, so a helicopter
// and a jeep are both fully in frame.
// ---------------------------------------------------------------------------

// The anchor lift: 0.375 of the bound radius, with a FLOOR of 1.0
// [orig: max(1.0, 0.375 * boundRadius) @0x437B1F..0x437B4B]. The floor is what
// stops a small hull from putting the camera inside itself.
inline constexpr float kMountAnchorMinLift = 1.0f;
inline constexpr float kMountAnchorRadiusScale = 0.375f;
inline float mount_anchor_lift(float bound_radius) {
	const float scaled = kMountAnchorRadiusScale * bound_radius;
	return scaled > kMountAnchorMinLift ? scaled : kMountAnchorMinLift;
}

// The chase distance: 1.0 + 1.5 * bound radius
// [orig: @0x438121..0x438136; the scale is dbl_7C56B0 = -1.5, negated at the
//  use site]. Unlike the lift this has no floor — the base 1.0 is the floor.
inline constexpr float kMountDistanceBase = 1.0f;
inline constexpr float kMountDistanceRadiusScale = 1.5f;
inline float mount_distance(float bound_radius) {
	return kMountDistanceBase + kMountDistanceRadiusScale * bound_radius;
}

// LOOK YAW IS DAMPED TO A QUARTER, not followed: the camera yaw is the
// vehicle's plus a quarter of the difference to where the rider is looking
// [orig: vehYaw + (lookYaw - vehYaw) / 4 @0x4380A3]. Following the look yaw
// outright would swing the camera around the hull instead of letting the
// vehicle lead.
inline constexpr float kMountLookYawScale = 0.25f;
inline float mount_look_yaw(float vehicle_yaw, float look_yaw) {
	return vehicle_yaw + (look_yaw - vehicle_yaw) * kMountLookYawScale;
}

// The mounted view pitches DOWN by a fixed amount [orig: -0x08000000 BAM
// @0x438150] — a vehicle is looked down on, not level with.
inline constexpr float kMountPitchDeg = -11.25f;

// An AIRCRAFT drops its eye by half the carrier's radius
// [orig: carrier radius >> 1 @0x438632..0x43864A].
inline constexpr float kAircraftEyeDropScale = 0.5f;
inline float aircraft_eye_drop(float carrier_radius) {
	return carrier_radius * kAircraftEyeDropScale;
}

// The mounted look-ahead point sits 6 units along the parent's orientation
// [orig: R(parent) * (6, 0, 0) @0x4387F7].
inline constexpr float kMountLookaheadDistance = 6.0f;

// ---------------------------------------------------------------------------
// Clearances. Applied after the march, these are what keep the eye out of the
// ground and the water rather than clipping through them.
// ---------------------------------------------------------------------------

// Both clearances are a quarter unit [orig: +0x4000 @0x438409..0x438420 for
// water, @0x43842F..0x438447 for terrain]. They are separate constants in the
// original even though they share a value, and they are kept separate here so
// a future divergence in one does not silently move the other.
inline constexpr float kWaterClearance = 0.25f;
inline constexpr float kTerrainClearance = 0.25f;

// Raise the eye to clear a surface, if it is below it.
inline float raise_above(float eye_z, float surface_z, float clearance) {
	const float floor_z = surface_z + clearance;
	return eye_z < floor_z ? floor_z : eye_z;
}

// The slope raise marches in HALF-unit steps [orig: 0x8000 @0x438508] and
// stops once a step rises less than the minimum
// [orig: flt_7C56A4 @0x4385F8]. That literal is 0.333 — NOT one third. Writing
// 1.0f/3.0f gives 0.33333433 and changes where the march terminates on a
// shallow slope.
inline constexpr float kSlopeStep = 0.5f;
inline constexpr float kSlopeMinRise = 0.333f;

} // namespace opennova::world
