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

// How banked wall-clock becomes logic ticks.
//
// WallClock is the SHELL's bank and the default: bank seconds, drain them in
// kTickDt quanta, and clamp a hitch's backlog to kMaxCatchupTicks — dropping
// the remainder so a load stall cannot spiral catch-up into the following
// frames. It is frame-rate independent and drops a > 500 ms backlog.
//
// RetailMainLoop is the witnessed retail bank [orig: Game_MainLoop @0x52B630]:
// its 7/8 EMA low-pass filters the banked time, so a stall is followed by a
// geometric fast-forward over the next frames instead of a dropped backlog.
// The dedicated host (apps/nw_server) selects it. Flipping the shell to it
// changes player-visible pacing after every load or shader hitch, so that is
// a maintainer decision, not a default.
enum class TickBankPolicy : uint8_t {
	WallClock,
	RetailMainLoop,
};

// The fixed-62.5 Hz real-time accumulator. The embedder runs the returned
// number of logic ticks and presents once after the batch; a zero return still
// presents render-only state (camera and attach can change between fixed
// ticks).
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
class TickAccumulator {
public:
	// 16 ms; matches AiEventQueue::kFrameDt.
	static constexpr double kTickDt = 1.0 / io::kTickHz;
	// WallClock's spiral-of-death clamp: the 500 ms / 16 ms accumulator cap.
	static constexpr int kMaxCatchupTicks = 31;

	// The retail bank's fixed point: 16 units per millisecond.
	static constexpr int32_t kUnitsPerMs = 16;
	// The 4 ms drain quantum and the 500 ms bank clamp, in bank units.
	static constexpr int32_t kQuantumUnits = 4 * kUnitsPerMs;
	static constexpr int32_t kBankClampUnits = 500 * kUnitsPerMs;
	// The most logic ticks one RetailMainLoop frame can run: a clamped 500 ms
	// bank drained in 4 ms quanta from a tick-aligned phase (125 quanta, 32 on
	// the phase).
	static constexpr int kRetailMaxCatchupTicks = 32;

	TickBankPolicy policy() const { return policy_; }
	// Selecting a policy discards whatever the other one had banked.
	void set_policy(TickBankPolicy policy) {
		if (policy == policy_) return;
		policy_ = policy;
		reset();
	}

	// Bank `delta` seconds of wall-clock; returns the logic ticks due now
	// (>= 0). The sub-quantum remainder stays banked; WallClock drops a clamped
	// backlog.
	int bank(double delta);

	// Discard banked wall-clock and the smoothing history (Play/Step/Stop
	// transitions, the mode-init reset @0x52B715..0x52B757), so resuming does
	// not burst-catch-up across a pause or load.
	void reset() {
		accum_ = 0.0;
		bank_ = kInitialBankUnits;
		smoothed_ = 0;
		phase_ = 0;
		carry_ = 0.0;
	}

	// The banked residual in seconds.
	double banked() const {
		return policy_ == TickBankPolicy::WallClock
				? accum_
				: static_cast<double>(bank_) / (1000.0 * kUnitsPerMs);
	}

private:
	int bank_wall_clock(double delta);
	int bank_retail_main_loop(double delta);

	// Game_MainLoop seeds the bank with one millisecond (`esi = 16` @0x52B63C).
	static constexpr int32_t kInitialBankUnits = 16;

	TickBankPolicy policy_ = TickBankPolicy::WallClock;
	double accum_ = 0.0;                 // WallClock: banked seconds
	int32_t bank_ = kInitialBankUnits;   // esi: residual + this frame's elapsed, smoothed
	int32_t smoothed_ = 0;               // g_frameTimeSmoothedFp4
	uint8_t phase_ = 0;                  // g_tickPhase (a byte, free-running)
	double carry_ = 0.0;                 // sub-unit wall-clock remainder
};

} // namespace opennova::world
