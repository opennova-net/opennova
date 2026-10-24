#pragma once

#include <cmath>
#include <cstdint>

#include <base/io/bam.h>
#include <base/io/crt_ftol.h>

namespace opennova::world {

// THE THIRD-PERSON CAMERA'S MOUNTED AND CLEARANCE LEGS.
//
// player_view.h carries the on-foot chase: the quarter-step anchor ease, the
// pivot nudge and the collision-march landing. What lives here is what
// changes when the followed entity is a VEHICLE (mount state +0x168 == 2 or
// 5 with a carrier at +0x16C — our SeatType Controller/Driver, the
// Entity::is_vehicle_control_seat() test), plus the clearances that keep the
// eye out of terrain and water, all in words. player_view_compose_camera's
// mounted fork consumes these; player_view_tick integrates the mounted anchor.
// [orig: ThirdPersonCamera_Update @0x437AF0 (the anchor); Camera_ComputeThirdPersonView
//  @0x437D10 (the eye) — the clearances @0x438409..0x438456, the mounted slope
//  march @0x43846E..0x438619, the aircraft drop @0x43861D..0x43864C]
//
// Camera_RaycastCollisionOffset @0x4378B0 is deliberately NOT here: its only
// callers sit in Camera_ComputeThirdPersonPositions @0x438B80, the mode-4
// death/overview intro. It belongs to the death camera, not the chase.

// ---------------------------------------------------------------------------
// Seeds. Following a new tracked entity starts further back and pitched up
// than entering play does (the play reset is player_view.h's kTpDistanceQ16
// with the orbit zeroed). [orig: Camera_SetTrackedEntity @0x4391D0 — orbit
// yaw 0 @0x439209, orbit pitch 0x04000000 BAM (5.625 deg) @0x439213, distance
// 0x30000 (3.0) @0x43921D; a DEAD target (Flags & 2) starts at 0xA0000 (10.0)
// @0x439273..0x439275 and ThirdPersonCamera_Update reels it back to 3.0
// @0x437CC0..0x437D02]. The orbit state is PlayerViewState's chase fields
// (player_view_track_entity / player_view_chase_tick).
// ---------------------------------------------------------------------------
inline constexpr int32_t kTpTrackedDistanceQ16 = 0x30000;
inline constexpr int32_t kTpTrackedOrbitPitchBam = 0x4000000;
inline constexpr int32_t kTpDeadTargetDistanceQ16 = 0xA0000;

// ---------------------------------------------------------------------------
// Mounted anchor. A vehicle is not framed like a soldier: the anchor lifts and
// the camera backs off, both scaled by the hull's bounding radius, so a
// helicopter and a jeep are both fully in frame. Every value below is a word
// (16.16 or BAM32), as the original keeps them.
// ---------------------------------------------------------------------------

// The anchor lift: 0.375 of the carrier's bound radius (carrier +0), with a
// FLOOR of 1.0 [orig: ThirdPersonCamera_Update @0x437B3B..0x437B46 —
// `(24576 * r + 0x8000) >> 16`, then `< 0x10000 -> 0x10000`]. The floor is
// what stops a small hull from putting the camera inside itself.
inline int32_t mount_anchor_lift_q16(int32_t bound_radius_q16) {
	const int32_t scaled = static_cast<int32_t>(
			(static_cast<int64_t>(24576) * bound_radius_q16 + 0x8000) >> 16);
	return scaled < 0x10000 ? 0x10000 : scaled;
}

// The mounted anchor EASES SLOWER than the on-foot quarter step: a sixteenth
// per tick horizontally and a thirty-second vertically, each with retail's
// half-step rounding [orig: `(target - anchor + 8) >> 4` @0x437C56/@0x437C6A,
// `(target_z - anchor_z + 16) >> 5` @0x437C79; the on-foot `+ 4) >> 2`
// @0x437C8D..0x437CB0 is player_view's].
inline constexpr int kMountAnchorEaseShiftXY = 4;
inline constexpr int kMountAnchorEaseShiftZ = 5;
inline int32_t mount_anchor_ease_q16(int32_t anchor, int32_t target, int shift) {
	return io::bam_add(anchor, io::bam_sar(io::bam_add(io::bam_sub(target, anchor),
	                                                   1 << (shift - 1)),
	                                       shift));
}

// The chase distance: 1.0 less ftol(r * -1.5), so 1.0 + 1.5 r with the half
// unit truncated toward zero [orig: Camera_ComputeThirdPersonView
// @0x438121..0x438136: `fild [edi]`, `fmul dbl_7C56B0` (-1.5), _ftol2_sse,
// `sub ecx, eax` from 0x10000]. Unlike the lift this has no floor: the base
// 1.0 is the floor.
inline constexpr double kMountDistanceRadiusScale = -1.5; // dbl_7C56B0
inline int32_t mount_distance_q16(int32_t bound_radius_q16) {
	return io::bam_sub(0x10000, io::retail_ftol_sse2(static_cast<double>(bound_radius_q16) *
	                                                kMountDistanceRadiusScale));
}

// LOOK YAW IS DAMPED TO A QUARTER, not followed: the camera yaw is the
// vehicle's plus a quarter of the difference to the rider's own Yaw word
// [orig: `(riderYaw - carrierYaw) >> 2 + carrierYaw` @0x438138..0x43814A — an
//  arithmetic shift of the BAM difference]. Following the look yaw outright
// would swing the camera around the hull instead of letting the vehicle lead.
inline int32_t mount_look_yaw_bam(int32_t vehicle_yaw, int32_t look_yaw) {
	return io::bam_add(vehicle_yaw, io::bam_sar(io::bam_sub(look_yaw, vehicle_yaw), 2));
}

// The mounted view pitches DOWN by a fixed amount [orig: `mov esi, 0F8000000h`
// @0x438150 = -0x08000000 BAM, -11.25 degrees]: a vehicle is looked down on,
// not level with.
inline constexpr int32_t kMountPitchBam = static_cast<int32_t>(0xF8000000u);

// An AIRCRAFT drops its eye by half the carrier's radius
// [orig: @0x43861D..0x43864C — `itemDef+0x196 - 3 <= 1` (unit_type 3 or 4)
//  gates `carrierRadius sar 1` off both the eye z and the look-at z].
// unit_type 3/4 are the air classes (3 the helicopter): the game's vehicle
// class reads 3/4 as aircraft (1), 1/2/12 as land (2) and 5..8 as boats (3)
// [orig: Entity_GetVehicleClass @0x4f9e27], and Entity_ClassifyForMinimap
// @0x50FA70 gives 3/4 icon 11 and 5..8 icon 15.
inline bool vehicle_unit_type_is_aircraft(int unit_type) {
	return unit_type == 3 || unit_type == 4;
}
inline int32_t aircraft_eye_drop_q16(int32_t carrier_radius_q16) {
	return io::bam_sar(carrier_radius_q16, 1);
}

// THE LOOK-AHEAD. Mounted (parent && slot 2/5) the look-at target is the
// carrier's chassis matrix times (6.0, 0, 0) — rotation only, so it follows
// the carrier's pitch and roll — eased into the camera's look-ahead offset a
// thirty-second per axis (`g_CameraLookaheadX..Z += (target - lookahead + 16)
// >> 5`) and added onto the look-at deltas; the final yaw comes from the
// look-at (fpatan) [orig: @0x438811..0x4388b5, the 0x60000 store @0x438835].
// The tracked-entity (on-foot) leg uses 6 units along the entity's own yaw
// floored at terrain + 1.0 instead [orig: @0x438767..0x4387C9,
// Terrain_SampleHeightBilinear + 0x10000 against entity z]; that leg rides
// the deferred orbit/tracked state.
inline constexpr int32_t kMountLookaheadQ16 = 0x60000;
inline constexpr float kMountLookaheadTerrainLift = 1.0f;

// ---------------------------------------------------------------------------
// Clearances. Applied after the march, these are what keep the eye out of the
// ground and the water rather than clipping through them.
// ---------------------------------------------------------------------------

// Both clearances are a quarter unit [orig: +0x4000 @0x43840E for water,
// @0x438447 for terrain]. They are separate constants in the original even
// though they share a value, and they are kept separate here so a future
// divergence in one does not silently move the other.
inline constexpr int32_t kWaterClearanceQ16 = 0x4000;
inline constexpr int32_t kTerrainClearanceQ16 = 0x4000;

// The water floor applies ONLY while the tracked entity's Position Z word is
// above water + clearance, and only lifts an eye below that line
// [orig: `cmp [esi+0Ch], water + 0x4000; jle skip` @0x438413..0x438416, `cmp
// eye.z, water + 0x4000; jge skip` @0x438418..0x43841E, both signed]: a
// swimming or submerged entity keeps an underwater eye.
inline int32_t raise_above_water_q16(int32_t eye_z, int32_t water_z, int32_t entity_z) {
	const int32_t floor_z = io::bam_add(water_z, kWaterClearanceQ16);
	return entity_z > floor_z && eye_z < floor_z ? floor_z : eye_z;
}

// The terrain floor: the bilinear sample at the eye's words plus 0x4000, or
// for an entity carrying the INDOORS flag (entity.h kEntityFlagIndoors =
// 0x800000: the heightmap has no interiors) the height-0 plane, no sample
// taken [orig: `test [esi+24h], 800000h` @0x438422..0x43842D, the sample
// @0x438439 + 0x4000 @0x438447, the floor @0x43844C..0x438456].
inline int32_t raise_above_terrain_q16(int32_t eye_z, int32_t terrain_z, bool indoors) {
	const int32_t floor_z = indoors ? 0 : io::bam_add(terrain_z, kTerrainClearanceQ16);
	return eye_z < floor_z ? floor_z : eye_z;
}

// THE MOUNTED SLOPE RAISE [orig: Camera_ComputeThirdPersonView @0x43846E..0x438619,
// mount states 2/5 only @0x43845A..0x438468]. From the eased anchor toward
// the eye, in words through the x87:
// - the horizontal length L = sqrt(dx^2 + dy^2) of the word deltas, kept
//   as ftol(L) for the margin [orig: @0x43848F..0x4384BE];
// - the unit u = ftol(d * (flt_7C32BC / L)) per axis (65536.0, so 16.16),
//   zero when L is zero [orig: the fucomp-zero gate @0x4384C2..0x4384C9, the
//   division and ftols @0x4384CB..0x4384F1, the zero leg @0x4384F8..0x438504];
// - the step: half of each, `(u * 0x8000 + 0x8000) >> 16` [orig:
//   @0x438508..0x438545];
// - the march length: L again, saturated at flt_7C19E0 (2147418112.0) and
//   ftol'd [orig: @0x438561..0x438594];
// - the march: while the run is below the length, step x / y, add 0x8000 to
//   the run, sample the bilinear terrain at the stepped words and keep the
//   steepest (h - anchor z) / run, the running max stored as a float32 and
//   seeded at 0.0, the x87 top holding either that float or the last
//   steeper slope at full width [orig: @0x4385A1..0x4385F6; the seed `fst
//   [var_70]` @0x438599 is a float store, the run's 0x8000 @0x4385BA, the
//   division and the max @0x4385D0..0x4385EA; the last sample can lie up to
//   a half unit past the eye];
// - the floor: anchor z + ftol(max * length + ftol(L) * flt_7C56A4), the
//   literal 0.333f, NOT one third [orig: @0x4385F8..0x438611]. The caller
//   lifts the eye to it [orig: @0x438613..0x438619].
// `sample_height(x, y)` is the bilinear terrain word at mission words.
inline constexpr int32_t kSlopeStepQ16 = 0x8000;
inline constexpr double kSlopeUnitScale = 65536.0;           // flt_7C32BC
inline constexpr double kSlopeMarchSaturation = 2147418112.0; // flt_7C19E0
inline constexpr float kSlopeRaiseMargin = 0.333f;           // flt_7C56A4
template <class SampleHeight>
int32_t mount_slope_raise_floor_q16(const int32_t anchor_q16[3], int32_t eye_x, int32_t eye_y,
                                    SampleHeight sample_height) {
	const int32_t dx = io::bam_sub(eye_x, anchor_q16[0]);
	const int32_t dy = io::bam_sub(eye_y, anchor_q16[1]);
	const double fdx = static_cast<double>(dx);
	const double fdy = static_cast<double>(dy);
	const double length = std::sqrt(fdx * fdx + fdy * fdy);
	const int32_t length_word = io::retail_ftol_sse2(length);
	int32_t unit_x = 0;
	int32_t unit_y = 0;
	if (length != 0.0) {
		const double scale = kSlopeUnitScale / length;
		unit_x = io::retail_ftol_sse2(fdx * scale);
		unit_y = io::retail_ftol_sse2(fdy * scale);
	}
	const int32_t step_x = static_cast<int32_t>(
			(static_cast<int64_t>(unit_x) * kSlopeStepQ16 + 0x8000) >> 16);
	const int32_t step_y = static_cast<int32_t>(
			(static_cast<int64_t>(unit_y) * kSlopeStepQ16 + 0x8000) >> 16);
	double march_length = std::sqrt(fdy * fdy + fdx * fdx);
	if (march_length > kSlopeMarchSaturation) march_length = kSlopeMarchSaturation;
	const int32_t march = io::retail_ftol_sse2(march_length);
	float max_stored = 0.0f;
	double max_slope = 0.0;
	int32_t x = anchor_q16[0];
	int32_t y = anchor_q16[1];
	for (int32_t run = 0; run < march;) {
		y = io::bam_add(y, step_y);
		x = io::bam_add(x, step_x);
		run += kSlopeStepQ16;
		const int32_t rise = io::bam_sub(sample_height(x, y), anchor_q16[2]);
		const double slope = static_cast<double>(rise) / static_cast<double>(run);
		if (static_cast<double>(max_stored) >= slope) {
			max_slope = static_cast<double>(max_stored);
		} else {
			max_slope = slope;
			max_stored = static_cast<float>(slope);
		}
	}
	const double raise = max_slope * static_cast<double>(march) +
	                     static_cast<double>(length_word) * static_cast<double>(kSlopeRaiseMargin);
	return io::bam_add(anchor_q16[2], io::retail_ftol_sse2(raise));
}

} // namespace opennova::world
