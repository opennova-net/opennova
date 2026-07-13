// The mouse-look pipeline (net-re §5.39): pixel deltas -> yaw/pitch BAM through the
// witnessed integer math — sens = setting << 11, the +0x8000 rounding, the scoped
// zoom divide, the flipmouse Y sense, and the ±80° pitch clamps with the +40°
// up-limit while prone. [orig: Input_ProcessMouseAxisBindings @ 0x499680; axis
// cases 166/164 @ 0x4e109d/@ 0x4e0fed]
#include "world/player_look.h"

#include <cstdint>
#include <cstdio>

using namespace opennova::world;

static int failures = 0;
#define CHECK(c) \
    do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

int main() {
    // --- the default-sens yaw step: 1 px right at setting 128 = 4 units << 16
    //     subtracted (mouse-right turns the engine heading NEGATIVE).
    //     scaled = (1 * (128<<11) + 0x8000) >> 16 = (0x40000 + 0x8000) >> 16 = 4.
    {
        PlayerLookSettings s;
        int32_t yaw = 0, pitch = 0;
        player_look_apply(yaw, pitch, s, 1, 0, 0, false);
        CHECK(yaw == -(4 << 16));
        CHECK(pitch == 0);
    }

    // --- Y sense: flipmouse OFF (default) negates the raw screen delta, so mouse
    //     FORWARD (raw -y) looks UP (+pitch); flipmouse ON passes it through.
    {
        PlayerLookSettings s;
        int32_t yaw = 0, pitch = 0;
        player_look_apply(yaw, pitch, s, 0, -10, 0, false);
        CHECK(pitch > 0);
        s.invert_y = true;
        int32_t pitch2 = 0;
        player_look_apply(yaw, pitch2, s, 0, -10, 0, false);
        CHECK(pitch2 < 0);
    }

    // --- sensitivity clamps to [1, 0x1FF] [orig: the mousescale adjust @ 0x49b19b].
    {
        PlayerLookSettings lo, hi;
        lo.sensitivity = -50;
        hi.sensitivity = 5000;
        int32_t yaw_lo = 0, yaw_hi = 0, pitch = 0;
        player_look_apply(yaw_lo, pitch, lo, 100, 0, 0, false);
        player_look_apply(yaw_hi, pitch, hi, 100, 0, 0, false);
        const int32_t want_lo = -static_cast<int32_t>(
            static_cast<uint32_t>((100LL * (1 << 11) + 0x8000) >> 16) << 16);
        const int32_t want_hi = -static_cast<int32_t>(
            static_cast<uint32_t>((100LL * (0x1FFLL << 11) + 0x8000) >> 16) << 16);
        CHECK(yaw_lo == want_lo);
        CHECK(yaw_hi == want_hi);
    }

    // --- the scoped zoom reduction divides the scaled sens [orig: @ 0x499714 —
    //     sens = base / current zoom].
    {
        PlayerLookSettings s; // default 128
        int32_t yaw_hip = 0, yaw_scoped = 0, pitch = 0;
        player_look_apply(yaw_hip, pitch, s, 64, 0, 0, false);
        player_look_apply(yaw_scoped, pitch, s, 64, 0, 4, false);
        // base: (64 * 0x40000 + 0x8000) >> 16 = 256 + 0 -> 256 units;
        // scoped /4: sens 0x10000 -> (64 * 0x10000 + 0x8000) >> 16 = 64 units.
        CHECK(yaw_hip == -(256 << 16));
        CHECK(yaw_scoped == -(64 << 16));
    }

    // --- pitch clamps: ±80°, and the UP limit drops to +40° while prone
    //     [orig: 0x38E38E00 / 0xC71C7200 / prone 0x1C71C700 @ 0x4e0ff7].
    {
        PlayerLookSettings s;
        s.sensitivity = 0x1FF;
        int32_t yaw = 0, pitch = 0;
        for (int i = 0; i < 500; ++i) player_look_apply(yaw, pitch, s, 0, -1000, 0, false);
        CHECK(pitch == kLookPitchMax);
        for (int i = 0; i < 500; ++i) player_look_apply(yaw, pitch, s, 0, 1000, 0, false);
        CHECK(pitch == kLookPitchMin);
        pitch = 0;
        for (int i = 0; i < 500; ++i) player_look_apply(yaw, pitch, s, 0, -1000, 0, true);
        CHECK(pitch == kLookPitchProneMax);
        // A pitch already above +40° clamps DOWN to it on the next prone apply.
        pitch = kLookPitchMax;
        player_look_apply(yaw, pitch, s, 0, -1, 0, true);
        CHECK(pitch == kLookPitchProneMax);
    }

    // --- yaw wraps through the BAM seam, no clamp [orig: case 166 — plain sub].
    {
        PlayerLookSettings s;
        s.sensitivity = 0x1FF;
        int32_t yaw = INT32_MIN + (1 << 16), pitch = 0;
        player_look_apply(yaw, pitch, s, 1000, 0, 0, false);
        CHECK(yaw > 0); // wrapped across the seam, x86 wrap semantics
    }

    if (failures == 0) std::printf("player_look_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
