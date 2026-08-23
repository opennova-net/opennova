#pragma once

// Spawn a detached child process with an explicit WORKING DIRECTORY.
//
// Godot's OS.create_process() gives the child the editor's working directory and offers no way
// to change it. That is fine for our own runtime, which takes absolute paths on the command
// line, but not for retail Jointops.exe: it opens its boot archives CWD-relative through a raw
// _lopen [orig: PFF_OpenAllArchives @ 0x4a4310], so launched from anywhere but the packed dir
// it finds no archives at all and dies on the zero-archives gate with ShowEarlyError(3) --
// before writing so much as a /FRISK line to say why.
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
	// Launch `path` with `args`, with the child's working directory set to `working_dir`.
	// Returns the child's process id, or -1 on failure (including any non-Windows host).
	// The child is detached and owned by the caller through the returned pid.
	// OS.is_process_running() reads it fine, but OS.kill() does NOT reliably reach it --
	// use kill_pid() below, which is the symmetric half of this call.
	static int64_t spawn_in_dir(const String &path, const PackedStringArray &args,
			const String &working_dir);

	// Whether this build can honour a working directory at all. GDScript gates the
	// Play-in-Retail action on it rather than failing at the moment of launch.
	static bool supports_working_directory();

	// Terminate a process by id. Returns true if it was terminated, or was already gone.
	static bool kill_pid(int64_t pid);

	// Whether a process id is still running.
	//
	// kill_pid and is_running exist because Godot's OS.kill() / OS.is_process_running() are
	// built around the processes Godot ITSELF spawned. Against a child from spawn_in_dir they
	// misreport: is_process_running said "gone", so the session's kill path short-circuited
	// as already-stopped and the child kept running with the editor reporting "stopped".
	// Spawn, liveness and kill have to come from the same place.
	static bool is_running(int64_t pid);

protected:
	static void _bind_methods();
};

} // namespace godot
