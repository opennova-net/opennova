#pragma once

#include <cstdint>
#include <string>

#include <editor/model/diagnostic.h>
#include <editor/run/launch_plan.h>
#include <editor/run/process_platform.h>

namespace opennova::editor {

// The editor's one managed game child (ADR 0046 d8/d10): Stopped, Running, or Stopping
// after a stop request until the child is gone or the deadline forces it. One child at a
// time: a start while another runs is refused. poll() is non-blocking and is what moves
// the machine; the UI calls it every frame, the CLI in a loop. The project session
// (editor/session/project_session.h) owns the one instance; the editor shell's
// EditorApp supplies the platform (godot/src/authoring/child_process.h).
enum class PlayState { Stopped, Running, Stopping };

inline constexpr int64_t kPlayStopDeadlineMs = 5000;
inline constexpr int64_t kPlayWaitStepMs = 10; // wait()'s pause between two observations

class PlaySession {
public:
	explicit PlaySession(ProcessPlatform &platform) : platform_(platform) {}

	bool start(const LaunchPlan &plan, Diagnostic &error);
	// Ask the child to stop; poll() escalates to a kill once kPlayStopDeadlineMs pass.
	void stop();
	// Observe the child; returns the state after the observation.
	PlayState poll();
	// Wait for the child to end, polling on the platform clock; true when Stopped in time.
	// (For a CLI or a test: the editor polls per frame instead.)
	bool wait(int64_t timeout_ms);

	PlayState state() const { return state_; }
	int64_t pid() const { return pid_; }
	const LaunchPlan &plan() const { return plan_; }
	// The build directory the child runs from ("" when Stopped): the build refuses to
	// prune it while the child is alive.
	std::string running_build_dir() const { return state_ == PlayState::Stopped ? std::string() : plan_.build_dir; }
	// True once the child exited on its own (not through stop()).
	bool exited_on_its_own() const { return exited_on_its_own_; }
	// The code the last child exited with, when it exited on its own and the platform read
	// it (-1: none). The game's own quit ends with 0 (retail's WinMain returns 0 whatever
	// its loop came to [orig: WinMain @ 0x763a80, the return @ 0x763c07]); anything else is
	// a crash or an error exit.
	int64_t exit_code() const { return exit_code_; }

private:
	void finish();

	ProcessPlatform &platform_;
	PlayState state_ = PlayState::Stopped;
	int64_t pid_ = -1;
	LaunchPlan plan_;
	int64_t stop_requested_ms_ = 0;
	bool exited_on_its_own_ = false;
	int64_t exit_code_ = -1;
};

const char *play_state_label(PlayState state);

} // namespace opennova::editor
