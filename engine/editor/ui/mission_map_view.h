#pragma once

#include <editor/model/value.h>
#include <editor/ui/viewport_view.h>

namespace opennova::editor {

// The mission's 2D map's view (ADR 0046 S23 C; preview/mission_map.h): its toolbar (Frame, the CMAP's zoom in and
// out, the CMAP's Grid and Text, the marks by kind, the labels, Stick, Snap), then the canvas filling the rest
// (preview/mission_map_canvas: a click selects a pin, a box selects what it holds, a drag moves the selection, the
// right button pans, the wheel zooms). Every change of the state a SetViewport; the selection is the document's,
// shared with the 3D view. S24: the right button's click opens the menu over the pin under it (selected first unless
// it is): Frame, Duplicate, Delete, then the record's own items the 3D view's menu has (ui/mission_mark_menu).
class MissionMapViewportView final : public ViewportView {
public:
	MissionMapViewportView();

protected:
	void draw_ready(Workspace &workspace, const ViewportModel &model, ViewportContext &context) override;

private:
	// What the right-click menu was opened over: the pin's record (row 0 none) and the event the selection held.
	NodeAddress menu_record_;
	NodeAddress menu_event_;
};

} // namespace opennova::editor
