#include <editor/session/view/workspace_view.h>

namespace opennova::editor {

const char *sound_state_token(WorkspaceView::SoundState state) {
	switch (state) {
		case WorkspaceView::SoundState::Idle:
			return "idle";
		case WorkspaceView::SoundState::Starting:
			return "starting";
		case WorkspaceView::SoundState::Playing:
			return "playing";
		case WorkspaceView::SoundState::Ended:
			return "ended";
		case WorkspaceView::SoundState::Stopped:
			return "stopped";
		case WorkspaceView::SoundState::Failed:
			return "failed";
	}
	return "idle";
}

} // namespace opennova::editor
