#pragma once

// The opt-in profiling clock shared by every perf-attribution seam (logic
// tick, AI, collision, replication fan, client frame, menu video): one
// steady_clock read in microseconds. Production paths never call it unless a
// caller handed them a perf record (nullptr = no clock reads).

#include <chrono>
#include <cstdint>

namespace opennova::io {

inline uint64_t perf_now_us() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
}

} // namespace opennova::io
