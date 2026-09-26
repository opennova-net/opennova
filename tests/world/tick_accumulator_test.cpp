/* world::TickAccumulator, pinned headless: the retail frame-time bank
   [orig: Game_MainLoop @ 0x52b630]: 1/16 ms units, the 1 ms seed, the 500 ms
   clamp, the 7/8 EMA, the 4 ms drain quantum, the free-running tick phase,
   and the FR counter that averages the smoothed bank over 2000 ms windows. */

#include <cmath>

#include "common/test_expect.h"

#include <runtime/world/tick_accumulator.h>

namespace {

using opennova::world::TickAccumulator;

int check_bank() {
    constexpr double kMs = 0.001;

    // One 16 ms frame banks one tick on the aligned phase; the 1 ms seed is
    // the only residual (16 + 256 units -> four 4 ms quanta drained -> 16).
    {
        TickAccumulator acc;
        TEST_EXPECT(acc.bank(TickAccumulator::kTickDt) == 1);
        TEST_EXPECT(std::abs(acc.banked() - kMs) < 1e-9);
    }

    // The logic update runs on the FIRST 4 ms quantum of each 16 ms group
    // (g_tickPhase & 3 == 0): an 8 ms frame already ticks, the next 8 ms
    // frame completes the group without a second tick.
    {
        TickAccumulator acc;
        TEST_EXPECT(acc.bank(TickAccumulator::kTickDt * 0.5) == 1);
        TEST_EXPECT(acc.bank(TickAccumulator::kTickDt * 0.5) == 0);
        TEST_EXPECT(acc.bank(TickAccumulator::kTickDt * 0.5) == 1);
    }

    // Under 4 ms nothing drains; the residual carries into the next frame.
    {
        TickAccumulator acc;
        TEST_EXPECT(acc.bank(2.0 * kMs) == 0);
        TEST_EXPECT(std::abs(acc.banked() - 3.0 * kMs) < 1e-9);
        TEST_EXPECT(acc.bank(14.0 * kMs) == 1);
    }

    // A long first frame runs several ticks (no EMA history yet) and keeps
    // the sub-quantum remainder: 16 + 640 units -> 10 quanta, phases 0..9.
    {
        TickAccumulator acc;
        TEST_EXPECT(acc.bank(TickAccumulator::kTickDt * 2.5) == 3);
        TEST_EXPECT(std::abs(acc.banked() - kMs) < 1e-9);
    }

    // The first frame after a reset is unsmoothed: 32 ms banks 16 + 512 = 528
    // units, 8 quanta, 2 ticks. The next 1 ms frame is smoothed against it,
    // (7 * 528 + 32 + 4) >> 3 = 466 units, 7 quanta from phase 8 = 2 ticks,
    // and a 2.0 s stall then clamps to 8000 units, 125 quanta from phase 15
    // = 31 ticks (32 only from an aligned phase).
    {
        TickAccumulator acc;
        TEST_EXPECT(acc.bank(0.032) == 2);
        TEST_EXPECT(acc.bank(0.001) == 2);
        TEST_EXPECT(acc.bank(2.0) == 31);
    }

    // The 500 ms clamp bypasses the EMA: a 2 s stall drains 125 quanta from
    // phase 0 (32 logic ticks, the fixed maximum) and leaves no residual.
    {
        TickAccumulator acc;
        TEST_EXPECT(acc.bank(2.0) == TickAccumulator::kRetailMaxCatchupTicks);
        TEST_EXPECT(acc.banked() == 0.0);
        // The next ordinary frame is smoothed against the clamped 8000-unit
        // history: (7 * 8000 + 256 + 4) >> 3 = 7032 units -> 109 quanta from
        // phase 125 -> 27 ticks. This is retail's post-stall fast-forward.
        TEST_EXPECT(acc.bank(TickAccumulator::kTickDt) == 27);
    }

    // Steady 16 ms frames converge on exactly one tick per frame: the EMA of
    // a constant is that constant.
    {
        TickAccumulator acc;
        int ticks = 0;
        for (int i = 0; i < 64; ++i) ticks += acc.bank(TickAccumulator::kTickDt);
        TEST_EXPECT(ticks == 64);
    }

    // reset() discards the bank, the smoothing history and the phase: the
    // frame after it banks from the 1 ms seed, unsmoothed.
    {
        TickAccumulator acc;
        TEST_EXPECT(acc.bank(2.0) == TickAccumulator::kRetailMaxCatchupTicks);
        acc.reset();
        TEST_EXPECT(std::abs(acc.banked() - kMs) < 1e-9);
        TEST_EXPECT(acc.bank(TickAccumulator::kTickDt) == 1);
    }
    return 0;
}

// The FR counter [orig: Game_MainLoop @0x52B8ED..0x52B98F]: each frame adds
// its smoothed bank, and a sum of at least 32000 units (2000 ms) publishes
// frames * 16000 / sum and restarts the window.
int check_frame_rate() {
    // A steady 16 ms stream re-counts the 1 ms seed residual every frame, so a
    // frame sums 272 units: frame 118 closes the window at 32096 units,
    // 118 * 16000 / 32096 = 58. The rate reads 0 until then.
    {
        TickAccumulator acc;
        for (int i = 0; i < 117; ++i) acc.bank(TickAccumulator::kTickDt);
        TEST_EXPECT(acc.average_fps() == 0);
        acc.bank(TickAccumulator::kTickDt);
        TEST_EXPECT(acc.average_fps() == 58);
        // The window restarts: 100 ms frames keep the published 58 until
        // their own window closes on the 26th frame, 12 over the EMA's
        // transition from 16 ms.
        for (int i = 0; i < 25; ++i) acc.bank(0.1);
        TEST_EXPECT(acc.average_fps() == 58);
        acc.bank(0.1);
        TEST_EXPECT(acc.average_fps() == 12);
        // reset() is the mode-init clear: the rate reads 0 again.
        acc.reset();
        TEST_EXPECT(acc.average_fps() == 0);
    }

    // A steady 100 ms stream sums 1616 units a frame: frame 20 closes the
    // window at 32320 units, 20 * 16000 / 32320 = 9.
    {
        TickAccumulator acc;
        for (int i = 0; i < 19; ++i) acc.bank(0.1);
        TEST_EXPECT(acc.average_fps() == 0);
        acc.bank(0.1);
        TEST_EXPECT(acc.average_fps() == 9);
    }
    return 0;
}

} // namespace

int main() {
    if (int rc = check_bank()) return rc;
    return check_frame_rate();
}
