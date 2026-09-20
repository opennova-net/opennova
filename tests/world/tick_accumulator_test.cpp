/* world::TickAccumulator — the retail frame-time bank
   [orig: Game_MainLoop @ 0x52b630], pinned headless: 1/16 ms units, the 1 ms
   seed, the 500 ms clamp, the 7/8 EMA, the 4 ms drain quantum and the
   free-running tick phase. */

#include <cmath>

#include "common/test_expect.h"

#include <runtime/world/tick_accumulator.h>

int main() {
    using opennova::world::TickAccumulator;
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

    // The 500 ms clamp bypasses the EMA: a 2 s stall drains 125 quanta from
    // phase 0 (32 logic ticks, the fixed maximum) and leaves no residual.
    {
        TickAccumulator acc;
        TEST_EXPECT(acc.bank(2.0) == TickAccumulator::kMaxCatchupTicks);
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

    // reset() discards the bank, the smoothing history and the phase.
    {
        TickAccumulator acc;
        TEST_EXPECT(acc.bank(2.0) == TickAccumulator::kMaxCatchupTicks);
        acc.reset();
        TEST_EXPECT(std::abs(acc.banked() - kMs) < 1e-9);
        TEST_EXPECT(acc.bank(TickAccumulator::kTickDt) == 1);
    }

    return 0;
}
