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

const char *find_scope_token(WorkspaceView::FindScope scope) {
	switch (scope) {
		case WorkspaceView::FindScope::All:
			return "all";
		case WorkspaceView::FindScope::Files:
			return "files";
		case WorkspaceView::FindScope::Names:
			return "names";
		case WorkspaceView::FindScope::Usages:
			return "usages";
	}
	return "all";
}

bool find_scope_from_token(const std::string &token, WorkspaceView::FindScope &out) {
	using S = WorkspaceView::FindScope;
	for (const S scope : {S::All, S::Files, S::Names, S::Usages})
		if (token == find_scope_token(scope)) {
			out = scope;
			return true;
		}
	return false;
}

} // namespace opennova::editor
