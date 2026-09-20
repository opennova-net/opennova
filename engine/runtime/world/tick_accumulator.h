#pragma once

#include <cstdint>
#include <base/io/tick_rate.h>

namespace opennova::world {

// The ONE engine tick cadence: 62.5 Hz — every fourth 4 ms drain quantum of
// Game_MainLoop runs the logic update, advancing the logic tick counter once
// per 16 ms [orig: current_tick @ 0x24c1968, incremented per drained 16 ms
// quantum in Game_MainLoop @ 0x52b630].

// Wall-clock milliseconds -> whole logic ticks (the original's ms/16 quantum
// count; fractional remainders truncate exactly like the drain).
constexpr int32_t ticks_from_ms(int64_t ms) {
    return static_cast<int32_t>(ms / io::kTickMs);
}

// The retail frame-time bank [orig: Game_MainLoop @0x52B630], in its 1/16 ms
// fixed point: every frame adds 16 * elapsed ms onto the previous frame's
// drain residual (@0x52B7B2); a bank over 500 ms is clamped to 500 ms and
// bypasses smoothing (@0x52B83E), otherwise a 7/8 EMA against the last
// smoothed value is applied IN PLACE (@0x52B85B) and published
// (@0x52B866); the smoothed bank is then drained in 4 ms quanta while at least
// 4 ms remain (@0x52BA18/@0x52BA21), the logic update running on every
// quantum whose free-running phase is a multiple of four (@0x52BA47/@0x52BA6B).
// Time is conserved through the residual and the cross-frame phase, and a
// stall is followed by the EMA's geometric catch-up — the witnessed retail
// post-hitch fast-forward, not a dropped backlog. The embedder runs the
// returned number of logic ticks and presents once after the batch; a zero
// return still presents render-only state (camera and attach can change
// between fixed ticks).
class TickAccumulator {
public:
	// 16 ms; matches AiEventQueue::kFrameDt.
	static constexpr double kTickDt = 1.0 / io::kTickHz;
	// The bank's fixed point: 16 units per millisecond.
	static constexpr int32_t kUnitsPerMs = 16;
	// The 4 ms drain quantum and the 500 ms bank clamp, in bank units.
	static constexpr int32_t kQuantumUnits = 4 * kUnitsPerMs;
	static constexpr int32_t kBankClampUnits = 500 * kUnitsPerMs;
	// The most logic ticks one frame can run: a clamped 500 ms bank drained in
	// 4 ms quanta from a tick-aligned phase (125 quanta, 32 on the phase).
	static constexpr int kMaxCatchupTicks = 32;

	// Bank `delta` seconds of wall-clock; returns the logic ticks due now
	// (>= 0). The sub-quantum remainder stays banked.
	int bank(double delta);

	// Discard banked wall-clock and the smoothing history (Play/Step/Stop
	// transitions, the mode-init reset @0x52B715..0x52B757), so resuming does
	// not burst-catch-up across a pause or load.
	void reset() { bank_ = kInitialBankUnits; smoothed_ = 0; phase_ = 0; carry_ = 0.0; }

	// The banked residual in seconds.
	double banked() const { return static_cast<double>(bank_) / (1000.0 * kUnitsPerMs); }

private:
	// Game_MainLoop seeds the bank with one millisecond (`esi = 16` @0x52B63C).
	static constexpr int32_t kInitialBankUnits = 16;
	int32_t bank_ = kInitialBankUnits;   // esi: residual + this frame's elapsed, smoothed
	int32_t smoothed_ = 0;               // g_frameTimeSmoothedFp4
	uint8_t phase_ = 0;                  // g_tickPhase (a byte, free-running)
	double carry_ = 0.0;                 // sub-unit wall-clock remainder
};

} // namespace opennova::world
