#pragma once

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
};

} // namespace opennova::editor
