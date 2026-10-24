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
    // (g_TickPhase & 3 == 0): an 8 ms frame already ticks, the next 8 ms
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

    // A 300 ms frame inside a steady 16 ms stream keeps its time: the EMA pays it
    // back over the following frames, 17 ticks beyond what a 16 ms frame runs,
    // where the same frame's time capped at 8/60 s (Godot's process step under
    // its default max_physics_steps_per_frame, which the shell no longer banks:
    // it hands the session the frame's measured elapsed, D-NET-NEW-LI-6) adds 6
    // [orig: Game_MainLoop @0x52B630 -- the elapsed @0x52B798..0x52B7A4, the add
    // @0x52B7B2, the EMA in place @0x52B845..0x52B85B].
    {
        const auto catchup = [](double long_frame) {
            TickAccumulator acc;
            for (int i = 0; i < 60; ++i) acc.bank(TickAccumulator::kTickDt);
            int ticks = acc.bank(long_frame);
            for (int i = 0; i < 120; ++i) ticks += acc.bank(TickAccumulator::kTickDt);
            return ticks;
        };
        const int stream = catchup(TickAccumulator::kTickDt);
        TEST_EXPECT(catchup(0.300) - stream == 17);
        TEST_EXPECT(catchup(8.0 / 60.0) - stream == 6);
    }

    // The lose cine's edit fade scales the smoothing history, not the bank:
    // the frame that scaled it keeps its ticks and the next frame banks against
    // the scaled word. A 16 ms frame banks 272 units (1 tick, 16 left); x1.66f
    // truncates the history to 451; the next 16 ms frame smooths to
    // (7 * 451 + 272 + 4) >> 3 = 429 units, 6 quanta from phase 4 = 2 ticks
    // (45 left); x0.60240966f truncates 429 to 258; the next frame smooths to
    // (7 * 258 + 301 + 4) >> 3 = 263 units, 4 quanta from phase 10 = 1 tick.
    // Unscaled, every one of those frames runs 1 tick. The products sit clear
    // of an integer, so x87's 24-bit device precision truncates them the same.
    // [orig: Game_ScaleSmoothedFrameTime @0x52B5D0; CCineEventEditFade_Start
    //  @0x5726C9 (flt_7D6858); CCineEventEditFade_Finish @0x572775 (flt_7D685C);
    //  Game_MainLoop's EMA read @0x52B845]
    {
        const float kStart = 1.66f;      // 0x3FD47AE1
        const float kFinish = 0.60240966f; // 0x3F1A3785
        TickAccumulator acc;
        TEST_EXPECT(acc.bank(TickAccumulator::kTickDt) == 1);
        acc.scale_smoothing_history(kStart);
        TEST_EXPECT(std::abs(acc.banked() - kMs) < 1e-9); // the bank itself is untouched
        TEST_EXPECT(acc.bank(TickAccumulator::kTickDt) == 2);
        TEST_EXPECT(std::abs(acc.banked() - 45.0 / 16000.0) < 1e-9);
        acc.scale_smoothing_history(kFinish);
        TEST_EXPECT(acc.bank(TickAccumulator::kTickDt) == 1);
        TEST_EXPECT(std::abs(acc.banked() - 7.0 / 16000.0) < 1e-9);

        TickAccumulator twin;
        TEST_EXPECT(twin.bank(TickAccumulator::kTickDt) == 1);
        TEST_EXPECT(twin.bank(TickAccumulator::kTickDt) == 1);
        TEST_EXPECT(twin.bank(TickAccumulator::kTickDt) == 1);

        // An empty history stays empty: the next frame banks unsmoothed.
        TickAccumulator cold;
        cold.scale_smoothing_history(kStart);
        TEST_EXPECT(cold.bank(TickAccumulator::kTickDt) == 1);
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

// The CPU share the same window publishes [orig: Game_MainLoop
// @0x52b926..0x52b948]: the frames' work milliseconds * 100 over 16 ms per
// logic update, a zero update count reading one; both restart with the window.
int check_cpu_percent() {
    TickAccumulator acc;
    // 100 ms frames close the window on the 20th; 19 frames of 4 ms work over
    // 6 updates each before it.
    for (int i = 0; i < 19; ++i) {
        acc.bank(0.1);
        acc.record_frame_work(4, 6);
    }
    TEST_EXPECT(acc.cpu_percent() == 0);
    acc.bank(0.1); // closes: 76 ms * 100 / (114 * 16) = 4
    TEST_EXPECT(acc.cpu_percent() == 4);
    // A window with work but no updates divides by one update's 16 ms.
    for (int i = 0; i < 19; ++i) {
        acc.bank(0.1);
        acc.record_frame_work(1, 0);
    }
    acc.bank(0.1);
    TEST_EXPECT(acc.cpu_percent() == 118); // 19 * 100 / 16
    acc.reset();
    TEST_EXPECT(acc.cpu_percent() == 0);
    return 0;
}

// The window's extrema (D-NET-NEW-LI-32, the frame-rate overlay's MN / MX) [orig: Game_MainLoop
// @0x52B630 - the longest @0x52B8F6..0x52B902, the shortest @0x52B90E..0x52B914, the restarts
// @0x52B97B / @0x52B985, g_StatsMaxFps @0x52B99D, g_StatsMinFps @0x52B9A9, the mode-init reset
// @0x52B739 / @0x52B73F; the statics @0x83016C = 2000, @0x830170 = 1]: every counted frame's
// smoothed bank raises the longest and lowers the shortest; the window publishes 16000 /
// longest as the minimum rate and 16000 / shortest as the maximum, then restarts them at
// 32000 units and 1. The mode-init reset zeroes the published rates alone.
int check_frame_rate_extrema() {
    // A steady 200 ms stream banks 3216 units a frame (the 1 ms residual re-counted) and
    // closes the window on the 10th (32160): the process's first window starts from the
    // 125 ms static, so its maximum reads 16000 / 2000 = 8 though no frame was that short,
    // and its minimum 16000 / 3216 = 4.
    {
        TickAccumulator acc;
        for (int i = 0; i < 9; ++i) acc.bank(0.2);
        TickAccumulator::FrameRateStats s = acc.frame_rate_stats();
        TEST_EXPECT(s.min_fps == 0 && s.max_fps == 0);
        acc.bank(0.2);
        s = acc.frame_rate_stats();
        TEST_EXPECT(s.average_fps == 4 && s.min_fps == 4 && s.max_fps == 8);
        // The restart's 2000 ms: the next window's maximum is its own frames', 4.
        for (int i = 0; i < 10; ++i) acc.bank(0.2);
        s = acc.frame_rate_stats();
        TEST_EXPECT(s.average_fps == 4 && s.min_fps == 4 && s.max_fps == 4);
    }
    // The mode-init reset keeps the extrema: five 200 ms frames, the reset, then a 16 ms
    // stream (272 units a frame) whose window closes on its 118th frame with the 3216-unit
    // frame from before the reset still its longest: the minimum reads 16000 / 3216 = 4,
    // the maximum 16000 / 272 = 58.
    {
        TickAccumulator acc;
        for (int i = 0; i < 5; ++i) acc.bank(0.2);
        acc.reset();
        TickAccumulator::FrameRateStats s = acc.frame_rate_stats();
        TEST_EXPECT(s.average_fps == 0 && s.min_fps == 0 && s.max_fps == 0 && s.cpu_percent == 0);
        for (int i = 0; i < 118; ++i) acc.bank(TickAccumulator::kTickDt);
        s = acc.frame_rate_stats();
        TEST_EXPECT(s.average_fps == 58 && s.max_fps == 58 && s.min_fps == 4);
    }
    return 0;
}

} // namespace

int main() {
    if (int rc = check_bank()) return rc;
    if (int rc = check_cpu_percent()) return rc;
    if (int rc = check_frame_rate_extrema()) return rc;
    return check_frame_rate();
}
