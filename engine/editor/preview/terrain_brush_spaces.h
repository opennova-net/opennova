#pragma once

#include <editor/preview/mission_map.h>
#include <editor/preview/model_preview_camera.h>
#include <editor/preview/terrain_brush.h>
#include <editor/preview/viewport_model.h>

namespace opennova::editor {

class MissionGround;

// The brush's spaces over the views (ADR 0046 S25): where a canvas's pointer meets the ground and where a ground point
// shows on its picture.

// A 3D view's (the mission's, the terrain's): the camera's ray through the pixel, met where the device's terrain
// meets it (ViewportDevice::surface_between, its height ground_at's), else, with no device, where the same raycast the
// Shell's device runs meets the mission's height field [orig: Terrain_RaycastHeightmapHiRes_0 @ 0x60e710, over the
// editor's coords and samples as TerrainData::raycast_terrain takes them], so an editor drawing no picture (the null
// backend, a headless MCP) lays the points a picture would; nothing where neither has a ground.
class CameraBrushSpace final : public TerrainBrushSpace {
public:
	CameraBrushSpace(const ViewportContext &context, const OrbitCamera &camera, const MissionGround *ground)
		: context_(context), camera_(camera), ground_(ground) {}
	bool pointer_ground(float x, float y, TerrainPoint &at, double &height) const override;
	bool to_picture(TerrainPoint at, double height, float &x, float &y) const override;
	double height(TerrainPoint at) const override;

private:
	const ViewportContext &context_;
	const OrbitCamera &camera_;
	const MissionGround *ground_;
};

// The 2D map's: its picture unprojected to the mission's plane (MissionMapView), the brush a circle of radius x
// pixels-per-metre; heights the mission's ground's where it is given (the hover readout), else 0.
class MapBrushSpace final : public TerrainBrushSpace {
public:
	MapBrushSpace(const MissionMapView &view, const MissionGround *ground) : view_(view), ground_(ground) {}
	bool pointer_ground(float x, float y, TerrainPoint &at, double &height) const override;
	bool to_picture(TerrainPoint at, double height, float &x, float &y) const override;
	double height(TerrainPoint at) const override;
	bool planar() const override { return true; }
	double pixels_per_metre() const override { return view_.scale > 0.0f ? 1.0 / double(view_.scale) : 0.0; }

private:
	MissionMapView view_;
	const MissionGround *ground_;
};

// The mission ground's height at a point (metres; false where it holds no terrain): the game's column height [orig:
// Terrain_RaycastHeightmapLoRes @ 0x60cc12 down the column, world::terrain_column_height].
bool mission_ground_height(const MissionGround &ground, TerrainPoint at, double &height);

} // namespace opennova::editor
