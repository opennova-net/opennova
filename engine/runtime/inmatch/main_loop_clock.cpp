#include <runtime/inmatch/main_loop_clock.h>

#include <base/platform/clock.h>
#include <formats/gamecfg/game_cfg.h>
#include <runtime/inmatch/session.h> // FrameInput
#include <runtime/world/tick_count_clock.h>

#include <chrono>
#include <thread>

namespace opennova::inmatch {

namespace {

// Retail's Sleep(1) and its GetTickCount, the tick count with the system timer's steps (engine
// world/tick_count_clock.h carries the witness, D-NET-1132).
class TickCountFrameLockClock final : public world::FrameLockClock {
public:
	void sleep_one_ms() override { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
	uint32_t now_ms() override { return world::frame_tick_count_ms(); }
};

} // namespace

world::FrameLockClock &main_loop_tick_count_clock() {
	static TickCountFrameLockClock clock;
	return clock;
}

// [orig: Game_MainLoop @0x52B630 - Sleep(1) @0x52B8B2, GetTickCount @0x52B8B8]
void PlatformFrameLockClock::sleep_one_ms() {
	clock_.sleep_ms(1);
}
uint32_t PlatformFrameLockClock::now_ms() {
	return world::frame_tick_count_ms(clock_);
}

// [orig: Game_MainLoop @0x52B86C (the lock word's read), g_FrameLockActive @0x52B876]
bool main_loop_frame_lock(const gamecfg::GameCfg *cfg) {
	return cfg != nullptr && cfg->lock_framerate != 0;
}

MainLoopClock::MainLoopClock(world::FrameLockClock *clock)
		: clock_(clock != nullptr ? clock : &main_loop_tick_count_clock()) {}

// [orig: Game_MainLoop @0x52B798..0x52B7B4 (the elapsed read and the stamp)]
double MainLoopClock::take_frame_elapsed_seconds() {
	const uint32_t now = clock_->now_ms();
	const uint32_t elapsed_ms = stamp_ms_ != 0 ? now - stamp_ms_ : 0u;
	// The tick count is never 0 (world::frame_tick_count_ms), and a test clock that reads 0
	// stamps as 1, so a stamp is always distinguishable from none.
	stamp_ms_ = now != 0 ? now : 1u;
	return static_cast<double>(elapsed_ms) / 1000.0;
}

void MainLoopClock::stamp_frame_input(FrameInput &input, const gamecfg::GameCfg *cfg) const {
	input.frame_stamp_ms = stamp_ms_;
	input.frame_lock = main_loop_frame_lock(cfg);
	input.since_render_seconds = seconds_since_render();
}

FrameInput MainLoopClock::begin_frame(const gamecfg::GameCfg *cfg) {
	FrameInput input;
	input.delta_seconds = take_frame_elapsed_seconds();
	stamp_frame_input(input, cfg);
	return input;
}

void MainLoopClock::end_frame(uint32_t outcome_frame_stamp_ms) {
	if (outcome_frame_stamp_ms != 0) stamp_ms_ = outcome_frame_stamp_ms;
}

void MainLoopClock::mark_rendered() {
	const uint32_t now = clock_->now_ms();
	render_ms_ = now != 0 ? now : 1u;
}

double MainLoopClock::seconds_since_render() const {
	if (render_ms_ == 0) return -1.0;
	return static_cast<double>(clock_->now_ms() - render_ms_) / 1000.0;
}

} // namespace opennova::inmatch
