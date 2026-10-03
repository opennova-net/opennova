// The calling thread's CPU time, for a test that bounds what its code costs rather than how long the
// machine took to run it: another process's load (ctest -j8, a build beside it) stops the thread and
// stretches the wall clock, never this clock. Kernel and user time both count (a file read's system
// call is the code's cost; waiting on the disk is not). Windows advances it at the scheduler's tick
// (kThreadCpuTickMs), so a bound over it leaves a tick of room each way; POSIX counts it exactly.
#pragma once

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <time.h>
#endif

#include <cstdint>

namespace test_clock {

#if defined(_WIN32)
inline constexpr double kThreadCpuTickMs = 15.625;
#else
inline constexpr double kThreadCpuTickMs = 0.0;
#endif

// Milliseconds of CPU the calling thread has used since it started.
inline double thread_cpu_ms() {
#if defined(_WIN32)
	FILETIME created{}, exited{}, kernel{}, user{};
	if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return 0.0;
	const auto hundred_ns = [](const FILETIME &time) {
		return (uint64_t(time.dwHighDateTime) << 32) | uint64_t(time.dwLowDateTime);
	};
	return double(hundred_ns(kernel) + hundred_ns(user)) / 10000.0;
#else
	timespec now{};
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
	return double(now.tv_sec) * 1000.0 + double(now.tv_nsec) / 1e6;
#endif
}

} // namespace test_clock
