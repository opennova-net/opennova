#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <editor/documents/terrain_planes.h>

namespace opennova::io {
struct JsonValue;
}

namespace opennova::editor {

// The terrain tools (ADR 0046 S25, the shared seams; CONTEXT.md "Terrain brush"): one brush the whole session
// shares (WorkspaceView::terrain_brush), which the mission's 3D view, its 2D map and the terrain viewport stroke
// with while their document's Terrain pane shows (DocumentView::terrain) and its tool is not None. A brush is UI
// over the game's data, never a terrain model: a stroke edits the file the game reads that plane from.

// The tools, by the pane's tabs: Sculpt (Raise, Lower, Smooth, Flatten, Set: the heights), Paint (Surface,
// Foliage, Colour, Detail: the maps) and Tiles (Stamp, and Tiles: select, move, turn and flip the .til's tiles).
// None: the views take their mission tools again.
enum class TerrainTool : uint8_t {
	None,
	Raise,
	Lower,
	Smooth,
	Flatten,
	Set,
	Surface,
	Foliage,
	Colour,
	Detail,
	Stamp,
	Tiles,
	kCount,
};
inline constexpr size_t kTerrainToolCount = static_cast<size_t>(TerrainTool::kCount);
enum class TerrainToolFamily : uint8_t { None, Sculpt, Paint, Tiles };

// A tool's token ("none", "raise", ..., "stamp", "tiles"), the tool a token names, its button's words ("Raise",
// ..., "Stamp", "Select"), its tab, and the plane its strokes edit (false for None and the tile tools).
const char *terrain_tool_token(TerrainTool tool);
bool terrain_tool_from_token(const std::string &token, TerrainTool &out);
const char *terrain_tool_label(TerrainTool tool);
TerrainToolFamily terrain_tool_family(TerrainTool tool);
const char *terrain_tool_family_label(TerrainToolFamily family);
bool terrain_tool_plane(TerrainTool tool, TerrainPlane &out);

// The brush's bounds (what set_workspace and a stroke's brush refuse past): its radius, metres (0: one texel of
// the plane, MED's one-texel click), the strength and the falloff (shares, 0 to 1), the spacing (a dab every
// spacing x radius along the pointer's ground path), a Set height (world units, TrnGen's raw / 256).
inline constexpr float kTerrainRadiusMost = 512.0f;
inline constexpr float kTerrainSpacingLeast = 0.05f;
inline constexpr float kTerrainSpacingMost = 2.0f;
inline constexpr float kTerrainHeightMost = 255.99609375f; // 65535 / 256

// The brush (WorkspaceView::terrain_brush): its tool, its shape (radius, strength, falloff, spacing) and the
// value each tool lays (Set's height, the surface class, the foliage code, the colour 0xRRGGBB, the detail layer,
// the stamped tile's cell, its quarter turns and flips), and where an MCP client shows its ring with no pointer
// (cursor, mission metres east and north; none while has_cursor is false).
struct TerrainBrush {
	TerrainTool tool = TerrainTool::Raise;
	float radius = 16.0f;
	float strength = 0.5f;
	float falloff = 0.5f;
	float spacing = 0.25f;
	float height = 0.0f;
	int surface = 1;
	int foliage = 0;
	uint32_t colour = 0xFFFFFF;
	int layer = 0;
	int cell = 0;
	int rotation = 0;
	bool flip_h = false;
	bool flip_v = false;
	bool has_cursor = false;
	double cursor[2] = {0.0, 0.0};
	bool operator==(const TerrainBrush &o) const {
		return tool == o.tool && radius == o.radius && strength == o.strength && falloff == o.falloff &&
		       spacing == o.spacing && height == o.height && surface == o.surface && foliage == o.foliage &&
		       colour == o.colour && layer == o.layer && cell == o.cell && rotation == o.rotation &&
		       flip_h == o.flip_h && flip_v == o.flip_v && has_cursor == o.has_cursor && cursor[0] == o.cursor[0] &&
		       cursor[1] == o.cursor[1];
	}
	bool operator!=(const TerrainBrush &o) const { return !(*this == o); }
};

// The brush's members by their tokens (tool, radius, strength, falloff, spacing, height, surface, foliage, colour,
// layer, cell, rotation, flip_h, flip_v, cursor), one bit each: what a stroke's brush names of its own (the rest
// the workspace's).
enum TerrainBrushMember : uint32_t {
	kBrushTool = 1u << 0,
	kBrushRadius = 1u << 1,
	kBrushStrength = 1u << 2,
	kBrushFalloff = 1u << 3,
	kBrushSpacing = 1u << 4,
	kBrushHeight = 1u << 5,
	kBrushSurface = 1u << 6,
	kBrushFoliage = 1u << 7,
	kBrushColour = 1u << 8,
	kBrushLayer = 1u << 9,
	kBrushCell = 1u << 10,
	kBrushRotation = 1u << 11,
	kBrushFlipH = 1u << 12,
	kBrushFlipV = 1u << 13,
	kBrushCursor = 1u << 14,
};
// The brush on the wire: {tool, radius, strength, falloff, spacing, height, surface, foliage, colour "#RRGGBB",
// layer, cell, rotation, flip_h, flip_v, cursor [x, y] | null}; `only` (bits) writes those members alone.
io::JsonValue terrain_brush_to_json(const TerrainBrush &brush, uint32_t only = ~0u);
// The members `json` names set in `brush` (each checked first: nothing set when one is refused), `named` taking
// their bits; false with why for a member it has not, a value of another type or past its bounds. `strokes`: a
// stroke's brush (no cursor, and no tile member).
bool read_terrain_brush(const io::JsonValue &json, TerrainBrush &brush, uint32_t &named, bool strokes, std::string &error);

// What a stroke asks (the terrain_stroke request, S25): the plane it edits, the brush it strokes with (the
// workspace's, but for the members `brush_named` names of `brush`), the points its dabs lie on (mission metres
// east and north, 1 to kTerrainStrokePoints, in the order the pointer went), and its gesture: 0 begins one (the
// answer names the token), a token the stroke goes on with; `end` ends it with this sample (its steps one undo
// step), `cancel` ends it dropping what it did.
inline constexpr size_t kTerrainStrokePoints = 256;
struct TerrainStrokeAsk {
	TerrainPlane plane = TerrainPlane::Heights;
	TerrainBrush brush;
	uint32_t brush_named = 0;
	std::vector<std::pair<double, double>> points;
	uint64_t gesture = 0;
	bool end = true;
	bool cancel = false;
	bool operator==(const TerrainStrokeAsk &o) const {
		return plane == o.plane && brush_named == o.brush_named && (brush_named == 0 || brush == o.brush) &&
		       points == o.points && gesture == o.gesture && end == o.end && cancel == o.cancel;
	}
	bool operator!=(const TerrainStrokeAsk &o) const { return !(*this == o); }
	bool asked() const { return !points.empty() || cancel; }
};
// The brush a stroke strokes with: `workspace`'s, the members the ask names taken from it. (Its wire form:
// session/terrain_stroke.h.)
TerrainBrush terrain_stroke_brush(const TerrainBrush &workspace, const TerrainStrokeAsk &ask);

// What a stroke came to (the request's outcome, S25): the regions it changed in each file (the plane's texels,
// [x0, x1) x [y0, y1)), the missions whose unlocked entities followed the ground and how many, the documents it
// opened quietly, and its bake's state in words ("" none).
struct TerrainStrokeRegion {
	std::string path;
	TerrainPlane plane = TerrainPlane::Heights;
	int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};
struct TerrainStrokeFollowed {
	std::string path;
	size_t entities = 0;
};
struct TerrainStrokeAnswer {
	std::vector<TerrainStrokeRegion> changed;
	std::vector<TerrainStrokeFollowed> followed;
	std::vector<std::string> opened;
	std::string bake;
	bool empty() const { return changed.empty() && followed.empty() && opened.empty() && bake.empty(); }
};

} // namespace opennova::editor
