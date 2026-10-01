#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include <editor/session/editor_request.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/workspace.h>
#include <editor/ui/files_window.h>
#include <editor/ui/import_dialog.h>
#include <editor/ui/project_find.h>
#include <editor/ui/project_settings_dialog.h>
#include <editor/ui/rename_dialog.h>
#include <editor/ui/welcome_view.h>
#include <runtime/devtools/imgui_pass.h>

namespace opennova::editor {

class DocumentWindow;
class InspectorWindow;
class PreviewWindow;
class ViewportDeviceSource;

// The OpenNova Editor's workspace (ADR 0046 d10, S11d): the ImGui pass with the editor's
// six windows on it (Files on the left, Document in the centre with Preview beside it,
// the Inspector on the right, Problems and Output along the bottom) and the File / Edit /
// Build menus on its menu bar, which ends with the unsaved files, the problem counts, the
// build and game state and Build / Play / Stop. The modals (the unsaved prompt, the import
// dialog, the project settings, a new project, a new file's name) are drawn here every
// frame, never by a window a hidden tab would skip; so are Find in project (Edit menu,
// Ctrl+Shift+F), beside the Document window's own find bar (Edit > Find..., Ctrl+F), and
// Rename everywhere (the Inspector's Rename... on a field defining a name, F2 there). Records in through set_view(), typed
// requests out through take_request(); the shell owns the frame bracket and the OS-only
// requests, a test drives it over a null backend.
class EditorWindows : public Workspace, public devtools::MenuBarContributor {
public:
	EditorWindows();
	~EditorWindows() override;
	EditorWindows(const EditorWindows &) = delete;
	EditorWindows &operator=(const EditorWindows &) = delete;

	devtools::ImGuiPass &pass() { return pass_; }

	// The session's view the windows draw from; must outlive the next draw. Another view's events
	// are its own: the windows are sent those posted from now on (begin_frame).
	void set_view(const SessionView *view);

	// One layout pass (the pass's draw_frame) inside the frame bracket below: false when
	// nothing was drawn.
	bool draw_frame(uint64_t frame_index);
	// The frame bracket, for a shell that drives the pass itself (EditorApp's layout pass):
	// begin_frame sends each view event posted since the last frame (view_events.h) to the
	// mailbox of the window it is for, which holds it until that window draws (a RevealRecord to
	// the view of its document and to the Inspector, a RevealFile to Files, which comes forward
	// for it, an AskRename to Rename everywhere, a SettingsApplied to the project settings, an
	// ImportPlanned to the import dialog); a request that acts on the files as saved, raised in
	// between, waits for every other request of the frame (request()), and end_frame queues it
	// after them, once the viewports' canvases that did not draw this frame (the Preview window's,
	// a document view's) have ended their gestures.
	void begin_frame();
	void end_frame();

	// The oldest pending request; false when none.
	bool take_request(EditorRequest &out);
	size_t pending_requests() const { return requests_.size() + deferred_.size(); }

	// The shell's answer to a PickDirectory / PickFile request (an empty path = cancelled):
	// a folder for a new project fills the new-project form, a project to open opens, the
	// game install folder and the runtime fill the project settings' fields (while the
	// dialog that asked is open on the project open now), a file to import is planned as
	// deliver_picks plans several.
	void deliver_pick(PickPurpose purpose, const std::string &path);
	// The shell's answer to a PickFile request that picks several files (none = cancelled): the
	// files to import planned in the import dialog (a PreviewImport raised like any window's
	// request), with the files they need when the editor's setting says so.
	void deliver_picks(PickPurpose purpose, const std::vector<std::string> &paths);

	// The Shell's devices, by document and viewport kind, the viewports' canvases draw through
	// (null: no picture).
	void set_devices(ViewportDeviceSource *devices) { devices_ = devices; }

	// The new-project form (the welcome view's and File > New project...'s), for a test.
	const NewProjectForm &new_project_form() const { return new_project_; }

	// Workspace
	const SessionView &view() const override;
	void request(EditorRequest request) override;
	ViewportDeviceSource *devices() const override { return devices_; }

	// MenuBarContributor
	void draw_menu_bar(devtools::ImGuiPass &pass) override;
	void draw_menu_bar_trailing(devtools::ImGuiPass &pass) override;

private:
	void draw_file_menu(const SessionView &v);
	void draw_edit_menu(const SessionView &v, const DocumentBase *document);
	void draw_build_menu(const SessionView &v);
	void draw_new_project();
	void shortcuts(const SessionView &v, const DocumentBase *document);

	void dispatch_events();

	devtools::ImGuiPass pass_;
	const SessionView *view_ = nullptr;
	SessionView empty_;
	uint64_t dispatched_ = 0; // the seq of the last view event sent to a window
	ViewportDeviceSource *devices_ = nullptr; // the Shell's (set_devices)
	std::deque<EditorRequest> requests_;
	std::vector<EditorRequest> deferred_; // this frame's requests that act on the files as saved
	bool in_frame_ = false;
	NewProjectForm new_project_;
	bool open_new_project_ = false;
	ProjectSettingsDialog settings_;
	ImportDialog import_;
	NewFilePrompt new_file_;
	ProjectFind find_;
	RenameDialog rename_;
	FilesWindow *files_window_ = nullptr; // owned by the pass
	DocumentWindow *document_window_ = nullptr; // owned by the pass
	InspectorWindow *inspector_window_ = nullptr; // owned by the pass
	PreviewWindow *preview_window_ = nullptr; // owned by the pass
	devtools::Window *problems_window_ = nullptr; // owned by the pass
};

} // namespace opennova::editor
