#include <runtime/world/tick_count_clock.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <chrono>
#endif

namespace opennova::world {

namespace {

#ifndef _WIN32
uint64_t monotonic_us() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
}
#endif

} // namespace

// The main loop's reads [orig: Game_MainLoop @0x52B630 - GetTickCount @0x52B798, @0x52B8B8].
uint32_t frame_tick_count_ms() {
#ifdef _WIN32
	const uint32_t now = static_cast<uint32_t>(GetTickCount());
#else
	const uint32_t now = tick_count_ms_from_us(monotonic_us());
#endif
	return now != 0 ? now : 1u;
}

uint64_t frame_tick_count_ms64() {
#ifdef _WIN32
	const uint64_t now = static_cast<uint64_t>(GetTickCount64());
#else
	const uint64_t now = tick_count_ms64_from_us(monotonic_us());
#endif
	return now != 0 ? now : 1u;
}

} // namespace opennova::world
