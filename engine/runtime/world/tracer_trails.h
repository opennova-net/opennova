// The tracer/trail emitter pool — the per-round point-ring channels behind every
// in-flight tracer streak, rocket smoke trail, and sniper wake. [orig: the 256 x 44-B
// channel pool @ 0x2BF5270 (g_TracerEmitterPool), reset + style build at mission start
// by CEffectEmitterPool_ResetAndBuildStyles @ 0x5DB3A0 (thiscall from Game_StartMission
// @ 0x525df7); alloc CEffectEmitterPool_AllocSlot @ 0x5DB7A0; append
// CEffectChannel_AppendPoint @ 0x5DB290; per-frame drain CEffectEmitterPool_Tick
// @ 0x5DB830 (Game_ProcessMainFrame @ 0x526758); ribbon draw
// CEffectChannel_RenderRibbon @ 0x5DB8A0. docs/world/world-wac-ai-re.md §18.4.]
//
// Split: the SIM owns the rings (append cadence is tick-accurate — one pre-move point
// per 62 Hz round tick, final point + kill on round death), the HOST styles and draws
// them (colors/sizes/blend live in the present pass beside the other presentation
// tables). The style CAP (ring length) is engine data consumed sim-side, so the
// witnessed per-type cap/jitter columns live here.
#ifndef OPENNOVA_WORLD_TRACER_TRAILS_H
#define OPENNOVA_WORLD_TRACER_TRAILS_H

#include <array>
#include <cstdint>
#include <vector>

#include "world/geom.h"

namespace opennova::world {

// One appended trail point [orig: the 16-B channel vertex {x,y,z fixed, w float}].
// w is the per-point width multiplier: styles with the jitter flag (smoke/sniper/NVG
// families) get 1.0 + rand16 * 1e-5, everything else exactly 1.0
// [orig: CEffectChannel_AppendPoint @ 0x5db2e2-0x5db30a].
struct TracerTrailPoint {
    Vec3 pos;
    float w = 1.0f;
};

// The witnessed per-style ring caps (style block +0x10 — also the color-ramp length)
// and jitter flags (style block +4). Ids are the ammo.def `tracer_type` ids
// [orig: AmmoDef_ParseTypeName @ 0x409b90 — stdred 1, stdgreen 2, rocket 3, at4 4,
// grenade 5, rapidred 6, rapidgreen 7, (8 = the NVG laser style, no name), sniperred 9,
// snipergreen 10, df1red 11, df1green 12; any other id takes the stdred block
// (the CEffectChannel_Init default case @ 0x5db1c8)].
inline constexpr int kTracerStyleCount = 13; // index 0 = the default/fallback row
inline constexpr int32_t kTracerStyleCap[kTracerStyleCount] = {
    12, 12, 12, 112, 112, 64, 6, 6, 32, 20, 20, 32, 32,
};
inline constexpr bool kTracerStyleJitter[kTracerStyleCount] = {
    false, false, false, true, true, true, false, false, true, true, true, false, false,
};

inline int32_t tracer_style_cap(int32_t style_id) {
    return (style_id >= 1 && style_id < kTracerStyleCount) ? kTracerStyleCap[style_id]
                                                           : kTracerStyleCap[0];
}
inline bool tracer_style_jitter(int32_t style_id) {
    return (style_id >= 1 && style_id < kTracerStyleCount) ? kTracerStyleJitter[style_id]
                                                           : kTracerStyleJitter[0];
}

// One trail channel [orig: the 44-B slot — c[2] point buffer, c[3] cap, c[4] count,
// c[5] age, c[6] style id, c[10] kill flag].
struct TracerTrailChannel {
    static constexpr int kMaxPoints = 112; // the largest witnessed cap (rocket/at4)

    bool active = false;
    bool kill = false;      // c[10]: drain-then-free requested (round died)
    int32_t style_id = 0;   // c[6]: the tracer_type id selected at alloc
    int32_t cap = 0;        // c[3]: ring length = the style's point cap
    int32_t count = 0;      // c[4]: live points
    int32_t age = 0;        // c[5]: ticks since the last append (saturates at cap)
    std::array<TracerTrailPoint, kMaxPoints> pts{}; // [0] oldest .. [count-1] newest
};

class TracerTrailPool {
public:
    static constexpr int kChannels = 256; // [orig: 256 slots + 256 alloc flags]

    // Heap storage — the pool is ~460 KB (256 x 112 points); World lives on the
    // stack in tests and hosts. [orig: a static global @ 0x2BF5270.]
    std::vector<TracerTrailChannel> channels =
            std::vector<TracerTrailChannel>(static_cast<size_t>(kChannels));

    // First-free scan alloc [orig: CEffectEmitterPool_AllocSlot @ 0x5db7a0]; -1 = full.
    int alloc(int32_t style_id);

    // Ring append: full -> drop the oldest first; jitter styles stamp the per-point
    // width multiplier; resets the channel age [orig: CEffectChannel_AppendPoint
    // @ 0x5db290 — w = 1.0 + PRNG_Next16() * 1e-5 on desc+4 styles, else 1.0].
    void append(int slot, const Vec3 &pos);

    // Round death: drain one point per tick once age reaches cap, free when empty
    // [orig: CEffectChannel_RequestKill @ 0x5db380 sets c[10]].
    void request_kill(int slot);

    // Once per 62 Hz logic tick [orig: CEffectEmitterPool_Tick @ 0x5db830]: per active
    // channel age < cap -> age++, else pop the oldest point; kill + empty -> free.
    void tick();

    void reset() noexcept;

private:
    // Presentation-only width jitter. The original draws from the shared effect PRNG
    // (PRNG_Next16_B @ 0x6130f0, stream unwitnessed); any uniform 16-bit source is
    // visually equivalent — this LCG is not gameplay state and is not replicated.
    uint32_t jitter_seed_ = 0x4d595df4u;
    uint16_t next_rand16();
};

} // namespace opennova::world

#endif // OPENNOVA_WORLD_TRACER_TRAILS_H
