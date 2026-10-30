#pragma once

// THE MAIN LOOP'S CLOCK: the frame timing every embedder that runs a session's frames hands
// the session, kept once. The game's frame (GameWorld) and opennova-serve's loop both drive this
// type, so a Serve Only host's frames bank, lock and re-base as a listen host's do, as retail's
// one main loop runs both with no dedicated-host test.
//
// Retail's loop, per frame [orig: Game_MainLoop @0x52B630]:
//   - the frame's elapsed time is GetTickCount's difference from the previous frame's stamp,
//     and that read is the stamp the next frame subtracts (@0x52B798..0x52B7B4);
//   - game.cfg's lock_framerate word, read every frame, locks it (@0x52B86C..0x52B876): a frame
//     that banked under 16 ms sleeps 1 ms at a time, each sleep's GetTickCount the new stamp
//     (@0x52B8A6..0x52B8DF, the restamps @0x52B8D7 / @0x52B8DB). The session's bank runs the
//     sleeps (Session::advance, world::TickAccumulator::bank); this clock hands it the frame's
//     stamp and the lock word and takes the lock's last read back as the next stamp;
//   - the render callback's re-base reads the clock after the render (@0x52BAC8..0x52BAD2): a
//     mission-start frame banks only the time since then (FrameInput::since_render_seconds);
//   - a mode's initialize (the mission start, a map change's included) re-reads the clock once
//     it returns (@0x52B766), so the load's time is never banked: the embedder restarts this
//     clock across a load it runs outside its frames (GameWorld's unload), and the session's
//     re-base takes the time since the last render for a load run inside a frame (a headless
//     host's map change).
// The clock is the main loop's tick count (world/tick_count_clock.h, D-NET-1132) unless a test
// hands its own.

#include <runtime/world/tick_accumulator.h> // world::FrameLockClock

#include <cstdint>

namespace opennova::gamecfg {
struct GameCfg;
}

namespace opennova::platform {
class Clock;
}

namespace opennova::inmatch {

struct FrameInput;

// The main loop's frame lock primitives: Sleep(1) and GetTickCount, the tick count with the
// system timer's steps (world::frame_tick_count_ms). The session's frame lock and every
// MainLoopClock read them unless a test hands its own.
world::FrameLockClock &main_loop_tick_count_clock();

// The same primitives over a platform backend's clock (base/platform/clock.h): Sleep(1) as the
// platform's 1 ms sleep, GetTickCount as the tick count over its microseconds
// (world::frame_tick_count_ms(const platform::Clock &)). An embedder that runs on a platform hands
// it to its MainLoopClock and its Session alike, so the frame's stamp, its lock and its CPU share
// read one clock. `clock` outlives it.
// [orig: Game_MainLoop @0x52B630 - Sleep(1) @0x52B8B2, the frame lock's GetTickCount @0x52B8B8]
class PlatformFrameLockClock final : public world::FrameLockClock {
public:
	explicit PlatformFrameLockClock(platform::Clock &clock) : clock_(clock) {}
	void sleep_one_ms() override;
	uint32_t now_ms() override;

private:
	platform::Clock &clock_;
};

// Whether the process's cfg block locks the main loop's frames: its lock_framerate word, which
// the loop reads every frame, a Serve Only host's as a player's (Config_SetDefaults stores 1).
// No block (a test's, a tool's) runs no lock.
// [orig: Game_MainLoop @0x52B86C (g_GameConfigState.lockFrameRate_08C), g_FrameLockActive
//  @0x52B876; Config_SetDefaults @0x54D12F]
bool main_loop_frame_lock(const gamecfg::GameCfg *cfg);

class MainLoopClock {
public:
	// `clock` null: the main loop's tick count (main_loop_tick_count_clock).
	explicit MainLoopClock(world::FrameLockClock *clock = nullptr);

	// The frame's elapsed wall time since the stamp, in the clock's whole milliseconds, the
	// read now the stamp; 0 when there is none (the first frame, or the first past a restart,
	// only stamps). Never an engine step capped below it (D-NET-1003).
	// [orig: Game_MainLoop @0x52B798..0x52B7B4]
	double take_frame_elapsed_seconds();
	// The frame's timing on `input` past its delta: the stamp the frame lock measures from,
	// the cfg block's lock word (main_loop_frame_lock) and the time since the last render.
	void stamp_frame_input(FrameInput &input, const gamecfg::GameCfg *cfg) const;
	// One live frame's timing whole: take_frame_elapsed_seconds as the delta, then
	// stamp_frame_input.
	FrameInput begin_frame(const gamecfg::GameCfg *cfg);
	// The session's frame ran: the lock's last read when it slept (FrameOutcome::
	// frame_stamp_ms, 0 when it did not) is the stamp the next frame measures from
	// [orig: Game_MainLoop @0x52B8DB, `mov ebp, [esp+loop_tick]` @0x52B794].
	void end_frame(uint32_t outcome_frame_stamp_ms);
	// The render callback returned: the stamp a mission-start frame re-bases to
	// [orig: Game_MainLoop @0x52BAC8..0x52BAD2].
	void mark_rendered();
	// Seconds since the last render on the clock, in its steps; negative before the first.
	double seconds_since_render() const;
	// A mission's world went ahead of a load the embedder runs outside its frames: the next
	// frame only stamps, as the mode's initialize re-reads the clock once the load returns
	// [orig: Game_MainLoop @0x52B70D (the initialize), @0x52B766 (the read)].
	void restart() { stamp_ms_ = 0; }
	// The stamp the next frame measures from (0: none).
	uint32_t frame_stamp_ms() const { return stamp_ms_; }

private:
	world::FrameLockClock *clock_;
	uint32_t stamp_ms_ = 0;
	uint32_t render_ms_ = 0;
};

} // namespace opennova::inmatch
