#pragma once

// Camera mode 4 — the death/overview lerp camera (net-re 0x52, §5.39). When
// the arbiter enters mode 4, Camera_SetTrackedEntity computes a FROM pose (a
// point 5 u from the player along the best clear trial direction, looking
// back at the player) and a TO pose (5 u behind the player along the
// player->anchor line, looking toward the anchor), and every rendered frame
// then lerps the view between them over 128 ticks from the death stamp.
// The anchor is the S2C 0x52 triple on a joiner (dword_A860E0/E4/E8) or, on
// the authority, the local entity's +0x178 killer entity position (else self).
// [orig: Camera_ComputeThirdPersonPositions @0x438b80;
//  Camera_RaycastCollisionOffset @0x4378b0; the lerp
//  Camera_ComputeThirdPersonView @0x4389eb..0x438b49; the start-tick writers
//  NapiNPClientMsg_EntityDeath @0x42ec0f / Entity_UpdateInfantryPlayerBody
//  @0x4b4d00; Camera_SetTrackedEntity @0x4391d0 mode-4 compute @0x439257]
// Everything is 16.16 fixed / BAM32 like the original; the presenting shell
// converts through world/angle.h.

#include <cstdint>
#include <functional>

namespace opennova::world {

struct DeathCameraPose {
    int32_t pos[3] = {0, 0, 0}; // 16.16 mission units
    int32_t yaw_bam = 0;        // BAM32, the fpatan(y, x) convention of the original
    int32_t pitch_bam = 0;
    int32_t roll_bam = 0;
};

struct DeathCameraState {
    bool valid = false;
    DeathCameraPose from;
    DeathCameraPose to;
    uint32_t start_tick = 0; // g_CameraLerpStartTick @0xB76470
};

// The clear-distance probe along a unit 16.16 direction from `origin`
// [orig: Camera_RaycastCollisionOffset(origin, dir, 0x8000, 0x50000)]:
// returns the reachable 16.16 distance (<= max_dist). The original walks
// 0.25 u steps and, for EVERY collision bone of the tracked entity, tests the
// bone force (Entity_ComputeCollisionForceFromBones @0x4afff0) and the
// terrain (Terrain_SampleHeightBilinear + lift > z) — with NO bones it
// returns max_dist untouched (@0x4378c4). The bone leg is the tracked
// residue; death_camera_probe_terrain below is the terrain leg for an
// embedder that knows the bone count.
using CameraRayProbe = std::function<int32_t(
        const int32_t origin[3], const int32_t dir_q16[3], int32_t lift, int32_t max_dist)>;

// The terrain height sampler (16.16 mission x, y -> 16.16 z).
using CameraTerrainSampler = std::function<int32_t(int32_t x, int32_t y)>;

// The step walk of the probe with the terrain leg only, run `bone_count`
// times per step like the original's per-bone loop [orig: @0x4378d8..
// 0x4379b0 — steps of 0x4000 from 0.25 u while sample < max_dist >> 14;
// terrain hit: accum -= dir >> 2 per axis and stop; result = the step
// distance reached + dot(dir, accum)]. bone_count 0 returns max_dist.
int32_t death_camera_probe_terrain(const CameraTerrainSampler &terrain,
                                   int bone_count,
                                   const int32_t origin[3],
                                   const int32_t dir_q16[3], int32_t lift,
                                   int32_t max_dist);

// The FROM/TO computation [orig: Camera_ComputeThirdPersonPositions
// @0x438b80]. `player_pos`/`anchor_pos` are 16.16 mission positions.
void death_camera_compute(const int32_t player_pos[3],
                          const int32_t anchor_pos[3],
                          const CameraRayProbe &probe,
                          DeathCameraState &out);

// The per-frame lerp [orig: @0x4389f8..0x438b3d]: t = min((tick -
// start) << 9, 0x10000) — 128 ticks to settle — and each channel is
// from + (((to - from) * t + 0x8000) >> 16).
void death_camera_view(const DeathCameraState &state, uint32_t tick,
                       DeathCameraPose &out);

// The trial-angle step and the probe reach the original uses.
constexpr int32_t kDeathCameraTrialStepBam = 0x1000000;
constexpr int32_t kDeathCameraReachQ16 = 0x50000; // 5.0 u
constexpr int32_t kDeathCameraLiftQ16 = 0x8000;   // 0.5 u

} // namespace opennova::world
