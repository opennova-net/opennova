#pragma once

#include <cstdint>
#include <base/io/tick_rate.h>

namespace opennova::world {

// The ONE engine tick cadence: 62.5 Hz — the 16 ms drain quantum Game_MainLoop
// banks wall-clock into, advancing the logic tick counter once per quantum
// [orig: current_tick @ 0x24c1968, incremented per drained 16 ms quantum in
// Game_MainLoop @ 0x52b630].

// Wall-clock milliseconds -> whole logic ticks (the original's ms/16 quantum
// count; fractional remainders truncate exactly like the drain).
constexpr int32_t ticks_from_ms(int64_t ms) {
    return static_cast<int32_t>(ms / io::kTickMs);
}

// The fixed-62.5 Hz real-time accumulator [orig: Game_MainLoop @ 0x52b630]:
// bank wall-clock seconds, drain them in kTickDt quanta, and clamp a hitch's
// backlog to kMaxCatchupTicks — dropping the remainder so a load stall cannot
// spiral catch-up into the following frames. The embedder runs the returned
// number of logic ticks and presents once after the batch; a zero return
// still presents render-only state (camera and attach can change between
// fixed ticks).
class TickAccumulator {
public:
	// 16 ms; matches AiEventQueue::kFrameDt.
	static constexpr double kTickDt = 1.0 / io::kTickHz;
	// The spiral-of-death clamp: the 500 ms / 16 ms accumulator cap.
	static constexpr int kMaxCatchupTicks = 31;

	// Bank `delta` seconds of wall-clock; returns the logic ticks due now
	// (>= 0). Fractional remainders stay banked; a clamped backlog is dropped.
	int bank(double delta);

	// Discard banked wall-clock (Play/Step/Stop transitions), so resuming
	// does not burst-catch-up across a pause or load.
	void reset() { accum_ = 0.0; }

	double banked() const { return accum_; }

private:
	double accum_ = 0.0;
};

} // namespace opennova::world
