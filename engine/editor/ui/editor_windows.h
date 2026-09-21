#pragma once

#include <cstdint>
#include <deque>
#include <string>

#include <editor/session/editor_request.h>
#include <editor/session/session_view.h>
#include <editor/ui/editor_host.h>
#include <runtime/devtools/imgui_pass.h>

namespace opennova::editor {

class ProjectWindow;

// The OpenNova Editor's workspace (ADR 0046 d10): the ImGui pass with the editor's
// windows on it (Project in the centre, Project files on the left, Files the game needs
// on the right, Problems and Output along the bottom) and the Project / Build / Play
// menus on its menu bar. Records in through set_view(), typed requests out through
// take_request(); the shell owns the frame bracket and the OS-only requests, a test
// drives it over a null backend.
class EditorWindows : public EditorHost, public devtools::MenuBarContributor {
public:
	EditorWindows();
	~EditorWindows() override;
	EditorWindows(const EditorWindows &) = delete;
	EditorWindows &operator=(const EditorWindows &) = delete;

	devtools::ImGuiPass &pass() { return pass_; }

	// The session's view the windows draw from; must outlive the next draw.
	void set_view(const SessionView *view) { view_ = view; }

	// One layout pass (the pass's draw_frame): false when nothing was drawn.
	bool draw_frame(uint64_t frame_index) { return pass_.draw_frame(frame_index); }

	// The oldest pending request; false when none.
	bool take_request(EditorRequest &out);
	size_t pending_requests() const { return requests_.size(); }

	// The shell's answer to a PickDirectory / PickFile request (an empty path = cancelled).
	void deliver_pick(PickPurpose purpose, const std::string &path);

	// EditorHost
	const SessionView &view() const override;
	void request(EditorRequest request) override;

	// MenuBarContributor
	void draw_menu_bar(devtools::ImGuiPass &pass) override;

private:
	devtools::ImGuiPass pass_;
	const SessionView *view_ = nullptr;
	SessionView empty_;
	std::deque<EditorRequest> requests_;
	ProjectWindow *project_window_ = nullptr; // owned by the pass
};

} // namespace opennova::editor
