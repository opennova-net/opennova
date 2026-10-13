#pragma once

// A child process for the apps/ binaries: started from an argv array with no
// shell between (nothing in an argument is ever interpreted), in a working
// directory of its own, its standard input the null device and its output
// appended to a file (or discarded), then polled, waited for, asked to stop
// or killed. The NovaWorld service's hosted-server supervisor runs
// opennova-serve through it (apps/novaworld_server/hosted/local_process_provider.h).
// Platform plumbing, not a port: CreateProcessW on Windows, fork and execv
// elsewhere.
//
// The child inherits none of this process's handles or descriptors beyond the
// three standard ones (a service's listening sockets stay its own), with
// `environment` set none of its environment either but what that names (a
// service's secrets in its variables stay its own), and with
// `end_with_parent` it ends when this process does: a kill-on-close job object
// on Windows, the parent-death signal on Linux. The kernel sends that signal
// when the THREAD that started the child exits, so start children from a
// thread that lives as long as they should (LocalProcessProvider keeps a
// spawner thread of its own for this). A ChildProcess owns its child:
// destroying one whose child still runs kills the child and reaps it.

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
	// hosted/command_runner.h); empty sends it where the output goes.
	std::string error_path;
	// The child's whole environment, NAME=value entries (filter_environment
	// picks them from this process's); unset, the child inherits this process's.
	std::optional<std::vector<std::string>> environment;
	bool end_with_parent = true;
};

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
	// is missing or not executable, the directory does not exist, the output
	// file does not open); nothing runs then. One child per ChildProcess: a
	// second spawn() while one is held is refused.
	bool spawn(const SpawnOptions &options, std::string &error);

	// Whether a child was started and has not been reaped yet.
	bool started() const;
	// The child's process id; 0 before spawn() and once it was reaped.
	int64_t pid() const;

	// The exit code once the child has exited (reaping it), nullopt while it
	// runs. A child ended by a signal reads 128 plus the signal's number; one
	// TerminateProcess ended reads the code it was given (1).
	std::optional<int> poll();
	// poll(), waiting up to `timeout_ms` for the exit.
	std::optional<int> wait(int timeout_ms);
	// The exit code poll() last read, kept after the reap.
	std::optional<int> exit_code() const { return exit_code_; }

	// Asks the child to stop: SIGTERM (opennova-serve then stops its session and
	// deregisters), or on Windows, which has no such request for a process with
	// no console, TerminateProcess. A no-op once it exited.
	void terminate();
	// Ends the child now: SIGKILL, or TerminateProcess. A no-op once it exited.
	void kill();

private:
	void reset();

	int64_t pid_ = 0;
	std::optional<int> exit_code_;
#ifdef _WIN32
	void *process_ = nullptr; // HANDLE
	void *job_ = nullptr;     // HANDLE, the kill-on-close job (end_with_parent)
#endif
};

} // namespace opennova::proc
