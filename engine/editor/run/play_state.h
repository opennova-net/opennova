#pragma once

namespace opennova::editor {

// Where the editor's one managed game child is (run/play_session.h): Stopped, Running, or
// Stopping after a stop request until the child is gone or the deadline forces it. A value of
// its own (S13 V4), so the view names it without the process seam.
enum class PlayState { Stopped, Running, Stopping };

// The state's word, as the view JSON and the shell say it.
inline const char *play_state_label(PlayState state) {
	switch (state) {
	case PlayState::Stopped: return "stopped";
	case PlayState::Running: return "running";
	case PlayState::Stopping: return "stopping";
	}
	return "stopped";
}

} // namespace opennova::editor
