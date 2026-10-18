#pragma once

// What a supervised run of opennova-serve leaves its supervisor (a hosted
// server's machine agent, apps/hostd) in its working directory, neither of it
// retail's: the run's tick health (--stats-file), rewritten once a minute, and
// the line it exits with (--exit-file), written at a non-zero exit.
//
// Tick health. The loop wakes once per logic quantum (62.5 Hz, main.cpp); each
// frame's time is its work, from its start to the loop's sleep. A frame is late
// when it starts past its own end's deadline: the loop is a whole quantum
// behind (a slow frame before it, or a machine that gave the server no CPU).
// Every kWindowMs the window's frames are summed up as their p50, p99 and
// longest frame time and the late frames, and the window starts over. The
// times are the caller's steady clock in nanoseconds, so a test steps them by
// hand. Instrumentation only: the loop runs as it did. Service policy, not a
// port.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace opennova::serve {

// One window of frames, summed up.
struct TickMinute {
	double p50_ms = 0.0;
	double p99_ms = 0.0;
	double max_ms = 0.0;
	uint64_t late = 0;
	uint64_t frames = 0;
};

class TickStats {
public:
	static constexpr int64_t kWindowNs = 60LL * 1000 * 1000 * 1000;

	// A frame that started at `start_ns`, ended its work at `end_ns`, and was
	// due to end by `deadline_ns` (late when it started past that).
	void frame(int64_t start_ns, int64_t end_ns, int64_t deadline_ns);
	// The window's summary once kWindowNs have passed since its first frame at
	// `now_ns`, and a new window begun; nullopt before that (or with no frame).
	std::optional<TickMinute> take_if_due(int64_t now_ns);

private:
	std::vector<double> frame_ms_;
	uint64_t late_ = 0;
	std::optional<int64_t> window_start_ns_;
};

// The nearest-rank `q` quantile (0 < q <= 1) of `values` (reordered); 0 for none.
double quantile(std::vector<double> &values, double q);

// The --stats-file's text, one JSON line: {"at": <Unix seconds>, "p50_ms",
// "p99_ms", "max_ms", "late", "frames"} (the times to 0.01 ms).
std::string tick_minute_json(const TickMinute &minute, int64_t at_unix);

// The --exit-file's text: `error` as one line (every control character a
// space), newline-terminated.
std::string exit_file_text(const std::string &error);

} // namespace opennova::serve
