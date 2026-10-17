#pragma once

// The steady clock hostd's timers read, in milliseconds.

#include <chrono>
#include <cstdint>

namespace opennova::hostd {

inline uint64_t steady_now_ms() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch())
					.count());
}

} // namespace opennova::hostd
