#pragma once

#include <cstdint>
#include <base/io/tick_rate.h>

namespace opennova::world {

// The ONE engine tick cadence: 62.5 Hz — every fourth 4 ms drain quantum of
// Game_MainLoop runs the logic update, advancing the logic tick counter once
// per 16 ms [orig: current_tick @ 0x24c1968, incremented per drained 16 ms
// quantum in Game_MainLoop @ 0x52b630 -> Game_ProcessMainFrame @ 0x5263f0]: the
// logic dispatches independently of the variable render rate, several ticks on
// a long frame and none on a short one.

// Wall-clock milliseconds -> whole logic ticks (the original's ms/16 quantum
// count; fractional remainders truncate exactly like the drain).
constexpr int32_t ticks_from_ms(int64_t ms) {
    return static_cast<int32_t>(ms / io::kTickMs);
}

// The frame lock's platform seam: retail's Sleep(1) and its GetTickCount read
// (the embedder's millisecond clock, the one its frame stamp reads).
class FrameLockClock {
public:
	virtual ~FrameLockClock() = default;
	virtual void sleep_one_ms() = 0;
	virtual uint32_t now_ms() = 0;
};

// One frame's lock (game.cfg lock_framerate set): the clock, and the frame's
// start stamp in its milliseconds, which each sleep's read replaces (the lock's
// last read is the stamp the next frame banks from). `sleeps` is what it slept.
struct FrameLock {
	FrameLockClock *clock = nullptr;
	uint32_t stamp_ms = 0;
	int sleeps = 0;
};

// The fixed-62.5 Hz real-time accumulator: retail's frame-time bank, the one
// the game (Simulation) banks through. Its 7/8 EMA low-pass filters the banked time, so a long frame's
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
// 0 from the mode-init reset until the first full window. The same window keeps the
// longest and the shortest smoothed bank it counted, which publish as the minimum and
// maximum frame rates (frame_rate_stats).
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
	// (>= 0). The sub-quantum remainder stays banked. `delta` is the frame's
	// elapsed wall time since the previous frame's start stamp, as the embedder
	// measures it (the Godot shell's GameWorld::take_frame_elapsed_seconds, the
	// headless server's steady clock), never an engine step capped below it
	// (D-NET-1003) [orig: Game_MainLoop @0x52B630 (the elapsed GetTickCount()
	//  - the previous stamp @0x52B798..0x52B7A4, the stamp the next frame
	//  subtracts @0x52B7B4)]. With a `lock` (game.cfg lock_framerate set, the
	// config's default 1 [orig: Config_SetDefaults @0x54D12F]) a frame that
	// banked under 16 ms after the smoothing sleeps 1 ms at a time, each sleep
	// adding the milliseconds since the last stamp, until 16 ms stand banked or
	// 500 sleeps ran; the smoothing history keeps the value the EMA stored, the
	// FR counter and the drain take the locked bank, and the lock's last read is
	// the next frame's stamp (FrameLock::stamp_ms) [orig: Game_MainLoop - the
	// word read @0x52B860 ahead of the history's store @0x52B866, g_FrameLockActive
	// @0x52B876, the unsigned test @0x52B8A4..0x52B8A6, the 500 cap @0x52B8A8..0x52B8AE, Sleep(1)
	// @0x52B8B2, the add @0x52B8B8..0x52B8D0, the restamps @0x52B8D7 / @0x52B8DB,
	// the loop test @0x52B8DF; the cine fixed step's skip @0x52B89D is not
	// modeled, as the drain's]. The embedder's
	// stamp restarts at a mission start
	// (the shell zeroes it when a mission's world unloads): the mission start is
	// the game mode's initialize, after which the loop re-reads its clock, so the
	// first frame never banks the gap since the previous mission [orig:
	// SinglePlayer_StartMission @0x561AF0 (the g_GameModeGameLoop push
	// @0x561E80..0x561E85); GameLoop_Init @0x526370 (the Game_StartMission call
	// @0x526377); Game_MainLoop @0x52B630 (the initialize @0x52B70D, GetTickCount
	// @0x52B75C into the stamp @0x52B766)].
	int bank(double delta, FrameLock *lock = nullptr);

	// The lock's bank floor, 16 ms (`shl eax, 8` @0x52B89F), and its sleep cap
	// (`cmp edi, 1F4h` @0x52B8A8).
	static constexpr int32_t kFrameLockUnits = 16 * kUnitsPerMs;
	static constexpr int kFrameLockMaxSleeps = 500;

	// Discard banked wall-clock, the smoothing history and the FR counter
	// (Play/Step/Stop transitions, the mode-init reset @0x52B715..0x52B757), so
	// resuming does not burst-catch-up across a pause or load.
	void reset() {
		bank_ = kInitialBankUnits;
		smoothed_ = 0;
		phase_ = 0;
		carry_ = 0.0;
		average_fps_ = 0;
		min_fps_ = 0;
		max_fps_ = 0;
		frame_count_ = 0;
		frame_time_sum_ = 0;
		cpu_percent_ = 0;
		update_ms_ = 0;
		update_count_ = 0;
	}

	// Scale the smoothing history, the value the next frame's EMA reads: retail's
	// one live writer outside the main loop (Game_ClearSmoothedFrameTime @0x52B620
	// zeroes it too, but nothing calls it), called by the lose cine's edit fade
	// (world::EpilogCine) from inside the drain's logic update, so the frame
	// that called it keeps its ticks and the next frame banks against the
	// scaled history. The word loads unsigned, multiplies by the float in x87
	// precision and stores truncated toward zero (the 0xC00 rounding control).
	// [orig: Game_ScaleSmoothedFrameTime @0x52B5D0 (the unsigned load
	//  @0x52B5D5..0x52B5E2, the multiply @0x52B5E8, the truncating store
	//  @0x52B5EC..0x52B606); its callers CCineEventEditFade_Start @0x5726B0 (the
	//  call @0x5726C9) and CCineEventEditFade_Finish @0x572750 (the call
	//  @0x572775); the EMA's read Game_MainLoop @0x52B845]
	void scale_smoothing_history(float factor);

	// The banked residual in seconds.
	double banked() const { return static_cast<double>(bank_) / (1000.0 * kUnitsPerMs); }

	// The FR counter's average frame rate (g_StatsAvgFps): the frame-pressure
	// input of both CNetQuality windows and the host's 0x0A server-fps byte.
	int32_t average_fps() const { return average_fps_; }

	// The CPU share the FR window publishes beside the frame rate: every frame
	// adds the wall milliseconds from its start stamp to the end of its logic
	// drain and counts the logic updates that drain ran; when the window
	// closes (in bank) the share becomes ms * 100 / (max(updates, 1) * 16)
	// and both counters restart. It reads 0 until the first full window.
	// [orig: Game_MainLoop — the update count `add g_StatsUpdateCount, 1`
	//  @0x52ba4f, `g_StatsUpdateMs += GetTickCount() - frame start`
	//  @0x52ba9b..0x52baa1, the publish @0x52b926..0x52b948 (a zero count reads
	//  1 @0x52b930), the restarts @0x52b96f / @0x52b975; the mode-init reset
	//  @0x52b72d / @0x52b733 / @0x52b74b]
	void record_frame_work(uint32_t elapsed_ms, int updates) {
		update_ms_ += elapsed_ms;
		update_count_ += static_cast<uint32_t>(updates > 0 ? updates : 0);
	}
	int32_t cpu_percent() const { return cpu_percent_; }

	// The window's frame-rate extrema beside its average: every frame's smoothed bank, after
	// it is counted, raises the longest (unsigned) and lowers the shortest; when the window
	// closes the maximum rate becomes 16000 / shortest (a shortest of 0 reads 1) and the
	// minimum 16000 / longest, and the two restart at 2000 ms and 1. Unlike the counters,
	// the extrema have no mode-init reset: their first window starts from the process's
	// statics, 125 ms and 1, and the published rates read 0 from the mode-init reset until
	// the first full window. The frame-rate overlay draws the four (hud::HudFrameRateOverlay).
	// [orig: Game_MainLoop @0x52B630 - the longest `cmp esi, ebp; jbe` @0x52B8F6 and its
	//  store @0x52B902, the shortest `cmp esi, ecx; jnb` @0x52B90E and its store @0x52B914,
	//  the zero shortest's 1 @0x52B946..0x52B94F, the restarts @0x52B97B / @0x52B985, the
	//  publishes g_StatsMaxFps @0x52B99D and g_StatsMinFps @0x52B9A9 (`mov eax, 3E80h; div`
	//  @0x52B994..0x52B999 / @0x52B9A2..0x52B9A7); the mode-init reset zeroes the rates
	//  alone @0x52B739 / @0x52B73F; the statics g_StatsMinFrameTimeFp4 @0x83016C = 2000 and
	//  g_StatsMaxFrameTimeFp4 @0x830170 = 1]
	struct FrameRateStats {
		int32_t average_fps = 0; // g_StatsAvgFps
		int32_t min_fps = 0;     // g_StatsMinFps
		int32_t max_fps = 0;     // g_StatsMaxFps
		int32_t cpu_percent = 0; // g_StatsCpuPercent
	};
	FrameRateStats frame_rate_stats() const {
		return {average_fps_, min_fps_, max_fps_, cpu_percent_};
	}

private:
	// Game_MainLoop seeds the bank with one millisecond (`esi = 16` @0x52B63C).
	static constexpr int32_t kInitialBankUnits = 16;
	// The shortest frame's static, 125 ms, and its window restart, 2000 ms.
	static constexpr uint32_t kShortestFrameStatic = 2000;
	static constexpr uint32_t kShortestFrameRestart = 2000u * kUnitsPerMs;

	int32_t bank_ = kInitialBankUnits;   // esi: residual + this frame's elapsed, smoothed
	int32_t smoothed_ = 0;               // g_FrameTimeSmoothedFp4
	uint8_t phase_ = 0;                  // g_TickPhase (a byte, free-running)
	double carry_ = 0.0;                 // sub-unit wall-clock remainder
	int32_t average_fps_ = 0;            // g_StatsAvgFps
	int32_t min_fps_ = 0;                // g_StatsMinFps
	int32_t max_fps_ = 0;                // g_StatsMaxFps
	uint32_t shortest_frame_ = kShortestFrameStatic; // g_StatsMinFrameTimeFp4
	uint32_t longest_frame_ = 1;         // g_StatsMaxFrameTimeFp4
	uint32_t frame_count_ = 0;           // g_StatsFrameCount
	uint32_t frame_time_sum_ = 0;        // g_StatsFrameTimeSumFp4
	int32_t cpu_percent_ = 0;            // g_StatsCpuPercent
	uint32_t update_ms_ = 0;             // g_StatsUpdateMs
	uint32_t update_count_ = 0;          // g_StatsUpdateCount
};

} // namespace opennova::world
