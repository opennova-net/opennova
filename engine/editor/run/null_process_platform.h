#pragma once

#include <chrono>
#include <cstdint>
#include <thread>

#include <editor/run/process_platform.h>

namespace opennova::editor {

// The process seam with no processes (ADR 0046 S13 A7): what a headless session runs over where no
// game is played (opennova-project, the command line). Nothing spawns (can_spawn is false, so Play
// says so and never tries), no child ever runs, a game a Play lease names is Unknown (kept, as a
// caller about to delete something treats one), and the clock is the machine's steady clock.
class NullProcessPlatform : public ProcessPlatform {
public:
	bool can_spawn() const override { return false; }
	int64_t spawn(const LaunchPlan &) override { return -1; }
	bool is_running(int64_t) override { return false; }
	bool terminate(int64_t) override { return true; }
	bool kill(int64_t) override { return true; }
	void release(int64_t) override {}
	int64_t now_ms() override {
		return std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch())
				.count();
	}
	void sleep_ms(int64_t ms) override {
		std::this_thread::sleep_for(std::chrono::milliseconds(ms > 0 ? ms : 0));
	}
};

} // namespace opennova::editor
