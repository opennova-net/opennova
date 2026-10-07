#pragma once

#include <string>
#include <vector>

#include <editor/graph/reference_queries.h>
#include <editor/session/editor_request.h>

namespace opennova::editor {

class TextureThumbnailImages;
class ViewportDeviceSource;
struct SessionView;
struct ViewportMouse;

// What Go to definition (F12) and Find usages (Shift+F12) act on where the pointer or the keyboard is (ADR 0046
// DI-18), offered by the window drawing it each frame it is there: a reference field what it names and whose
// uses are those of what it names, a Files row its file, a Problems row its uses. One under the pointer wins over
// one with the keyboard; the workspace's selection gives each key what the offer lacks (EditorWindows::jump_subject).
struct JumpSubject {
	std::vector<ReferenceTarget> definition; // where Go to definition leads (several: the first)
	std::string usages_file;                 // whose uses Find usages lists ("" none)
	std::string usages_locator;              // its record there ("" the file itself)
	bool pointer = false;                    // under the pointer, not only with the keyboard
	bool any() const { return !definition.empty() || !usages_file.empty(); }
};

// The editor's windows and their seam to the session (ADR 0046 d10, S13 A2; CONTEXT.md
// "Workspace"): what every window sees of the world, the session's view to read, a sink for the
// typed requests it raises, and the devices its viewports' canvases draw through (S13 V5: the
// Shell's, by document and kind; null where there are none, a headless run, a test), and the one its
// texture thumbnails draw through (S18; null likewise: a framed box in their place). The composition
// (EditorWindows) implements it; a test can implement it with a seeded view and a captured queue.
// The Shell (EditorApp) drains the requests into the session and hands the devices in.
class Workspace {
public:
	virtual ~Workspace() = default;
	virtual const SessionView &view() const = 0;
	virtual void request(EditorRequest request) = 0;
	virtual ViewportDeviceSource *devices() const = 0;
	virtual TextureThumbnailImages *thumbnail_images() const { return nullptr; }
	// Files the OS dropped on the window, taken by the item whose rectangle (the window's pixels) they
	// landed in (S18: a texture's tab and a texture field take an image as Replace): the paths, once; false
	// for none there.
	virtual bool take_dropped_files(float min_x, float min_y, float max_x, float max_y, std::vector<std::string> &paths) {
		(void)min_x, (void)min_y, (void)max_x, (void)max_y, (void)paths;
		return false;
	}
	// The system pointer hidden for this frame: a picture under the mouse draws the game's own there (a
	// menu's, DI-08), so one pointer shows. The Shell shows it again on a frame none asks.
	virtual void hide_pointer() {}
	// What F12 and Shift+F12 act on here this frame (DI-18): taken by the next frame's shortcuts.
	virtual void offer_jump(const JumpSubject &subject) { (void)subject; }
	// The mouse a canvas has over its picture this frame (DI-34: a menu's, whose sounds hear it as the game's
	// mouse): the Shell hands the frame's to the session (ProjectSession::canvas_mice). No request: it is the
	// frame's input, as the clock's time is.
	virtual void canvas_mouse(const ViewportMouse &mouse) { (void)mouse; }
};

} // namespace opennova::editor
