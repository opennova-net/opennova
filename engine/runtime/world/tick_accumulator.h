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

// The fixed-62.5 Hz real-time accumulator: retail's frame-time bank, the one
// the game (Simulation) and the dedicated host (apps/nw_server) both bank
// through. Its 7/8 EMA low-pass filters the banked time, so a long frame's
// backlog is paid back over the next frames instead of as one burst of
// catch-up ticks. The embedder runs the returned number of logic ticks and
// presents once after the batch; a zero return still presents render-only
// state (camera and attach can change between fixed ticks).
//
// The retail bank [orig: Game_MainLoop @0x52B630], in its 1/16 ms fixed point:
// every frame adds 16 * elapsed ms onto the previous frame's drain residual
// (@0x52B7B2); a bank over 500 ms is clamped to 500 ms and bypasses smoothing
// (@0x52B83E), otherwise a 7/8 EMA against the last smoothed value is applied
// IN PLACE (@0x52B85B) and published (@0x52B866); the smoothed bank is then
// drained in 4 ms quanta while at least 4 ms remain (@0x52BA18/@0x52BA21), the
// logic update running on every quantum whose free-running phase is a multiple
// of four (@0x52BA47/@0x52BA6B). Time is conserved through the residual and
// the cross-frame phase, and a stall is followed by the EMA's geometric
// catch-up — the witnessed retail post-hitch fast-forward.
//
// The same frame feeds the FR counter [orig: Game_MainLoop @0x52B8ED..0x52B98F]:
// every frame counts once and adds its smoothed bank; once the sum reaches
// 2000 ms the average frame rate becomes frames * 16000 / sum and both
// counters restart. The smoothed bank still holds the previous frame's
// sub-4 ms residual, so a steady 16 ms stream reads 58, not 62. The rate reads
// 0 from the mode-init reset until the first full window.
class TickAccumulator {
public:
	// 16 ms; matches AiEventQueue::kFrameDt.
	static constexpr double kTickDt = 1.0 / io::kTickHz;

	// The retail bank's fixed point: 16 units per millisecond.
	static constexpr int32_t kUnitsPerMs = 16;
	// The 4 ms drain quantum and the 500 ms bank clamp, in bank units.
	static constexpr int32_t kQuantumUnits = 4 * kUnitsPerMs;
	static constexpr int32_t kBankClampUnits = 500 * kUnitsPerMs;
	// The most logic ticks one frame can run: a clamped 500 ms bank drained in
	// 4 ms quanta from a tick-aligned phase (125 quanta, 32 on the phase).
	static constexpr int kRetailMaxCatchupTicks = 32;
	// The FR counter's window, 2000 ms of summed smoothed bank (`cmp ebx,
	// 7D00h` @0x52B91A), and one second in bank units (`imul eax, 3E80h`
	// @0x52B957).
	static constexpr uint32_t kFrameRateWindowUnits = 2000u * kUnitsPerMs;
	static constexpr uint32_t kUnitsPerSecond = 1000u * kUnitsPerMs;

	// Bank `delta` seconds of wall-clock; returns the logic ticks due now
	// (>= 0). The sub-quantum remainder stays banked.
	int bank(double delta);

	// Discard banked wall-clock, the smoothing history and the FR counter
	// (Play/Step/Stop transitions, the mode-init reset @0x52B715..0x52B757), so
	// resuming does not burst-catch-up across a pause or load.
	void reset() {
		bank_ = kInitialBankUnits;
		smoothed_ = 0;
		phase_ = 0;
		carry_ = 0.0;
		average_fps_ = 0;
		frame_count_ = 0;
		frame_time_sum_ = 0;
	}

	// The banked residual in seconds.
	double banked() const { return static_cast<double>(bank_) / (1000.0 * kUnitsPerMs); }

	// The FR counter's average frame rate (g_statsAvgFps): the frame-pressure
	// input of both CNetQuality windows and the host's 0x0A server-fps byte.
	int32_t average_fps() const { return average_fps_; }

private:
	// Game_MainLoop seeds the bank with one millisecond (`esi = 16` @0x52B63C).
	static constexpr int32_t kInitialBankUnits = 16;

	int32_t bank_ = kInitialBankUnits;   // esi: residual + this frame's elapsed, smoothed
	int32_t smoothed_ = 0;               // g_frameTimeSmoothedFp4
	uint8_t phase_ = 0;                  // g_tickPhase (a byte, free-running)
	double carry_ = 0.0;                 // sub-unit wall-clock remainder
	int32_t average_fps_ = 0;            // g_statsAvgFps
	uint32_t frame_count_ = 0;           // g_statsFrameCount
	uint32_t frame_time_sum_ = 0;        // g_statsFrameTimeSumFp4
};

} // namespace opennova::world
