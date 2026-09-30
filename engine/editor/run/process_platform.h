#pragma once

#include <cstdint>
#include <string>

#include <editor/run/launch_plan.h>

namespace opennova::editor {

// The OS seam under PlaySession (ADR 0046 d8): spawn a child with a working directory,
// ask whether it still runs, ask it to stop, force it, read the code it exited with,
// forget it, and read a clock. The Godot layer implements it
// (godot/src/authoring/child_process.*, the working-dir spawn that keeps the process handle
// so a recycled pid is never mistaken for the child); the tests drive a fake. Spawn,
// liveness and kill come from ONE implementation: mixing Godot's own process helpers with a
// foreign child once reported "stopped" while the child kept running.
class ProcessPlatform {
public:
	virtual ~ProcessPlatform() = default;
	// False where this platform has no spawn at all: Play says so instead of trying.
	virtual bool can_spawn() const { return true; }
	// The child's id, or -1 when it could not be started.
	virtual int64_t spawn(const LaunchPlan &plan) = 0;
	virtual bool is_running(int64_t pid) = 0;
	// Ask the child to exit (close its window / SIGTERM); true when the request was delivered
	// or the child had already exited.
	virtual bool terminate(int64_t pid) = 0;
	// Force the child down; true when it is gone afterwards.
	virtual bool kill(int64_t pid) = 0;
	// The code an exited child ended with, read through the handle spawn kept (so before
	// release()); false while it runs, or where the platform cannot read one.
	virtual bool exit_code(int64_t pid, uint32_t &out) {
		(void)pid;
		(void)out;
		return false;
	}
	// Forget the child (release the handle spawn kept).
	virtual void release(int64_t pid) = 0;
	// Whether a process this platform did not spawn (or spawned before the editor restarted) still
	// runs `executable` as `pid`: a Play lease's game (run/play_lease.h). The image's file name is
	// compared, so a recycled pid running something else is not it. False where it cannot tell.
	virtual bool process_alive(int64_t pid, const std::string &executable) {
		(void)pid;
		(void)executable;
		return false;
	}
	virtual int64_t now_ms() = 0;
	// Yield between two observations of a blocking wait (PlaySession::wait); the
	// per-frame poll never calls it.
	virtual void sleep_ms(int64_t ms) = 0;
};

} // namespace opennova::editor
