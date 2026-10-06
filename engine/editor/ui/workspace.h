#pragma once

#include <string>
#include <vector>

#include <editor/session/editor_request.h>

namespace opennova::editor {

class TextureThumbnailImages;
class ViewportDeviceSource;
struct SessionView;

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
};

} // namespace opennova::editor
