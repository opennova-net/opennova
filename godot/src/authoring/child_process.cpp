#include "authoring/child_process.h"

#include <utility>
#include <vector>

#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/time.hpp>

#ifdef _WIN32
#include <windows.h>

#include <base/io/os_path.h>
#endif

namespace godot {

using opennova::editor::ProcessLiveness;

namespace {

ProcessLiveness liveness_of(opennova::proc::Liveness liveness) {
	switch (liveness) {
		case opennova::proc::Liveness::Alive:
			return ProcessLiveness::Alive;
		case opennova::proc::Liveness::Dead:
			return ProcessLiveness::Dead;
		case opennova::proc::Liveness::Unknown:
			break;
	}
	return ProcessLiveness::Unknown;
}

int64_t ticks_ms() {
	return static_cast<int64_t>(Time::get_singleton()->get_ticks_msec());
}

#ifdef _WIN32
// What a child's top-level windows are now: whether one shows, and whether one is the foreground window.
struct ChildWindows {
	DWORD pid = 0;
	bool shown = false;
	bool foreground = false;
};
BOOL CALLBACK look_at_window(HWND window, LPARAM param) {
	ChildWindows &look = *reinterpret_cast<ChildWindows *>(param);
	DWORD owner = 0;
	GetWindowThreadProcessId(window, &owner);
	if (owner != look.pid || !IsWindowVisible(window)) return TRUE;
	look.shown = true;
	look.foreground = look.foreground || window == GetForegroundWindow();
	return TRUE;
}

// A child started behind: each shown window of it sent to the bottom without activation (posted to its
// thread: the editor never waits on the game) and its flashing stopped.
BOOL CALLBACK keep_window_behind(HWND window, LPARAM param) {
	DWORD owner = 0;
	GetWindowThreadProcessId(window, &owner);
	if (owner != static_cast<DWORD>(param) || !IsWindowVisible(window)) return TRUE;
	SetWindowPos(window, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
	FLASHWINFO flash{};
	flash.cbSize = sizeof(flash);
	flash.hwnd = window;
	flash.dwFlags = FLASHW_STOP;
	FlashWindowEx(&flash);
	return TRUE;
}
#endif

} // namespace

ChildProcessPlatform::~ChildProcessPlatform() {
	// The editor closing lets its games go, never ends them (a Play lease names one across a restart).
	std::lock_guard<std::mutex> lock(mutex_);
	for (auto &[pid, child] : children_) child.release();
	children_.clear();
}

opennova::proc::ChildProcess *ChildProcessPlatform::child_of(int64_t pid) {
	const auto it = children_.find(pid);
	return it == children_.end() ? nullptr : &it->second;
}

bool ChildProcessPlatform::exited(int64_t pid) {
	opennova::proc::ChildProcess *child = child_of(pid);
	return !child || child->exit_code() || child->poll();
}

bool ChildProcessPlatform::can_spawn() const {
	return true;
}

int64_t ChildProcessPlatform::spawn(const opennova::editor::LaunchPlan &plan, std::string &error) {
	opennova::proc::SpawnOptions options;
	options.argv.push_back(plan.executable);
	options.argv.insert(options.argv.end(), plan.args.begin(), plan.args.end());
	options.working_dir = plan.working_dir;
	// The game outlives the editor: started out of the editor's job and session, let go at release().
	options.end_with_parent = false;
	// Behind (the MCP gaps lane): its first window shown without activation, and the foreground locked only
	// while it starts (tend() lets it go once its first window has been sent back: BehindStarts).
	options.behind = plan.behind;
	std::lock_guard<std::mutex> lock(mutex_);
#ifdef _WIN32
	if (plan.behind && !locked_) locked_ = LockSetForegroundWindow(LSFW_LOCK) != 0;
#endif
	opennova::proc::ChildProcess child;
	if (!child.spawn(options, error)) {
#ifdef _WIN32
		// Nothing started: a lock taken for it goes at once, unless another child still starts behind.
		if (locked_ && !behind_.lock_wanted(ticks_ms())) {
			LockSetForegroundWindow(LSFW_UNLOCK);
			locked_ = false;
		}
#endif
		return -1;
	}
	const int64_t pid = child.pid();
	if (opennova::proc::ChildProcess *stale = child_of(pid)) stale->release();
	children_[pid] = std::move(child);
	if (plan.behind) behind_.spawned(pid, ticks_ms());
	return pid;
}

bool ChildProcessPlatform::is_running(int64_t pid) {
	std::lock_guard<std::mutex> lock(mutex_);
	return !exited(pid);
}

bool ChildProcessPlatform::terminate(int64_t pid) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (exited(pid)) return true;
	return child_of(pid)->terminate(true);
}

bool ChildProcessPlatform::kill(int64_t pid) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (exited(pid)) return true;
	// Only asks the OS to end it: the child holds its files until it is gone, which PlaySession's next poll
	// observes (is_running) before Play reads it stopped. Never waited on here: the editor's frame never blocks
	// on the game (S26 C8).
	child_of(pid)->kill();
	return true;
}

bool ChildProcessPlatform::exit_code(int64_t pid, uint32_t &out) {
	std::lock_guard<std::mutex> lock(mutex_);
	opennova::proc::ChildProcess *child = child_of(pid);
	if (!child) return false;
	const std::optional<uint32_t> code = child->exit_code() ? child->exit_code() : child->poll();
	if (!code) return false;
	out = *code;
	return true;
}

void ChildProcessPlatform::release(int64_t pid) {
	std::lock_guard<std::mutex> lock(mutex_);
	const auto it = children_.find(pid);
	if (it == children_.end()) return;
	it->second.release();
	children_.erase(it);
}

bool ChildProcessPlatform::process_identity(int64_t pid, opennova::editor::ProcessIdentity &out) {
	std::lock_guard<std::mutex> lock(mutex_);
	opennova::proc::ChildProcess *child = child_of(pid);
	opennova::proc::ProcessIdentity identity;
	if (!child || !child->identity(identity)) return false;
	out.image = identity.image;
	out.created = identity.created;
	return !out.created.empty();
}

ProcessLiveness ChildProcessPlatform::process_liveness(int64_t pid, const std::string &created) {
	return liveness_of(opennova::proc::process_liveness(pid, created));
}

ProcessLiveness ChildProcessPlatform::semaphore_held(const std::string &name) {
#ifdef _WIN32
	// Opened by its name in this session's namespace, where the game makes it (CreateSemaphoreA with no
	// Global\ prefix), for SYNCHRONIZE alone, and let go at once: one that will not be opened for that
	// right is there all the same.
	HANDLE handle = OpenSemaphoreW(SYNCHRONIZE, FALSE, opennova::io::widen_utf8(name).c_str());
	if (handle != nullptr) {
		CloseHandle(handle);
		return ProcessLiveness::Alive;
	}
	const DWORD error = GetLastError();
	if (error == ERROR_FILE_NOT_FOUND) return ProcessLiveness::Dead;
	return error == ERROR_ACCESS_DENIED ? ProcessLiveness::Alive : ProcessLiveness::Unknown;
#else
	(void)name;
	return ProcessLiveness::Unknown;
#endif
}

int64_t ChildProcessPlatform::now_ms() {
	return ticks_ms();
}

void ChildProcessPlatform::tend() {
#ifdef _WIN32
	std::lock_guard<std::mutex> lock(mutex_);
	if (!behind_.tending() && !locked_) return;
	const int64_t now = ticks_ms();
	for (const int64_t pid : behind_.pids()) {
		const bool gone = exited(pid);
		ChildWindows look;
		look.pid = static_cast<DWORD>(pid);
		if (!gone) EnumWindows(look_at_window, reinterpret_cast<LPARAM>(&look));
		opennova::editor::BehindStarts::Step step = opennova::editor::BehindStarts::Step::Leave;
		if (!behind_.tend(pid, now, gone, look.shown, look.foreground, step)) continue;
		if (step == opennova::editor::BehindStarts::Step::SendBack) EnumWindows(keep_window_behind, static_cast<LPARAM>(pid));
	}
	// Held only while a child starts: let go once each one's first window went back (or kLockMs passed).
	if (locked_ && !behind_.lock_wanted(now)) {
		LockSetForegroundWindow(LSFW_UNLOCK);
		locked_ = false;
	}
#else
	locked_ = false; // no foreground lock off Windows
#endif
}

void ChildProcessPlatform::sleep_ms(int64_t ms) {
	OS::get_singleton()->delay_msec(static_cast<int>(ms));
}

} // namespace godot
