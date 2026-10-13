#pragma once

// A child process: started from an argv array with no shell between (nothing in
// an argument is ever interpreted), in a working directory of its own, its
// standard input the null device and its output appended to a file (or
// discarded; its standard error in a file of its own where asked), then polled,
// waited for, asked to stop, killed, or released to run on its own. The one
// child-process seam of the engine's embedders: the NovaWorld service's
// hosted-server supervisor runs opennova-serve through it
// (apps/novaworld_server/hosted/local_process_provider.h) and its cloud provider
// the AWS CLI (hosted/command_runner.h), and a front end that
// starts the game (the editor's Play) holds the game through it. Platform
// plumbing, not a port: CreateProcessW on Windows, fork and execv elsewhere. It
// is not in the web build: a browser starts no process.
//
// The child inherits none of this process's handles or descriptors beyond the
// three standard ones (a service's listening sockets stay its own), with
// `environment` set none of its environment either but what that names (a
// service's secrets in its variables stay its own), and with
// `end_with_parent` it ends when this process does: a kill-on-close job object
// on Windows, the parent-death signal on Linux. The kernel sends that signal
// when the THREAD that started the child exits, so start children from a
// thread that lives as long as they should (LocalProcessProvider keeps a
// spawner thread of its own for this). Without `end_with_parent` the child is
// meant to outlive this process: on Windows it breaks away from any job this
// process is in (where that job allows it), so the job's kill-on-close does not
// take it down; on POSIX it leads a session of its own (setsid), so a hangup of
// this process's terminal does not either. A ChildProcess owns its child:
// destroying one whose child still runs kills the child and reaps it, unless
// release() let it go first.
//
// Windows paths. The program's path and the working directory are given with
// '/' or '\' (both are spelled '\' at the call), and one past what
// CreateProcessW takes (an image of MAX_PATH less its terminator, a current
// directory of MAX_PATH less its trailing backslash and terminator) is passed
// in its short (8.3) form where its volume keeps one; where it keeps none, the
// spawn is refused with that error rather than handed a path it would cut. A
// GUI child keeps its windows: the spawn's CREATE_NO_WINDOW only keeps a
// console child from opening a console window.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace opennova::proc {

struct SpawnOptions {
	// argv[0] is the program's path, run as given (no PATH search); the rest
	// reach the program as they are.
	std::vector<std::string> argv;
	// The child's working directory; empty keeps this process's.
	std::string working_dir;
	// The file the child's standard output and standard error are appended to
	// (created when missing; owner-only on POSIX); empty discards them.
	std::string output_path;
	// The file the child's standard error is appended to instead (created when
	// missing; owner-only on POSIX), so a program whose output is data keeps it
	// apart from its diagnostics (the hosted servers' AWS CLI runs,
	// apps/novaworld_server/hosted/command_runner.h); empty sends it where the
	// output goes.
	std::string error_path;
	// The child's whole environment, NAME=value entries (filter_environment
	// picks them from this process's); unset, the child inherits this process's.
	std::optional<std::vector<std::string>> environment;
	// The child ends with this process (above). A child meant to outlive it,
	// to be let go with release(), is started with this false.
	bool end_with_parent = true;
	// Windows: the child's first window is shown without activation
	// (STARTF_USESHOWWINDOW, SW_SHOWNOACTIVATE), so a game started behind
	// does not take the foreground. Ignored elsewhere.
	bool behind = false;
};

// A process as the OS knows it: the image it runs, '/'-separated (Windows: the
// path it was started by, QueryFullProcessImageNameW's, with any 8.3 component
// spelled out by GetLongPathNameW; Linux: /proc/<pid>/exe's target, a link
// resolved), and when the OS created it, as the platform writes it (Windows:
// GetProcessTimes' creation FILETIME as a decimal 100 ns count; Linux:
// "<boot_id>:<starttime>", the boot's id from /proc/sys/kernel/random/boot_id
// and /proc/<pid>/stat's starttime in clock ticks after boot). `created` is
// compared with another of the same platform, never read: equal for one
// process, different for another that later takes its pid, after a reboot too.
struct ProcessIdentity {
	std::string image;
	std::string created;
};

// Whether a process runs. Unknown where the platform cannot tell (it has no way
// to ask, or the process will not be opened for the question), which a caller
// about to delete something treats as alive.
enum class Liveness : uint8_t { Alive, Dead, Unknown };

// Whether process `pid`, created at `created` (a ProcessIdentity's), still runs:
// one this process need not have started, or started before it restarted. Dead
// when no process of that id runs, it has exited (a zombie on Linux), or the one
// under the id was created at another time (a recycled pid, a reboot's); Alive
// when the one created then runs; Unknown when `created` is empty or `pid` is
// not positive, when the process will not be opened (Windows' OpenProcess for
// PROCESS_QUERY_LIMITED_INFORMATION alone, which an elevated process still
// grants, failing for any reason but that no such process exists), and on
// platforms other than Windows and Linux. On Windows the exit is asked by
// waiting on the process (SYNCHRONIZE; its exit code could be STILL_ACTIVE's
// 259), and by its exit code only where that right is refused.
Liveness process_liveness(int64_t pid, const std::string &created);

// This process's environment as NAME=value entries (Windows' hidden
// "=C:"-style entries left out). Reads no variable by name.
std::vector<std::string> current_environment();
// The entries of current_environment() whose name is one of `names` (case
// aside on Windows, where names are case-insensitive); a name ending in '*'
// matches every name it begins (LC_* is every locale variable).
std::vector<std::string> filter_environment(const std::vector<std::string> &names);

class ChildProcess {
public:
	ChildProcess();
	~ChildProcess();
	ChildProcess(ChildProcess &&other) noexcept;
	ChildProcess &operator=(ChildProcess &&other) noexcept;
	ChildProcess(const ChildProcess &) = delete;
	ChildProcess &operator=(const ChildProcess &) = delete;

	// Starts the child. False with `error` when it did not start (the program
	// is missing or not executable, the directory does not exist, the output or
	// error file does not open, a Windows path is too long even in its short
	// form, with the OS's own error where there is one); nothing runs then. One child
	// per ChildProcess: a second spawn() while one runs is refused, and one
	// after the first has exited forgets it.
	bool spawn(const SpawnOptions &options, std::string &error);

	// Whether a child was started and has not been reaped yet.
	bool started() const;
	// The child's process id; 0 before spawn() and once it was reaped.
	int64_t pid() const;

	// The exit code once the child has exited (reaping it), nullopt while it
	// runs. Unsigned: a Windows code such as 0xC0000005 reads as it is. A POSIX
	// child ended by a signal reads 128 plus the signal's number; one
	// TerminateProcess ended reads the code it was given (1).
	std::optional<uint32_t> poll();
	// poll(), waiting up to `timeout_ms` (negative: no limit) for the exit.
	std::optional<uint32_t> wait(int timeout_ms);
	// The exit code poll() last read, kept after the reap.
	std::optional<uint32_t> exit_code() const { return exit_code_; }

	// The child's identity (ProcessIdentity), read when it started (what could
	// not be read then is read now), false when it cannot be read. On Windows it
	// is read through the process handle the spawn kept, so a pid the OS
	// recycled is never mistaken for the child, and it answers while that handle
	// is held: past the exit, until release() or destruction. On Linux it is
	// read from /proc/<pid> and answers while the child is unreaped (once
	// reaped, its pid may be another's). Elsewhere false.
	bool identity(ProcessIdentity &out) const;

	// Asks the child to stop. POSIX: SIGTERM (opennova-serve then stops its
	// session and deregisters). Windows: WM_CLOSE posted to each of the child's
	// top-level unowned windows (a game's orderly quit); a child with no such
	// window (a console or headless one, opennova-serve's, or a GUI one whose
	// window is not up yet) has no stop request to take, so it is ended with
	// TerminateProcess, or with `polite_only` left alone. True when the request
	// was delivered (a WM_CLOSE posted, SIGTERM sent) or the fallback ended the
	// child; false when nothing was delivered or no child runs. A child may
	// ignore the request: a caller that needs it gone waits, then kill()s.
	bool terminate(bool polite_only = false);
	// Ends the child now: SIGKILL, or TerminateProcess (code 1). Returns at once;
	// wait() reaps it. A no-op once it exited.
	void kill();

	// Lets the child go without ending it: this object forgets it (closing the
	// handles it holds; a POSIX child is reaped by a detached thread once it
	// exits), keeps no exit code, and then holds nothing. A child meant to be
	// let go is started with end_with_parent false. One that runs in its
	// kill-on-close job (Windows) or with the parent-death signal armed (Linux)
	// would end with this process regardless (neither can be taken back), so
	// that release() is refused (false) and the child stays held; where the
	// job or the signal could not be set up, it is let go. True otherwise, also
	// when nothing was held.
	bool release();

private:
	void reset();

	int64_t pid_ = 0;
	std::optional<uint32_t> exit_code_;
	bool ends_with_parent_ = false; // the kill-on-close job or the parent-death signal is armed
	ProcessIdentity identity_; // read at spawn
#ifdef _WIN32
	void *process_ = nullptr; // HANDLE, kept until release() or destruction
	void *job_ = nullptr; // HANDLE, the kill-on-close job (end_with_parent)
#endif
};

} // namespace opennova::proc
