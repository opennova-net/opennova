/* world::TickAccumulator — the 62.5 Hz wall-clock bank
   [orig: Game_MainLoop @ 0x52b630], pinned headless. */

#include <cmath>

#include "common/test_expect.h"

#include <runtime/world/tick_accumulator.h>

int main() {
    using opennova::world::TickAccumulator;

    // One quantum banks one tick; the carry stays fractional.
    {
        TickAccumulator acc;
        TEST_EXPECT(acc.bank(TickAccumulator::kTickDt) == 1);
        TEST_EXPECT(acc.banked() < 1e-12);
    }

    // Two half-quanta: nothing due, then the carry completes a tick.
    {
        TickAccumulator acc;
        TEST_EXPECT(acc.bank(TickAccumulator::kTickDt * 0.5) == 0);
        TEST_EXPECT(acc.bank(TickAccumulator::kTickDt * 0.5) == 1);
    }

    // A long frame runs several ticks and keeps the sub-quantum remainder.
    {
        TickAccumulator acc;
        TEST_EXPECT(acc.bank(TickAccumulator::kTickDt * 2.5) == 2);
        TEST_EXPECT(std::abs(acc.banked() - TickAccumulator::kTickDt * 0.5) < 1e-9);
    }

    // The spiral-of-death clamp: a hitch caps at 31 and DROPS the backlog.
    {
        TickAccumulator acc;
        TEST_EXPECT(acc.bank(2.0) == TickAccumulator::kMaxCatchupTicks);
        TEST_EXPECT(acc.banked() == 0.0);
        // The next ordinary frame resumes cleanly.
        TEST_EXPECT(acc.bank(TickAccumulator::kTickDt) == 1);
    }

    // Exactly the clamp boundary is NOT a drop: 31 quanta run, carry survives.
    {
        TickAccumulator acc;
        const double t = TickAccumulator::kTickDt * 31 + TickAccumulator::kTickDt * 0.25;
        TEST_EXPECT(acc.bank(t) == 31);
        TEST_EXPECT(std::abs(acc.banked() - TickAccumulator::kTickDt * 0.25) < 1e-9);
    }

    // reset() discards banked time (Play/Step/Stop transitions).
    {
        TickAccumulator acc;
        TEST_EXPECT(acc.bank(TickAccumulator::kTickDt * 0.9) == 0);
        acc.reset();
        TEST_EXPECT(acc.bank(TickAccumulator::kTickDt * 0.5) == 0);
    }

    return 0;
}
