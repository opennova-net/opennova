#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <editor/documents/terrain_planes.h>
#include <editor/model/edit.h>
#include <editor/session/terrain_family.h>
#include <editor/session/view/terrain_tools_view.h>

namespace opennova::editor {

class DocumentBase;
class SessionCore;

// What a plane's planner reads of one sample of a stroke (ADR 0046 S25, the shared seams; session/terrain_stroke's
// server makes it): the session, the view document the stroke is in and its family, the plane and its file's
// document (opened quietly, never an import's output: its source in its place), the brush as the stroke strokes
// with it, the sample's points (mission metres east and north, in the pointer's order) and the gesture its edits
// carry, which every step of the stroke shares (its link) and whose first sample `first` is.
struct TerrainStrokeContext {
	SessionCore &core;
	const DocumentBase &view;
	const TerrainFamily &family;
	TerrainPlane plane = TerrainPlane::Heights;
	const TerrainFamilyFile &file;
	DocumentBase &target;
	TerrainBrush brush;
	const std::vector<std::pair<double, double>> &points;
	uint64_t gesture = 0;
	bool first = false;
};

// What a planner makes of the sample: the batch the file's document takes (Apply edits whose payload its type made,
// each carrying the stroke's gesture: the server sets it), the step's words ("Raised the ground in dvxi5.cpt": what
// Undo says), the regions it changes (the stroke's answer) and its bake's state in words ("" none).
struct TerrainStrokePlan {
	std::vector<Edit> edits;
	std::string words;
	std::vector<TerrainStrokeRegion> changed;
	std::string bake;
};

// The planners, one per lane's planes, each in its own file (the round's anchors): the heights' (lane A,
// session/terrain_stroke_heights.cpp) and the maps' (lane B, session/terrain_stroke_paint.cpp: the surface, foliage,
// colour and blend maps). False, nothing planned, with why (a terrain.stroke refusal).
bool plan_heights_stroke(TerrainStrokeContext &context, TerrainStrokePlan &plan, std::string &error);
bool plan_paint_stroke(TerrainStrokeContext &context, TerrainStrokePlan &plan, std::string &error);

} // namespace opennova::editor
