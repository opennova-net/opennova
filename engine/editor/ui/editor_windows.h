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
#include <editor/ui/texture_source_dialog.h>
#include <editor/ui/welcome_view.h>
#include <runtime/devtools/imgui_pass.h>

namespace opennova::editor {

class DocumentWindow;
class InspectorWindow;
class PreviewWindow;
class ProblemsWindow;
class TextureThumbnailImages;
class ViewportDeviceSource;

// The OpenNova Editor's workspace (ADR 0046 d10, S11d): the ImGui pass with the editor's
// six windows on it (Files on the left, Document in the centre with Preview beside it,
// the Inspector on the right, Problems and Output along the bottom) and on its menu bar Back and
// Forward (the navigation history) then the File / Edit / Build menus, the bar ending with the
// unsaved files, the problem counts, the build and game state and Build / Play / Stop. The modals (the unsaved prompt, the import
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
	// ImportPlanned to the import dialog; a FocusWindow brings the window it names forward); a request
	// that acts on the files as saved, raised in
	// between, waits for every other request of the frame (request()), and end_frame queues it
	// after them, once the viewports' canvases that did not draw this frame (the Preview window's,
	// a document view's) have ended their gestures.
	void begin_frame();
	void end_frame();

	// The oldest pending request; false when none.
	bool take_request(EditorRequest &out);
	// The pending requests of `kind`, oldest first, taken out; the others left waiting as they were (a
	// test standing in for the Shell serves one kind as it comes: the windows' set_workspace requests).
	std::vector<EditorRequest> take_requests_of(EditorRequestKind kind);
	size_t pending_requests() const { return requests_.size() + deferred_.size(); }

	// The shell's answer to a PickDirectory / PickFile request (an empty path = cancelled):
	// a folder for a new project fills the new-project form (the workspace's), a project to open opens, the
	// game install folder and the runtime fill the project settings' fields (while the
	// dialog that asked is open on the project open now), a file to import is planned as
	// deliver_picks plans several.
	void deliver_pick(PickPurpose purpose, const std::string &path);
	// The shell's answer to a PickFile request that picks several files (none = cancelled): the
	// files to import planned in the import dialog (a PreviewImport raised like any window's
	// request), with the files they need when the editor's setting says so.
	void deliver_picks(PickPurpose purpose, const std::vector<std::string> &paths);
	// Files the OS dropped on the editor's window at (x, y) of its pixels (S18; the drop's own point, which
	// the Shell reads from the OS cursor when the drop arrives): held for the item they land on to take
	// this frame or the next (take_dropped_files: the point in the item's rect and its window the one under
	// the point, no window drawn over it, no modal open over it), then let go.
	void drop_files(std::vector<std::string> paths, float x, float y);

	// The Shell's devices, by document and viewport kind, the viewports' canvases draw through
	// (null: no picture).
	void set_devices(ViewportDeviceSource *devices) { devices_ = devices; }
	// The Shell's thumbnail device the texture previews draw through (null: framed boxes, S18).
	void set_thumbnail_images(TextureThumbnailImages *images) { thumbnail_images_ = images; }

	// The new-project form (the welcome view's and File > New project...'s), for a test.
	const NewProjectForm &new_project_form() const { return new_project_; }

	// Workspace
	const SessionView &view() const override;
	void request(EditorRequest request) override;
	ViewportDeviceSource *devices() const override { return devices_; }
	TextureThumbnailImages *thumbnail_images() const override { return thumbnail_images_; }
	bool take_dropped_files(float min_x, float min_y, float max_x, float max_y, std::vector<std::string> &paths) override;

	// MenuBarContributor
	void draw_menu_bar(devtools::ImGuiPass &pass) override;
	void draw_menu_bar_trailing(devtools::ImGuiPass &pass) override;

private:
	// Back and Forward, the menu bar's first items (the navigation history).
	void draw_navigation(const SessionView &v);
	void draw_file_menu(const SessionView &v);
	void draw_edit_menu(const SessionView &v, const DocumentBase *document);
	void draw_build_menu(const SessionView &v);
	void draw_new_project();
	void draw_build_panel(const SessionView &v);
	void shortcuts(const SessionView &v, const DocumentBase *document);
	// The dialog the session's order shows (shown_modal), its window brought forward as it comes to show where a
	// window draws it (Files' Rename..., Problems' confirmation): a held dialog no frame draws would hold the ones
	// after it.
	void bring_modal_forward(const SessionView &v);

	void dispatch_events();

	devtools::ImGuiPass pass_;
	const SessionView *view_ = nullptr;
	SessionView empty_;
	uint64_t dispatched_ = 0; // the seq of the last view event sent to a window
	ViewportDeviceSource *devices_ = nullptr; // the Shell's (set_devices)
	TextureThumbnailImages *thumbnail_images_ = nullptr; // the Shell's (set_thumbnail_images)
	std::deque<EditorRequest> requests_;
	std::vector<EditorRequest> deferred_; // this frame's requests that act on the files as saved
	bool in_frame_ = false;
	// The texture a Replace with image... pick is for (S18), and the files the OS dropped, held for the
	// item they land on (drop_files) for `frames` more end_frames.
	std::string replace_target_;
	int brought_ = 0; // the HeldModal last brought forward (bring_modal_forward)
	struct Dropped {
		std::vector<std::string> paths;
		float x = 0.0f, y = 0.0f;
		int frames = 0;
	};
	Dropped dropped_;
	// The new-project form's text fields (what it holds is the workspace's: new_project).
	NewProjectForm new_project_;
	ProjectSettingsDialog settings_;
	ImportDialog import_;
	NewFilePrompt new_file_;
	ProjectFind find_;
	RenameDialog rename_;
	TextureSourceDialog texture_source_;
	FilesWindow *files_window_ = nullptr; // owned by the pass
	DocumentWindow *document_window_ = nullptr; // owned by the pass
	InspectorWindow *inspector_window_ = nullptr; // owned by the pass
	PreviewWindow *preview_window_ = nullptr; // owned by the pass
	ProblemsWindow *problems_window_ = nullptr; // owned by the pass
};

} // namespace opennova::editor
