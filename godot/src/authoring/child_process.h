#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#include <editor/run/process_platform.h>

namespace godot {

// The OS under the editor's Play session (ADR 0046 d8): the process seam
// implemented over the platform's process API. Spawns the game with the build
// directory as its working directory and keeps the process HANDLE until release(),
// so every later question about the child goes through that handle and a pid the
// OS recycled to an unrelated process can never be mistaken for it. Spawn, liveness,
// stop and kill all come from this one place: mixing Godot's own process helpers with
// a foreign child once reported "stopped" while the child kept running.
//
// Windows only for now: the only consumer is a Windows game binary, and a spawn that
// silently ignored the working directory would reproduce exactly the bug the
// handle-keeping design exists to fix. Elsewhere can_spawn() is false and Play says
// that it is Windows-only.
class ChildProcessPlatform : public opennova::editor::ProcessPlatform {
public:
	~ChildProcessPlatform() override;

	bool can_spawn() const override;
	int64_t spawn(const opennova::editor::LaunchPlan &plan) override;
	bool is_running(int64_t pid) override;
	// Asks the child to close its window (the game's orderly quit); false when the
	// request could not be delivered to a child that still runs.
	bool terminate(int64_t pid) override;
	bool kill(int64_t pid) override;
	bool exit_code(int64_t pid, uint32_t &out) override;
	void release(int64_t pid) override;
	// A game this platform no longer holds (a Play lease's, across an editor restart): opened by
	// its pid, alive while it has not exited and its image's file name is the lease's executable's.
	bool process_alive(int64_t pid, const std::string &executable) override;
	int64_t now_ms() override;
	void sleep_ms(int64_t ms) override;

private:
	std::mutex mutex_;
	std::unordered_map<int64_t, void *> children_; // pid -> process handle
};

} // namespace godot
