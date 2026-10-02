#pragma once

#include <memory>

#include <editor/ui/viewport_view.h>

namespace opennova::editor {

// A mission viewport's view (ADR 0046 S14; ui/viewport_views' mission row, the Document tab's main
// view beside the outline): the mission as the game draws it (its terrain, sky, water and entities
// through the Shell's device, built over the frames with "Loading" meanwhile), its marks, areas and
// paths drawn over the picture by its canvas (preview/mission_canvas, the mission viewport's half).
// A toolbar above the canvas: the snap a move and a lift take (metres) and a turn takes (degrees),
// Stick (a move keeps each entity's height over the ground), Show (the layers the device draws, the
// marks the canvas draws, the labels, how far a mark is drawn), Time (the mission's start time or an
// hour of the day), Frame and Top (the camera on the selection or everything, straight down over its
// target), Place (an item picked from the project's catalogs, then each click on the picture places
// one of it there: an EditInViewport drop the viewport plans; Esc or Stop placing ends it), Ground
// (the selected entities set down on the ground under them), Play mission. A Files row let go over
// the picture is a drop of that file (a model, whose item the viewport finds). Every change is a
// request: the options a SetViewport, the camera's gestures SetViewports, a drag's samples EditRecord
// batches under one gesture, a click or a marquee a SelectRecord, a placing click or a drop an
// EditInViewport. Under the canvas, the files the picture asked the project for and did not find (an
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
