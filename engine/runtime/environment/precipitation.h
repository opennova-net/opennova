#pragma once

// The precipitation drop pool [orig: the 3072-slot {x, y, z, floor} 16.16
// table at 0x2BF99D0 (the walkers hold the y column, unk_2BF99D4);
// Precipitation_SeedPool (ex sub_5DEBB0) @ 0x5debb0;
// Precipitation_Reset (ex sub_5DF3A0) @ 0x5df3a0 from Game_StartMission
// @ 0x5249d4; Precipitation_FallTick (ex sub_5DE8F0) @ 0x5de8f0 from
// Entity_UpdateAllEntities @ 0x4c2214; WeatherParticle_UpdatePositions
// @ 0x5dec40 from the drawer Render_WeatherTrailParticles @ 0x5dee10].
//
// Every slot is one drop in the mission frame (16.16, Z up): the drawer walks
// the first `active` slots, the fall tick lowers every slot each logic tick
// while it rains, and the per-render update wraps each slot back into the
// 32 x 32 x 16 m volume around the camera, re-flooring a wrapped slot on the
// terrain / water / the first entity under it. Retail's drops therefore
// never spawn — they recycle through the volume forever. The compile into
// triangles is renderer/precipitation_draw_list.h.

#include <vector>
#include <cstdint>

namespace opennova::env {

struct PrecipitationSlot {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    int32_t floor_z = 0; // the terrain/water/entity height under (x, y)
};

// The embedder's floor queries for a wrapped slot, mission-frame 16.16.
// terrain_height: the bilinear terrain height at (x, y)
// [orig: Terrain_SampleHeightBilinear @ 0x6067b0]. entity_hit: the first
// entity surface between z_top and z_bottom under (x, y); returns true and
// the hit height [orig: Physics_RaycastIntContext @ 0x5385e0 +
// Physics_RaycastProximityEntities @ 0x538350, the ray from floor + 200 m down to
// the floor]. Null callbacks skip that query.
struct PrecipitationFloorSampler {
    int32_t (*terrain_height)(void *ctx, int32_t x, int32_t y) = nullptr;
    bool (*entity_hit)(void *ctx, int32_t x, int32_t y, int32_t z_top,
            int32_t z_bottom, int32_t &hit_z) = nullptr;
    void *ctx = nullptr;
};

struct PrecipitationField {
    static constexpr int kSlots = 3072;
    // The rain gate: g_EnvRainPctCurrent > 48 (16.16) [orig: @ 0x5de92e; @ 0x5dee48].
    static constexpr int32_t kRainGateQ16 = 48;
    // Per-tick fall in 16.16 [orig: @ 0x5de938 rain, @ 0x5de93f snow].
    static constexpr int32_t kRainFallPerTick = 12288;
    static constexpr int32_t kSnowFallPerTick = 2048;
    // The volume around the camera: x/y span 0x200000 from camera - 0x40000,
    // z span 0x100000 from camera - 0x80000 [orig: @ 0x5dec67..0x5dec75].
    static constexpr int32_t kSpanXY = 0x200000;
    static constexpr int32_t kBackXY = 0x40000;
    static constexpr int32_t kSpanZ = 0x100000;
    static constexpr int32_t kBelowZ = 0x80000;
    // The entity raycast starts 200 m above the terrain floor
    // [orig: the 0xC80000 constant @ 0x5dedc4].
    static constexpr int32_t kRaycastHeightQ16 = 0xC80000;

    // Retail's static 3072-slot table; heap-backed here so the World (and
    // every snapshot of it) stays small enough for stack-built worlds.
    std::vector<PrecipitationSlot> slots = std::vector<PrecipitationSlot>(kSlots);
    // The accumulated fall since the last draw, the drawer's trail vector
    // source [orig: dword_2C05A2C, zeroed by the drawer @ 0x5deee8].
    int32_t fall_accum_z = 0;

    // Precipitation_Reset: zero everything, then seed. `rand16` is the PRNG_B
    // stream draw the seed consumes three times per slot.
    void reset(uint16_t (*rand16)(void *ctx), void *ctx);
    // Precipitation_SeedPool: x, y = (r << 21 + 0x8000) >> 16 (0..32 m), z =
    // (r << 20 + 0x8000) >> 16 (0..16 m) [orig: @ 0x5debed..0x5dec2c].
    void seed(uint16_t (*rand16)(void *ctx), void *ctx);
    // Precipitation_FallTick: while raining, every slot falls by the kind's
    // per-tick amount and the accumulator records it.
    void fall_tick(int32_t rain_pct_q16, uint32_t precipitation_kind);
    // The active slot count the drawer and the update walk:
    // (3072 * rain + 0x8000) >> 16, only when > 1, capped at 3072
    // [orig: @ 0x5dec92..0x5deca6; @ 0x5df15d..0x5df170].
    static int active_count(int32_t rain_pct_q16);
    // WeatherParticle_UpdatePositions: wrap every active slot into the
    // camera volume; a slot that wrapped on any axis is re-floored.
    void update(int32_t cam_x, int32_t cam_y, int32_t cam_z,
            int32_t rain_pct_q16, int32_t water_height_q16,
            const PrecipitationFloorSampler &sampler);
};

} // namespace opennova::env
