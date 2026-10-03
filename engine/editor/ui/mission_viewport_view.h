#pragma once

#include <memory>

#include <editor/ui/viewport_view.h>

namespace opennova::editor {

// A mission viewport's view (ADR 0046 S14; ui/viewport_views' mission row, the Document tab's main
// view beside the outline): the mission as the game draws it (its terrain, sky, water and entities
// through the Shell's device, built over the frames with "Loading" meanwhile), its marks, areas and
// paths drawn over the picture by its canvas (preview/mission_canvas, the mission viewport's half).
// A toolbar above the canvas (ADR 0046 S15, Placing and tweaking): the tools, each saying in its
// tooltip what a click then does (Select; Place, its palette beside the picture: preview/
// mission_palette, an item picked by name, then each click places one, or one dragged from the
// palette onto the picture; Path stops, the paths listed beside the picture, each click adding the
// picked path's next stop; Area, a box dragged on the ground; Esc goes back to Select), the snap a
// move, a lift, a placed record and an area's edge take (metres) and a turn takes (degrees), Stick (a
// move keeps each entity's height over the ground), Show (the layers the device draws, the marks the
// canvas draws, the labels, how far a mark is drawn), Time (the mission's start time or an hour of the
// day), Frame and Top (the camera on the selection or everything, straight down over its target),
// Ground (the selected entities set down on the ground under them), Play mission. Under it, while an
// entity is the primary selected record, its place and heading as numbers (metres east, north and
// high; degrees clockwise from north), typed exactly. A Files row let go over the picture is a drop of
// that file (a model, whose item the viewport finds). The right button's click opens what applies
// there (Place here, Paste here, Frame, Drop to ground, Duplicate, Delete, Select same item, Go to in
// outline, Show events using this). Ctrl+C, Ctrl+X and Ctrl+V copy, cut and paste (Ctrl+V over the
// picture pastes there). Every change is a request: the options a SetViewport, the camera's gestures
// SetViewports, a drag's samples EditRecord batches under one gesture, a click or a marquee a
// SelectRecord, a placing click, a drop or a command an EditInViewport. Under the canvas, what a click
// does now (the canvas's hint) and the files the picture asked the project for and did not find (an
// Import mends them).
class MissionViewportView final : public ViewportView {
public:
	MissionViewportView();
	~MissionViewportView() override;

protected:
	void draw_ready(Workspace &workspace, const ViewportModel &model, ViewportContext &context) override;

private:
	struct Tools;
	std::unique_ptr<Tools> tools_;
};

// The snaps the toolbar offers (metres; 0 free) and the turns (degrees; 0 whole degrees).
inline constexpr float kMissionSnaps[] = { 0.0f, 0.25f, 1.0f, 5.0f, 10.0f };
inline constexpr float kMissionTurns[] = { 0.0f, 5.0f, 15.0f, 45.0f, 90.0f };

} // namespace opennova::editor
