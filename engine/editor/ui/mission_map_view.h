#pragma once

#include <editor/ui/viewport_view.h>

namespace opennova::editor {

// The mission's 2D map's view (ADR 0046 S23 C; preview/mission_map.h): its toolbar (Frame, the CMAP's zoom in and
// out, the CMAP's Grid and Text, the marks by kind, the labels, Stick, Snap), then the canvas filling the rest
// (preview/mission_map_canvas: a click selects a pin, a box selects what it holds, a drag moves the selection, the
// right button pans, the wheel zooms). Every change of the state a SetViewport; the selection is the document's,
// shared with the 3D view.
class MissionMapViewportView final : public ViewportView {
public:
	MissionMapViewportView();

protected:
	void draw_ready(Workspace &workspace, const ViewportModel &model, ViewportContext &context) override;
};

} // namespace opennova::editor
