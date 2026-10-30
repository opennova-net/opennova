#pragma once

#include <base/proc/child_process.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

// A child process the platform started (os.h Os::spawn): its id, its exit code once it ended,
// a kill, and a release that lets it run on alone. What the engine starts and when (the updater's
// check, its install) is the engine's; the spawn itself (an argv array with no shell, the working
// directory, the output file, whether the child ends with this process) is base/proc's
// SpawnOptions, and a desktop backend's child is base/proc's ChildProcess (ChildProcessHandle).
//
// The design follows SDL3's SDL_Process (SDL_CreateProcess from an argv array,
// SDL_WaitProcess(block = false) for the exit code, SDL_KillProcess, SDL_DestroyProcess); GLFW
// and sokol_app have none.
namespace opennova::platform {

class Process {
public:
	virtual ~Process() = default; // a child still held is killed and reaped (ChildProcess's rule)
	// The child's process id; 0 once it was reaped or let go.
	virtual int64_t pid() const = 0;
	// The child's exit code once it has exited (reaping it), nullopt while it runs.
	virtual std::optional<uint32_t> poll() = 0;
	// Ends the child now; poll() reads its code once it is gone.
	virtual void kill() = 0;
	// Lets the child run on with this object holding nothing (a child started with
	// end_with_parent false; base/proc's ChildProcess::release carries the rule). False when it
	// stays held.
	virtual bool release() = 0;
};

// base/proc's child as a Process: the spawn every desktop backend's Os runs.
class ChildProcessHandle final : public Process {
public:
	// A child started from `options`; null with `error` when it did not start.
	static std::unique_ptr<Process> spawn(const proc::SpawnOptions &options, std::string &error) {
		auto handle = std::make_unique<ChildProcessHandle>();
		if (!handle->child_.spawn(options, error))
			return nullptr;
		return handle;
	}

	int64_t pid() const override { return child_.pid(); }
	std::optional<uint32_t> poll() override { return child_.poll(); }
	void kill() override { child_.kill(); }
	bool release() override { return child_.release(); }

private:
	proc::ChildProcess child_;
};

} // namespace opennova::platform
