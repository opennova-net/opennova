#pragma once

#include <cstdint>
#include <string>

#include <editor/run/launch_plan.h>

namespace opennova::editor {

// Whether a process runs: Unknown where the platform cannot tell (it has no process seam, or the
// process will not be opened for a question), which a caller about to delete something treats as
// alive.
enum class ProcessLiveness : uint8_t { Alive, Dead, Unknown };

// A child as the OS knows it, read through the handle spawn kept: the image it runs, as the OS
// reports it (a link or an alias resolved), and when the OS created it, written as the platform
// writes it ("" when unknown): compared with another of the same platform, never read.
struct ProcessIdentity {
	std::string image;
	std::string created;
};

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
	// The identity of the child `pid` this platform spawned (process_identity: the image and the
	// creation time the OS reports), read through the handle spawn kept, before release(); false
	// where the platform cannot tell.
	virtual bool process_identity(int64_t pid, ProcessIdentity &out) {
		(void)pid;
		(void)out;
		return false;
	}
	// Whether the process a Play lease names still runs (run/play_lease.h): `pid`, created at
	// `created` (process_identity's), which this platform may not have spawned, or spawned before
	// the editor restarted. Dead when the platform knows it: no process of that id, one that has
	// exited, or one created at another time under the id (a recycled pid, a reboot's). Alive when
	// the one created then runs. Unknown where it cannot tell (the process will not be opened, or
	// the lease records no time), the default.
	virtual ProcessLiveness process_liveness(int64_t pid, const std::string &created) {
		(void)pid;
		(void)created;
		return ProcessLiveness::Unknown;
	}
	virtual int64_t now_ms() = 0;
	// Yield between two observations of a blocking wait (PlaySession::wait); the
	// per-frame poll never calls it.
	virtual void sleep_ms(int64_t ms) = 0;
};

} // namespace opennova::editor
