#pragma once

#include <string>

#include <editor/preview/canvas_gesture.h>
#include <editor/preview/terrain_brush.h>
#include <editor/preview/viewport_overlay.h>

namespace opennova::editor {

class DocumentBase;
struct SessionView;

// The terrain brush's half of a canvas (ADR 0046 S25, the shared seams), embedded by each canvas a brush strokes in:
// the mission's 3D view (MissionCanvas), its 2D map (MissionMapCanvas) and the terrain viewport (TerrainCanvas). While
// it is live (its view document's Terrain pane shows and the brush's tool is not None) it takes the canvas's left
// button and the brush's keys before the canvas's own tools, the camera's gestures staying the canvas's: a press over
// the ground begins a stroke under the host canvas's gesture token, each frame raises one terrain_stroke with that
// frame's dabs, the release ends it (end_edit naming the token) and Esc cancels it (end_edit with cancel); a stamp
// is a drop of the tile's cell, the tile tools their edit_in_viewport commands. Its space (TerrainBrushSpace) maps the
// pointer to the ground for the host.
class TerrainBrushHalf {
public:
	// The frame's start: the session's brush, and whether it is live over the view document `document` (null: none).
	void follow(const SessionView &view, const DocumentBase *document);
	bool live() const { return live_; }
	const TerrainBrush &brush() const { return brush_; }
	// A stroke is under way (a press over the ground not let go yet).
	bool stroking() const { return stroking_; }
	// The frame's pointer and keys over the picture: true when the brush took them (the host does nothing more with
	// them), false when it is not live or leaves them to the host (the camera's buttons and keys). `gesture` is the
	// host's one gesture machine, `subject` what it shows, `space` the ground under its picture (null: none).
	bool input(CanvasGesture &gesture, const CanvasSubject &subject, const TerrainBrushSpace *space, const CanvasInput &in,
	           CanvasRequests &out);
	// What it draws over the picture while live (its ring, its circle, a tile's square), and the line under the picture
	// (its hover hint, "" none).
	void shapes(const TerrainBrushSpace *space, const CanvasInput &in, OverlayList &out) const;
	std::string hint(const TerrainBrushSpace *space, const CanvasInput &in) const;

private:
	bool live_ = false;
	bool stroking_ = false;
	std::string path_;
	TerrainBrush brush_;
	TerrainDabPath dabs_;
};

} // namespace opennova::editor
