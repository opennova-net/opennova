#pragma once

#include <vector>

#include <editor/session/view/terrain_tools_view.h>

namespace opennova::editor {

// The terrain brush's math (ADR 0046 S25, the shared seams; its data is session/view/terrain_tools_view.h's): what a
// dab weighs at a distance from its centre and where a pointer's path lays its dabs. UI over the game's data, never a
// terrain model: the planners lay the dabs on the plane's own file (session/terrain_stroke_planes.h).

// A point on the ground, mission metres east and north.
struct TerrainPoint {
	double x = 0.0;
	double y = 0.0;
};

// A dab's weight `distance` metres from its centre: 1 within radius x (1 - falloff), then a smoothstep down to 0 at the
// radius, 0 past it. A categorical plane's dab (a surface class, a foliage code: terrain_plane_categorical) is
// hard-edged: 1 where the texel's centre lies within the radius, else 0. A radius of 0 (one texel, the original
// editor's click) weighs 1 at its centre alone.
float terrain_brush_weight(const TerrainBrush &brush, double distance, bool categorical);

// How far apart a stroke's dabs lie, metres: spacing x radius, never closer than half a texel (`texel`, the plane's
// texel in metres).
double terrain_dab_step(const TerrainBrush &brush, double texel);

// The dabs along a pointer's path on the ground: the press's first point always one, then one every `step` metres along
// the path, the distance walked since the last carried from one frame's path to the next. No airbrush: a pointer held
// still lays nothing more, so a stroke is its path (replayable on the wire as its points).
class TerrainDabPath {
public:
	// The press: its point the first dab.
	void begin(TerrainPoint at, std::vector<TerrainPoint> &dabs);
	// The pointer moved on to `at`: the dabs the path from the last point lays.
	void to(TerrainPoint at, double step, std::vector<TerrainPoint> &dabs);
	bool started() const { return started_; }
	void reset() { *this = TerrainDabPath(); }

private:
	bool started_ = false;
	TerrainPoint last_;
	double walked_ = 0.0; // since the last dab
};

// Where a view's pointer meets the ground and where a ground point shows (S25): the brush's space over one canvas.
// The mission's 3D view asks its device's ray (and, with no device, the game's own march over the mission's height
// field, so the null-backend editor and a headless MCP lay identical points); the 2D map unprojects (a planar
// space); the terrain viewport asks its device.
class TerrainBrushSpace {
public:
	virtual ~TerrainBrushSpace() = default;
	// The ground under the picture's pixel (x, y): mission metres east, north and the ground's height; false off it.
	virtual bool pointer_ground(float x, float y, TerrainPoint &at, double &height) const = 0;
	// The picture's pixel of a point at a height; false where it does not show (behind the camera).
	virtual bool to_picture(TerrainPoint at, double height, float &x, float &y) const = 0;
	// The ground's height at a point (0 where no terrain lies).
	virtual double height(TerrainPoint at) const = 0;
	// A planar space (the 2D map): its pixels per metre, the brush a circle on it.
	virtual bool planar() const { return false; }
	virtual double pixels_per_metre() const { return 0.0; }
};

} // namespace opennova::editor
