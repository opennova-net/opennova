#pragma once

#include <editor/session/editor_request.h>

namespace opennova::editor {

class MenuPreviewViewport;
class ModelPreviewViewport;
struct SessionView;

// The devices the Shell renders the previews through (ADR 0046 d11): the menu's and the model's
// offscreen viewports, each null where the Shell has none (a headless run, a test). S13 V5's
// viewport seam replaces the pair with a device source keyed by kind.
struct WorkspaceDevices {
	MenuPreviewViewport *menu = nullptr;
	ModelPreviewViewport *model = nullptr;
};

// The editor's windows and their seam to the session (ADR 0046 d10, S13 A2; CONTEXT.md
// "Workspace"): what every window sees of the world, the session's view to read, a sink for the
// typed requests it raises, and the devices its previews draw through. The composition
// (EditorWindows) implements it; a test can implement it with a seeded view and a captured
// queue. The Shell (EditorApp) drains the requests into the session and hands the devices in.
class Workspace {
public:
	virtual ~Workspace() = default;
	virtual const SessionView &view() const = 0;
	virtual void request(EditorRequest request) = 0;
	virtual const WorkspaceDevices &devices() const = 0;
};

} // namespace opennova::editor
