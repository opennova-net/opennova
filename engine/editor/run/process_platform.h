#pragma once

#include <cstdint>

#include <editor/run/launch_plan.h>

namespace opennova::editor {

// The OS seam under PlaySession (ADR 0046 d8): spawn a child with a working directory,
// ask whether it still runs, ask it to stop, force it, forget it, and read a clock. The
// Godot layer implements it (godot/src/authoring/process.*, the restored working-dir
// spawn that keeps the process handle so a recycled pid is never mistaken for the
// child); the tests drive a fake. Spawn, liveness and kill come from ONE implementation:
// mixing Godot's own process helpers with a foreign child once reported "stopped" while
// the child kept running.
class ProcessPlatform {
public:
	virtual ~ProcessPlatform() = default;
	// The child's id, or -1 when it could not be started.
	virtual int64_t spawn(const LaunchPlan &plan) = 0;
	virtual bool is_running(int64_t pid) = 0;
	// Ask the child to exit (close its window / SIGTERM); true when the request was delivered
	// or the child had already exited.
	virtual bool terminate(int64_t pid) = 0;
	// Force the child down; true when it is gone afterwards.
	virtual bool kill(int64_t pid) = 0;
	// Forget the child (release the handle spawn kept).
	virtual void release(int64_t pid) = 0;
	virtual int64_t now_ms() = 0;
	// Yield between two observations of a blocking wait (PlaySession::wait); the
	// per-frame poll never calls it.
	virtual void sleep_ms(int64_t ms) = 0;
};

} // namespace opennova::editor
