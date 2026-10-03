#pragma once

#include <functional>
#include <string>
#include <vector>

#include <editor/model/node.h>
#include <editor/preview/canvas_gesture.h>
#include <editor/preview/mission_handle_edit.h>
#include <editor/preview/mission_scene.h>
#include <editor/preview/viewport_overlay.h>

namespace opennova::editor {

class ViewportDevice;
struct MissionViewportOptions;
struct OrbitCamera;

// What the mission's canvas draws over the device's picture (ADR 0046 S14), as pure functions of
// the scene, the marks and the pointer into an OverlayList: each shown mark's glyph by its pool (an
// item a diamond, a building a square, a marker a cross, an organic a dot) ringed in its team's
// colour, the hovered mark ringed, the selected marks ringed (the primary thicker) and the primary's
// handles (a line along its heading to the yaw handle's disc, a square above it for the height), an
// area's footprint (its corners at the ground where a device answers, else at its z_min; its top and
// bottom too where its flags bound z) with its edge handles when primary, a path's line through its
// stops (closed unless it does not loop; thick while it or one of its markers is selected), a label
// beside the hovered and the selected marks (beside the other shown ones with the labels option,
// decluttered: preview/mission_labels), and the marquee. A line with an end behind the near plane is
// clipped to it. At most kMissionMarksDrawn marks are drawn, the nearest. S15: the selected marks
// ringed twice over a dark ring (they read on any ground, among thousands of glyphs) and the hovered
// one ringed wide; the handle the pointer is on (or a drag holds) named beside it with its step ("Turn,
// 15 deg steps"); an Alt-drag's copies as rings where they go, a line from each original; the Area
// tool's box on the ground; and on an empty mission, a line saying how to start.

inline constexpr uint32_t kMissionItemRgb = 0xFFDC5A;
inline constexpr uint32_t kMissionBuildingRgb = 0xC0C0C0;
inline constexpr uint32_t kMissionMarkerRgb = 0x6EDCFF;
inline constexpr uint32_t kMissionOrganicRgb = 0x7CD67C;
inline constexpr uint32_t kMissionAreaRgb = 0xFFA028;
inline constexpr uint32_t kMissionBlueRgb = 0x5A8CFF;
inline constexpr uint32_t kMissionRedRgb = 0xFF5A5A;
inline constexpr uint32_t kMissionPathRgb = 0xC8C8C8;

// The colour of a team (1 blue, 2 red; 0 none, kMissionPathRgb for the rest).
uint32_t mission_team_rgb(int team);

// The direction an entity faces in the presentation frame (its yaw a compass heading, 0 north, 90
// east: world::presentation_forward_from_angles).
PreviewVec3 mission_entity_heading(const MissionEntityMark &entity);
// Where an entity's handles sit: its yaw handle `reach` metres along its heading, its height handle
// `reach` metres above it.
PreviewVec3 mission_yaw_handle(const MissionEntityMark &entity, float reach);
PreviewVec3 mission_height_handle(const MissionEntityMark &entity, float reach);
// The middle of an area's edge `edge`, at mission height `z`.
PreviewVec3 mission_area_edge_middle(const MissionAreaMark &area, MissionHandle edge, double z);
// An area's anchor height: the ground at its centre where `device` answers, else its z_min.
double mission_area_anchor_z(const MissionAreaMark &area, const ViewportDevice *device);

// One frame's input to the overlays.
struct MissionOverlayInput {
	const MissionScene *scene = nullptr;
	const MissionViewportOptions *options = nullptr;
	const OrbitCamera *camera = nullptr;
	int width = 0, height = 0;
	const std::vector<MissionMark> *marks = nullptr;
	int hover = -1; // the mark under the pointer (-1 none)
	int primary = -1; // the primary record's mark (-1 none)
	const std::vector<int> *selected = nullptr; // the other selected records' marks
	const std::vector<NodeId> *selected_rows = nullptr; // every selected record's row (a path's line thickens)
	float handle_reach = 0.0f; // metres the primary's handles stand from its anchor
	const ViewportDevice *device = nullptr; // the ground an area's corners sit on
	std::function<std::string(const NodeAddress &)> title; // a mark's label (none: no labels)
	bool marquee = false;
	CanvasPoint marquee_from, marquee_to;
	// S15: the pointer; the primary's handle it is on or a drag holds (its words and step beside it),
	// the snap and the turn a drag takes now, a drag under way.
	CanvasPoint pointer;
	bool active_handle = false;
	MissionHandle handle = MissionHandle::Move;
	float snap = 0.0f, turn = 0.0f;
	bool dragging = false;
	// An Alt-drag's copies: the records it took, as pressed, and how far their copies go (metres east
	// and north; null none).
	const std::vector<MissionPressed> *copies = nullptr;
	double copy_by[2] = { 0.0, 0.0 };
	// The Area tool's box on the ground (mission points), snapped to `box_snap` metres as its drop is.
	bool box = false;
	double box_from[3] = { 0.0, 0.0, 0.0 }, box_to[3] = { 0.0, 0.0, 0.0 };
	float box_snap = 0.0f;
};
OverlayList mission_overlay_shapes(const MissionOverlayInput &in);

// A segment from `a` to `b` projected, its end behind the near plane clipped to it: false when both
// ends are behind it.
bool mission_project_segment(const OrbitCamera &camera, int width, int height, const PreviewVec3 &a,
		const PreviewVec3 &b, CanvasPoint &from, CanvasPoint &to);

} // namespace opennova::editor
