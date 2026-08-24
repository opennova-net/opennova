#pragma once

// Spawn a child process with an explicit WORKING DIRECTORY, and track it by its handle.
//
// Godot's OS.create_process() gives the child the editor's working directory and offers no way
// to change it. That is fine for our own runtime, which takes absolute paths on the command
// line, but not for retail Jointops.exe: it opens its boot archives CWD-relative through a raw
// _lopen (retail: PFF_OpenAllArchives @0x4a4310, see docs/vfs/vfs-pff-mount-re.md), so launched
// from anywhere but the packed dir it finds no archives at all and dies on the zero-archives
// gate with ShowEarlyError(3) -- before writing so much as a /FRISK line to say why.
//
// Windows only, deliberately: the only consumer is a Windows game binary, and a spawn that
// silently ignored the working directory would reproduce exactly the bug this exists to fix.

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

namespace godot {

class Process : public Object {
	GDCLASS(Process, Object)

public:
	// Launch `path` with `args`, with the child's working directory set to `working_dir` (empty
	// inherits ours, exactly like OS.create_process). Returns the child's process id, or -1 on
	// failure (including any non-Windows host).
	//
	// The process HANDLE is kept until release(): every later query about this pid goes through
	// that handle, so a pid Windows has recycled to an unrelated process after the child exited
	// can never be mistaken for it. A pid is the caller's token, not the identity.
	static int64_t spawn_in_dir(const String &path, const PackedStringArray &args,
			const String &working_dir);

	// Whether this build can honour a working directory at all. GDScript gates the
	// Play-in-Retail action on it rather than failing at the moment of launch.
	static bool supports_working_directory();

	// Terminate a process by id. Returns true if it was terminated or had already exited.
	// Returns false when the process exists and could not be terminated -- never "true because
	// nothing could be opened", which is how a stop once reported success while retail kept
	// running.
	static bool kill_pid(int64_t pid);

	// Whether a process id is still running.
	//
	// kill_pid / is_running / wait_for_exit exist because Godot's OS.kill() and
	// OS.is_process_running() are built around the processes Godot ITSELF spawned. Against a
	// child from spawn_in_dir they misreport: is_process_running said "gone", so the session's
	// kill path short-circuited as already-stopped and the child kept running with the editor
	// reporting "stopped". Spawn, liveness, and kill have to come from the same place.
	static bool is_running(int64_t pid);

	// Block until the process has exited, for at most `timeout_msec` (negative = forever).
	// Returns true once it is gone. TerminateProcess only REQUESTS the exit: the child can still
	// hold its files open for a moment afterwards, and a caller that repacks those files on the
	// same stack races it.
	static bool wait_for_exit(int64_t pid, int64_t timeout_msec);

	// Forget a child: closes the handle spawn_in_dir kept. Call once the session has finished
	// with the pid; a released pid falls back to by-id probing, which has the recycling caveat.
	static void release(int64_t pid);

protected:
	static void _bind_methods();
};

} // namespace godot
