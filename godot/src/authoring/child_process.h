#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#include <base/proc/child_process.h>
#include <editor/run/behind_start.h>
#include <editor/run/process_platform.h>

namespace godot {

// The OS under the editor's Play session (ADR 0046 d8): the process seam over the engine's one child-process
// seam (base/proc's ChildProcess, started with no shell between and its handle kept), so every later question
// about the child goes through what spawn kept and a pid the OS recycled to an unrelated process can never be
// mistaken for it. Spawn, liveness, stop and kill all come from that one place: mixing Godot's own process
// helpers with a foreign child once reported "stopped" while the child kept running.
//
// The game outlives the editor (a Play lease names it across an editor restart, run/play_lease.h): it is
// started not to end with this process, and the editor letting go of it, at release() or as it closes, never
// ends it. What stays here is the editor's own: the game install's run-one-at-a-time gate and the tending of a
// child started behind (Windows').
class ChildProcessPlatform : public opennova::editor::ProcessPlatform {
public:
	~ChildProcessPlatform() override;

	// Where base/proc starts a process: Windows and POSIX.
	bool can_spawn() const override;
	int64_t spawn(const opennova::editor::LaunchPlan &plan, std::string &error) override;
	bool is_running(int64_t pid) override;
	// Asks the child to close its window (the game's orderly quit; SIGTERM on POSIX), and nothing more: a child
	// whose window is not up yet is left to Stop's deadline, whose kill ends it. True when the request was
	// delivered or the child had already exited.
	bool terminate(int64_t pid) override;
	bool kill(int64_t pid) override;
	bool exit_code(int64_t pid, uint32_t &out) override;
	void release(int64_t pid) override;
	// The child's image and creation time as base/proc reads them at spawn (ProcessIdentity).
	bool process_identity(int64_t pid, opennova::editor::ProcessIdentity &out) override;
	// A game this platform no longer holds (a Play lease's, across an editor restart): base/proc's
	// process_liveness.
	opennova::editor::ProcessLiveness process_liveness(int64_t pid, const std::string &created) override;
	// The game install's run-one-at-a-time gate (OpenSemaphoreW by its name): Alive when it is there,
	// Dead when no such semaphore is (ERROR_FILE_NOT_FOUND), Unknown on any other failure and off Windows.
	opennova::editor::ProcessLiveness semaphore_held(const std::string &name) override;
	int64_t now_ms() override;
	void sleep_ms(int64_t ms) override;

	// A child spawned behind (LaunchPlan::behind, the MCP gaps lane: play {behind}) kept behind while it
	// starts, as opennova::editor::BehindStarts decides (editor/run/behind_start.h): its first window was shown
	// without activation (base/proc's `behind`) and the foreground locked at the spawn (LockSetForegroundWindow,
	// refused harmlessly when the editor holds no foreground rights, when the child gets none either), the lock
	// let go as soon as the child's first window has been sent back (or kLockMs passed); each call (the Shell's
	// pump) sends its shown windows to the bottom of the z-order without activation and stops their taskbar
	// flashing, until one is the foreground window (left alone from then on) or kTendMs. Windows only.
	void tend();

private:
	// The child spawn started under `pid` (the id it had then: base/proc's reads 0 once it is reaped), or null.
	opennova::proc::ChildProcess *child_of(int64_t pid);
	// Whether the child has exited (or is not held).
	bool exited(int64_t pid);

	std::mutex mutex_;
	std::unordered_map<int64_t, opennova::proc::ChildProcess> children_;
	opennova::editor::BehindStarts behind_; // the children started behind, tended
	bool locked_ = false;                   // the foreground lock is held
};

} // namespace godot
