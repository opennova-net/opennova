#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <editor/preview/canvas_gesture.h>
#include <editor/preview/terrain_brush.h>
#include <editor/preview/viewport_overlay.h>
#include <editor/session/view/viewport_kind.h>
#include <formats/til/til.h>

namespace opennova::terrain {
struct SectorLayout;
}

namespace opennova::editor {

class DocumentBase;
class MissionGround;
struct SessionView;
struct ViewportContext;

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
// - Tiles: a click selects (the viewport's click), a press on a tile then a drag moves it with the selected tiles (the
//   viewport's tile drag under the host's gesture token, the tile's corner keeping its place under the pointer; Shift
//   the 16 grid), a press on a selected tile keeping the selection for the drag (a click there selects it alone); the
//   selected tiles are outlined; R, Shift+R, H, V, PgUp, PgDn and Delete turn, flip, layer and remove them (the tile
//   commands).
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
	// A stroke is under way (a press over the ground not let go yet).
	bool stroking() const { return stroking_; }
	// A press of the brush's is under way (a stroke, or a Tiles press on a tile): the host hands it every frame's input
	// until it is let go.
	bool pressing() const { return stroking_ || tile_pressing_; }
	// The frame's pointer and keys over the picture: true when the brush took them (the host does nothing more with
	// them), false when it is not live or leaves them to the host (the camera's buttons and keys). `gesture` is the
	// host's one gesture machine, `subject` what it shows, `space` the ground under its picture (null: none), `context`
	// the host's viewport's (a tile's drag is planned with it, as the host plans its own drags: a batch under the
	// gesture's token; null, no tile is dragged).
	bool input(CanvasGesture &gesture, const CanvasSubject &subject, const TerrainBrushSpace *space, const CanvasInput &in,
	           CanvasRequests &out, const ViewportContext *context = nullptr);
	// What it draws over the picture while live (its ring, its circle, a stamp's square where the stamp would lay it;
	// the selected tiles' squares; a ring at the brush's cursor where the pointer is off the picture and an MCP client
	// placed one), and the line under the picture (its hover hint, "" none). `ground`: the view's ground as the game
	// reads it, for the hint's words (null: none).
	void shapes(const TerrainBrushSpace *space, const CanvasInput &in, OverlayList &out) const;
	std::string hint(const TerrainBrushSpace *space, const CanvasInput &in, const MissionGround *ground) const;
	// Where it meets the ground under the pointer, while it is live and the pointer is over the picture.
	bool ground(const TerrainBrushSpace *space, const CanvasInput &in, TerrainPoint &at) const;
	// The brush the next dab strokes with as the keys held change it (Shift inverts), as a stroke's brush names it.
	static TerrainStrokeAsk stroke_ask(const TerrainBrush &brush, bool invert);

private:
	void send_(CanvasGesture &gesture, const std::vector<TerrainPoint> &dabs, bool invert, CanvasRequests &out);
	void keys_(CanvasGesture &gesture, const CanvasInput &in, CanvasRequests &out, bool &took);
	void command_(const char *name, std::vector<double> by, const std::string &value, CanvasRequests &out) const;
	void follow_tiles_(const SessionView &view, const DocumentBase &document);
	void select_(CanvasPoint at, CanvasJoin join, CanvasRequests &out) const;
	bool tile_input_(CanvasGesture &gesture, const CanvasSubject &subject, const TerrainBrushSpace *space,
	                 const CanvasInput &in, CanvasRequests &out, const ViewportContext *context);

	ViewportKind kind_;
	bool live_ = false;
	bool stroking_ = false;
	std::string path_;
	std::string view_path_;
	TerrainBrush brush_;
	TerrainDabPath dabs_;
	// The ground's height under a stroke's press (Flatten's target, its flat ring's height while it strokes).
	double press_height_ = 0.0;
	// The Tiles tool's: the placed tiles of the .til the view's tiles go to (its open document's, in draw order), their
	// rows and which the view selected; a press's grabbed tile, the pointer's offset on the picture from its corner,
	// whether a drag sample went out, and whether its select waits for a click (a press on a selected tile).
	TilFile tiles_;
	std::vector<NodeId> tile_ids_;
	std::vector<bool> tile_selected_;
	bool tile_pressing_ = false;
	bool tile_sent_ = false;
	bool select_on_click_ = false;
	NodeId tile_grab_ = 0;
	CanvasPoint grab_offset_;
	CanvasPoint press_mouse_;
};

// The original editor's depth pixel under a mission point (x east, y north, metres) over the terrain's sector grid
// `layout`: the atlas texel of its column, the second axis down the map one further, the quadrant its sector places
// added; (0, 0) over an empty sector [orig: JOTACmed.exe MapView_GetDepthPixelAtCursor @ 0x4228b0, shown by MapView_Present @ 0x417c57 as
// "Depth Pixel X: %3d Y: %3d"]. The sector is the game's own lookup's (coords_resolve_sector: past the grid, & 15 on
// an axis the terrain wraps, else clamped to 0..15, as MED's index is clamped where its extent mask meets it @
// 0x4229ae..0x4229c6). False where the layout has no grid.
bool med_depth_pixel(const terrain::SectorLayout &layout, bool wrap_x, bool wrap_z, TerrainPoint at, int &x, int &y);

} // namespace opennova::editor
