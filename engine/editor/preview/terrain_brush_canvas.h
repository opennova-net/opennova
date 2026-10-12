#pragma once

#include <string>
#include <vector>

#include <editor/preview/canvas_gesture.h>
#include <editor/preview/terrain_brush.h>
#include <editor/preview/viewport_overlay.h>
#include <editor/session/view/viewport_kind.h>

namespace opennova::editor {

class DocumentBase;
class MissionGround;
struct SessionView;

// The terrain brush's half of a canvas (ADR 0046 S25), embedded by each canvas a brush strokes in: the mission's 3D
// view (MissionCanvas), its 2D map (MissionMapCanvas) and the terrain viewport (TerrainCanvas). While it is live (its
// view document's Terrain pane shows and the brush's tool is not None) it takes the canvas's left button and the
// brush's keys before the canvas's own tools, the camera's gestures staying the canvas's (the middle and right
// buttons, Alt, the wheel, the fly keys):
// - Sculpt and Paint: a press over the ground begins a stroke under the host's gesture token, each frame raises one
//   terrain_stroke with that frame's dabs (TerrainDabPath: the press's point, then one every spacing x radius), the
//   release ends it (an EndEdit naming the token), Esc during the press cancels it (the EndEdit's cancel); Shift
//   inverts (Raise lowers, Lower raises, Foliage paints 0); Ctrl+click picks (an eyedropper command: Set and Flatten
//   take the height, a paint tool the value).
// - Stamp: a click lays the cell's tile with its corner under the pointer (a drop of the tile, Shift snapping to 16).
// - Tiles: a click selects, a drag moves (the viewport's click and its tile drag); R, Shift+R, H, V, PgUp, PgDn and
//   Delete turn, flip, layer and remove the selected tiles (the tile commands).
// - [ and ] shrink and grow the radius by a quarter, with Shift the strength by a tenth; Esc with no press puts the
//   brush down (tool none).
// Every change a request. Its space (TerrainBrushSpace) maps the pointer to the ground for the host.
class TerrainBrushHalf {
public:
	explicit TerrainBrushHalf(ViewportKind kind) : kind_(kind) {}
	// The frame's start: the session's brush, and whether it is live over the view document `document` (null: none).
	void follow(const SessionView &view, const DocumentBase *document);
	bool live() const { return live_; }
	const TerrainBrush &brush() const { return brush_; }
	// A stroke is under way (a press over the ground not let go yet), and the plane it strokes.
	bool stroking() const { return stroking_; }
	// The frame's pointer and keys over the picture: true when the brush took them (the host does nothing more with
	// them), false when it is not live or leaves them to the host (the camera's buttons and keys). `gesture` is the
	// host's one gesture machine, `subject` what it shows, `space` the ground under its picture (null: none).
	bool input(CanvasGesture &gesture, const CanvasSubject &subject, const TerrainBrushSpace *space, const CanvasInput &in,
	           CanvasRequests &out);
	// What it draws over the picture while live (its ring, its circle, a tile's square; a ring at the brush's cursor
	// where the pointer is off the picture and an MCP client placed one), and the line under the picture (its hover
	// hint, "" none). `ground`: the view's ground as the game reads it, for the hint's words (null: none).
	void shapes(const TerrainBrushSpace *space, const CanvasInput &in, OverlayList &out) const;
	std::string hint(const TerrainBrushSpace *space, const CanvasInput &in, const MissionGround *ground) const;
	// The brush the next dab strokes with as the keys held change it (Shift inverts), as a stroke's brush names it.
	static TerrainStrokeAsk stroke_ask(const TerrainBrush &brush, bool invert);

private:
	void send_(CanvasGesture &gesture, const std::vector<TerrainPoint> &dabs, bool invert, CanvasRequests &out);
	void keys_(CanvasGesture &gesture, const CanvasInput &in, CanvasRequests &out, bool &took);
	void command_(const char *name, std::vector<double> by, const std::string &value, CanvasRequests &out) const;

	ViewportKind kind_;
	bool live_ = false;
	bool stroking_ = false;
	std::string path_;
	std::string view_path_;
	TerrainBrush brush_;
	TerrainDabPath dabs_;
};

// The line under a view's picture while the brush is live (S25): in Sculpt the original editor's depth-pixel readout,
// "Height 132.4 m" [orig: JOTACmed.exe sub_417B30, its DEPTHPIXEL_MODE]; in Paint the value it lays and the ground's
// there in the game's words; for Stamp where the click lays the tile; "" for none.
std::string terrain_brush_hint(const TerrainBrush &brush, bool on_ground, TerrainPoint at, double height,
                               const MissionGround *ground);

} // namespace opennova::editor
