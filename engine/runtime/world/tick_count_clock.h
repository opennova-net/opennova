#pragma once

// The main loop's millisecond clock (D-NET-1132). The game's frame banks GetTickCount: the
// elapsed time since the previous frame's stamp, its frame lock's reads after each Sleep(1), and
// the stamp the mode's initialize takes. GetTickCount moves only on the system timer's
// interrupt, 1/64 s by default (timeBeginPeriod does not change it), so the bank grows in 15 and
// 16 ms steps, a lock short of 16 ms after a 15 ms step sleeps on to the next one, and the logic
// updates (and the host's per-tick sends) fall on those steps.
// [orig: Game_MainLoop @0x52B630 - GetTickCount @0x52B75C (the initialize's stamp) and @0x52B798
//  (the frame's elapsed, the add @0x52B7B2), the frame lock's read @0x52B8B8 after Sleep(1)
//  @0x52B8B2]
// It is the process's one clock: the protocol reads the same GetTickCount (the host's
// inmatch::host_monotonic_milliseconds, the joiner's connection, the NovaWorld session), as do
// the main loop's CPU share and its mission-start re-base.

#include <cstdint>

namespace opennova::world {

// Windows' default system-timer interval, 1/64 s, in microseconds: GetTickCount's step.
inline constexpr uint64_t kSystemTimerIntervalUs = 15625;

// The millisecond count GetTickCount64 shows at a monotonic reading of `us` microseconds: the
// whole milliseconds of the last timer interrupt at or before it (0, 15, 31, 46, 62, ...: steps
// of 15 or 16 ms, 64 of them a second).
constexpr uint64_t tick_count_ms64_from_us(uint64_t us) {
	return (us / kSystemTimerIntervalUs) * kSystemTimerIntervalUs / 1000u;
}

// The same count as GetTickCount shows it: its low 32 bits.
constexpr uint32_t tick_count_ms_from_us(uint64_t us) {
	return static_cast<uint32_t>(tick_count_ms64_from_us(us));
}

// The main loop's clock now, never 0 (0 is an embedder's "no stamp"): GetTickCount itself on
// Windows; elsewhere (the Linux serve app) tick_count_ms_from_us over the monotonic clock, the
// same steps on the same interval.
uint32_t frame_tick_count_ms();

// The same clock at 64 bits, never 0, for the readers that keep 64-bit deadlines (the joiner's
// connection): GetTickCount64 on Windows, whose low 32 bits are GetTickCount; elsewhere
// tick_count_ms64_from_us over the monotonic clock.
uint64_t frame_tick_count_ms64();

} // namespace opennova::world
