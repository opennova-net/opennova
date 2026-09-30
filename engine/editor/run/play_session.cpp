#include <editor/run/play_session.h>

namespace opennova::editor {

bool PlaySession::start(const LaunchPlan &plan, Diagnostic &error) {
	if (state_ != PlayState::Stopped) {
		error = make_finding(CoreFinding::PlayAlreadyRunning, DiagnosticSeverity::Error,
		                     "The game is already running; stop it before starting it again.");
		return false;
	}
	const int64_t pid = platform_.spawn(plan);
	if (pid < 0) {
		error = make_finding(CoreFinding::PlaySpawn, DiagnosticSeverity::Error,
		                     "The game could not be started: " + plan.executable);
		return false;
	}
	pid_ = pid;
	plan_ = plan;
	state_ = PlayState::Running;
	exited_on_its_own_ = false;
	exit_code_ = -1;
	stop_requested_ms_ = 0;
	return true;
}

void PlaySession::stop() {
	if (state_ != PlayState::Running) return;
	state_ = PlayState::Stopping;
	stop_requested_ms_ = platform_.now_ms();
	platform_.terminate(pid_);
}

PlayState PlaySession::poll() {
	if (state_ == PlayState::Stopped) return state_;
	if (!platform_.is_running(pid_)) {
		exited_on_its_own_ = state_ == PlayState::Running;
		// Read through the handle before finish() releases it.
		uint32_t code = 0;
		if (exited_on_its_own_ && platform_.exit_code(pid_, code)) exit_code_ = int64_t(code);
		finish();
		return state_;
	}
	if (state_ == PlayState::Stopping && platform_.now_ms() - stop_requested_ms_ >= kPlayStopDeadlineMs) {
		platform_.kill(pid_);
		finish();
	}
	return state_;
}

bool PlaySession::wait(int64_t timeout_ms) {
	const int64_t start = platform_.now_ms();
	while (poll() != PlayState::Stopped) {
		if (platform_.now_ms() - start >= timeout_ms) return false;
		platform_.sleep_ms(kPlayWaitStepMs);
	}
	return true;
}

void PlaySession::finish() {
	platform_.release(pid_);
	pid_ = -1;
	state_ = PlayState::Stopped;
	stop_requested_ms_ = 0;
}

} // namespace opennova::editor
