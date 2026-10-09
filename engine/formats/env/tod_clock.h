#pragma once

#include <cstdint>

// The one runtime mission time-of-day clock: the 8.24 hour accumulator every
// 62 Hz tick advances by the exact integer rate
// [orig: g_EnvTodAdvancePerTick = 0x18000000 / (3720 * minutes_per_day)
//  @0x57d108, set by Environment_SetTodAdvanceRate @0x57d170 with the
//  60-minute day floor; the BMS header's Q8.8 start hour widens into the
//  accumulator at Game_StartMission @0x525371]. Pure integer math — the shell
//  keeps the HHMM display conversion and the preview scrub knob.

namespace opennova::env {

inline constexpr int32_t kTodHoursPerDay = 24;
inline constexpr int32_t kTodFixed24OneHour = 1 << 24;
// 24 h in 8.24 = 0x18000000, the witnessed dividend.
inline constexpr int32_t kTodDayFixed24 = kTodHoursPerDay * kTodFixed24OneHour;
inline constexpr int32_t kTodTicksPerRealMinute = 3720; // 60 s x 62 logic ticks
inline constexpr int32_t kTodMinMinutesPerDay = 60;     // [orig: @0x57d170 floor]
inline constexpr int32_t kTodQ8_8ToFixed24Shift = 16;

// The BMS mission header's unsigned Q8.8 start hour -> the 8.24 accumulator,
// day-wrapped [orig: @0x525371].
inline int32_t tod_start_fixed24(int32_t start_time_q8_8) {
	const int64_t raw = start_time_q8_8 & 0xFFFF;
	return static_cast<int32_t>(
			(raw << kTodQ8_8ToFixed24Shift) % kTodDayFixed24);
}

// A day length under retail's 60-minute floor runs as 60 [orig: @0x57d170].
inline int32_t tod_floored_minutes_per_day(int32_t minutes_per_day) {
	return minutes_per_day < kTodMinMinutesPerDay ? kTodMinMinutesPerDay : minutes_per_day;
}

// The exact per-tick increment [orig: @0x57d108]; the day length clamps to
// retail's 60-minute floor [orig: @0x57d170], and a day length of 0 is a
// clock that stands: no advance at all [orig: Environment_SetTodAdvanceRate
// @0x57d176 tests it first, @0x57d196 stores 0].
inline int32_t tod_advance_per_tick(int32_t minutes_per_day) {
	if (minutes_per_day == 0) return 0;
	return kTodDayFixed24 / (kTodTicksPerRealMinute * tod_floored_minutes_per_day(minutes_per_day));
}

// The advance an .env's tod_rate parses into, which has no zero test: the
// 60-minute floor alone [orig: TimeOfDay_ParseProperty @0x57d0e4..0x57d118].
inline int32_t tod_rate_advance_per_tick(int32_t minutes_per_day) {
	return tod_advance_per_tick(tod_floored_minutes_per_day(minutes_per_day));
}

// The advance Environment_InitDefaults leaves before any file or mission sets
// one: 75, about a 1440-minute day [orig: Environment_InitDefaults @0x57c010].
inline constexpr int32_t kTodDefaultAdvancePerTick = 75;

// Advance the accumulator by completed logic ticks, day-wrapped.
inline int32_t tod_advance(int32_t time_fixed24, int32_t ticks,
		int32_t advance_per_tick) {
	return static_cast<int32_t>(
			(static_cast<int64_t>(time_fixed24) +
					static_cast<int64_t>(ticks) * advance_per_tick) %
			kTodDayFixed24);
}

} // namespace opennova::env
