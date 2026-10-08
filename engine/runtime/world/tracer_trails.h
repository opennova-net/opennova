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
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include <runtime/world/geom.h>

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

// The style blocks' +8 and +0xC words: the trail anchor's wobble rate (BAM per
// entity update) and amplitude (16.16), which a channel copies at its init and
// the round's trail point reads (tracer_trail_anchor). The static std/rapid/
// sniper blocks hold 0x10000000 / 0x400; the runtime-built DF1 pair 0x10000000
// / 0x100, rocket and at4 0x8000000 / 0x4000, grenade 0x8000000 / 0x1000 and the
// NVG laser 0x8000000 / 0.
// [orig: g_TracerStyle_StdRed..SniperGreen +8/+0xC @ 0x8437F8..0x8460EC;
//  CEffectEmitterPool_ResetAndBuildStyles @ 0x5DB3E5..0x5DB3EB (DF1 red),
//  @ 0x5DB467..0x5DB46D (DF1 green), @ 0x5DB4E8..0x5DB4F2 (rocket),
//  @ 0x5DB58C..0x5DB596 (at4), @ 0x5DB62A..0x5DB636 (grenade),
//  @ 0x5DB6E1..0x5DB6EB (NVG laser); CEffectChannel_Init @ 0x5DB243..0x5DB249]
inline constexpr int32_t kTracerStyleWobbleRate[kTracerStyleCount] = {
    0x10000000, 0x10000000, 0x10000000, 0x8000000, 0x8000000, 0x8000000, 0x10000000,
    0x10000000, 0x8000000, 0x10000000, 0x10000000, 0x10000000, 0x10000000,
};
inline constexpr int32_t kTracerStyleWobbleAmplitude[kTracerStyleCount] = {
    0x400, 0x400, 0x400, 0x4000, 0x4000, 0x1000, 0x400, 0x400, 0, 0x400, 0x400, 0x100, 0x100,
};

inline int32_t tracer_style_cap(int32_t style_id) {
    return (style_id >= 1 && style_id < kTracerStyleCount) ? kTracerStyleCap[style_id]
                                                           : kTracerStyleCap[0];
}
inline bool tracer_style_jitter(int32_t style_id) {
    return (style_id >= 1 && style_id < kTracerStyleCount) ? kTracerStyleJitter[style_id]
                                                           : kTracerStyleJitter[0];
}
inline int32_t tracer_style_wobble_rate(int32_t style_id) {
    return (style_id >= 1 && style_id < kTracerStyleCount) ? kTracerStyleWobbleRate[style_id]
                                                           : kTracerStyleWobbleRate[0];
}
inline int32_t tracer_style_wobble_amplitude(int32_t style_id) {
    return (style_id >= 1 && style_id < kTracerStyleCount)
                   ? kTracerStyleWobbleAmplitude[style_id]
                   : kTracerStyleWobbleAmplitude[0];
}

// One trail channel [orig: the 44-B slot — c[0]/c[1] the wobble words, c[2] point
// buffer, c[3] cap, c[4] count, c[5] age, c[6] style id, c[10] kill flag].
struct TracerTrailChannel {
    static constexpr int kMaxPoints = 112; // the largest witnessed cap (rocket/at4)

    bool active = false;
    bool kill = false;      // c[10]: drain-then-free requested (round died)
    int32_t wobble_rate = 0;      // c[0] = style +8 [orig: @ 0x5db243]
    int32_t wobble_amplitude = 0; // c[1] = style +0xC [orig: @ 0x5db249]
    int32_t style_id = 0;   // c[6]: the tracer_type id selected at alloc
    int32_t cap = 0;        // c[3]: ring length = the style's point cap
    int32_t count = 0;      // c[4]: live points; -1 from alloc until the first append
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
    // The channel starts at count -1 [orig: CEffectChannel_Init @ 0x5db233].
    int alloc(int32_t style_id);

    // Ring append: full -> drop the oldest first; jitter styles stamp the per-point
    // width multiplier; resets the channel age [orig: CEffectChannel_AppendPoint
    // @ 0x5db290 — w = 1.0 + PRNG_Next16() * 1e-5 on desc+4 styles, else 1.0]. At
    // count -1 (a fresh channel) it stores nothing and only lifts the count to 0,
    // so a round's first pre-move point never draws.
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

// The point a round appends to its trail channel: the round position plus its
// euler matrix (heading, pitch, roll, about the 16.16 position) applied to
// {0, d, d}, where d = (amplitude x (sine >> 6) + 0x8000) >> 16 and the sine is
// the Q22 table entry at (entity_update_counter x rate + 0x200000) >> 22. The
// rate and amplitude are the channel's c[0] / c[1] words, so the point waves
// across the flight path every 32 updates (rocket, at4, grenade) or 16 (the
// bullet tracers), 0.25 units out for a rocket. Every caller reaches it only
// with the round's channel set. [orig: Projectile_GetTrailAnchorPos @ 0x4E64E0
// (the sine @ 0x4E6503..0x4E652D, d @ 0x4E652A..0x4E6587, the transform
// Math_FixedPointTransformPoint22 @ 0x4E658B over
// Math_BuildFixedPointMatrixFromEulerAngles @ 0x4E64FE of round+4..+0x18);
// the per-tick appends @ 0x4EA04F / @ 0x4EA97A, the death append
// Projectile_ReleaseEffects @ 0x4E8292; g_EntityUpdateCounter @ 0x24C1948]
Vec3 tracer_trail_anchor(const TracerTrailChannel &channel, const Vec3 &position,
                         int32_t heading_bam, int32_t pitch_bam, int32_t roll_bam,
                         uint32_t entity_update_counter);

} // namespace opennova::world
