#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#include <editor/run/behind_start.h>
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
	// The child's image as the OS reports it (QueryFullProcessImageNameW: a link or an alias
	// resolved) and its creation time (GetProcessTimes, the FILETIME's 100 ns count in decimal),
	// read through the handle spawn kept.
	bool process_identity(int64_t pid, opennova::editor::ProcessIdentity &out) override;
	// A game this platform no longer holds (a Play lease's, across an editor restart), opened by
	// its pid for PROCESS_QUERY_LIMITED_INFORMATION alone (which an elevated game still grants):
	// Dead when no process of that id runs, it has exited, or its creation time is not the lease's
	// (a recycled pid, a reboot's); Alive when it is; Unknown when it will not be opened, or the
	// lease records no time.
	opennova::editor::ProcessLiveness process_liveness(int64_t pid, const std::string &created) override;
	// The game install's run-one-at-a-time gate (OpenSemaphoreW by its name): Alive when it is there,
	// Dead when no such semaphore is (ERROR_FILE_NOT_FOUND), Unknown on any other failure.
	opennova::editor::ProcessLiveness semaphore_held(const std::string &name) override;
	int64_t now_ms() override;
	void sleep_ms(int64_t ms) override;

	// A child spawned behind (LaunchPlan::behind, the MCP gaps lane: play {behind}) kept behind while it
	// starts, as opennova::editor::BehindStarts decides (editor/run/behind_start.h): its first window was shown
	// without activation (STARTUPINFO SW_SHOWNOACTIVATE) and the foreground locked at the spawn
	// (LockSetForegroundWindow, refused harmlessly when the editor holds no foreground rights, when the child gets
	// none either), the lock let go as soon as the child's first window has been sent back (or kLockMs passed);
	// each call (the Shell's pump) sends its shown windows to the bottom of the z-order without activation and
	// stops their taskbar flashing, until one is the foreground window (left alone from then on) or kTendMs.
	void tend();

private:
	std::mutex mutex_;
	std::unordered_map<int64_t, void *> children_; // pid -> process handle
	opennova::editor::BehindStarts behind_;        // the children started behind, tended
	bool locked_ = false;                          // the foreground lock is held
};

} // namespace godot
