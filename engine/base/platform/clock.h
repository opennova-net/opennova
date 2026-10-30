#pragma once

#include <cstdint>

// The platform's time and its pump (ADR 0077 d2: the engine's platform API, backend-neutral): a
// monotonic clock, a sleep, and the pump the engine's loop calls where the OS's events are to be
// collected. The loop itself and every rule over this time are the engine's: retail's main loop
// banks GetTickCount, whose system-timer steps (15 and 16 ms) the engine derives from these
// microseconds (runtime/world/tick_count_clock.h frame_tick_count_ms over a Clock), its frame lock
// sleeps 1 ms at a time (inmatch::PlatformFrameLockClock), and it pumps the window's messages at
// its own points (inmatch::TickInputSource carries those witnesses).
//
// The design follows SDL3's (SDL_GetTicksNS, SDL_Delay, SDL_PumpEvents), GLFW's (glfwGetTime,
// glfwPollEvents) and sokol's (sokol_time's stm_now; sokol_app collects its events ahead of each
// frame callback): the time is the backend's, read on demand; the pump never blocks.
namespace opennova::platform {

class Clock {
public:
	virtual ~Clock() = default;

	// Microseconds on a monotonic clock from an origin of the backend's (its start): never
	// stepping back, never moved by the wall clock.
	virtual uint64_t now_us() const = 0;
	// The same clock in whole milliseconds.
	uint64_t now_ms() const { return now_us() / 1000u; }

	// Blocks the calling thread for at least `ms` milliseconds (Sleep, SDL_Delay).
	virtual void sleep_ms(uint32_t ms) = 0;

	// The frame pump entry: the OS's pending events collected without waiting, each into the
	// device that reports it (the window's state and close request, the input device's queue,
	// in the order the OS gave them). The engine's loop calls it where retail's pumps its window
	// messages.
	virtual void pump_events() = 0;
};

} // namespace opennova::platform
