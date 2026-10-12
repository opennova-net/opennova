#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <formats/cpt/cpt.h>

#include "terrain_bake_state.h"

namespace opennova::trngen {

// A region of a depth map baked again: the bytes a whole bake of the new map makes (terrain_bake.h),
// made by remaking only the nodes the edit reaches. TrnGen.exe has no such pass (its tile cache is all or
// nothing, terrain_bake_state.h); this one remakes what its walk would remake and keeps the rest:
//
// - the leaves whose height taps read an edited texel (a leaf's 65 x 65 vertices read the map at its
//   corner plus 0 to 64 on each axis, wrapping at 1024, a locked axis of its quadrant within its 512
//   [orig: TrnGen.exe build_mesh_lod_vertices @ 0x4042B0 (the port's build)]), made from the new map;
// - their ancestors up to the 512 quadrants, level by level, each merging its four children's LOD 14
//   sections (a remade child's own, an untouched child's kept one) [orig: TrnGen.exe
//   QuadtreeNode_Process @ 0x403470, the merge of sub_409FA0 (the port's build)]; the root is never remade,
//   its tile being leaf (0, 0)'s (the export reads the leaves' level for it), but its merge is checked as the
//   whole bake checks it (merged_faces_sound, D-TERRAIN-22), so a map the whole bake refuses is refused here;
// - the remade nodes' tiles, each through the TPM1 codec and the strip pass as the bake makes them;
// - the draw-back over R, the remade leaves' footprints: the new map there, then every leaf whose footprint
//   meets R drawn back in the walk's order (nw, ne, sw, se), its writes outside R dropped [orig: TrnGen.exe
//   sub_402A20 @ 0x402A20, called for each leaf in the walk (the port's build)]. A leaf writes only inside its
//   footprint and a texel outside R keeps what the leaves there wrote before, so the atlas is the whole bake's.
// The cites marked "the port's build" follow the TrnGen.exe the port was made from; those marked "the
// MiscTools build" the one served beside it (terrain-re.md, the TrnGen provenance item).

// A rectangle of depth-map texels: x in [x, x + w), y in [y, y + h).
struct DepthRect {
	int x = 0;
	int y = 0;
	int w = 0;
	int h = 0;

	bool empty() const noexcept { return w <= 0 || h <= 0; }
	bool meets(const DepthRect &other) const noexcept {
		return !empty() && !other.empty() && x < other.x + other.w && other.x < x + w && y < other.y + other.h &&
		       other.y < y + h;
	}
};

// The leaves (as .cpt tile indices, ascending) whose height taps read a texel of any of `rects`, under the
// quadrants' lock flags (top-left, top-right, bottom-left, bottom-right).
std::vector<int> leaves_tapping(const std::vector<DepthRect> &rects, const std::array<CornerLockFlags, 4> &locks);

// A leaf's footprint, the texels its draw-back can write: its 65 x 65 vertices' square, cut at the map's
// last row and column (the draw-back clamps to 1023 and does not wrap).
DepthRect leaf_footprint(int tile_index) noexcept;

struct RegionBakeInput {
	// The whole new depth map, raw16, kDepthSide x kDepthSide, the top row first (an 8-bit map smoothed
	// first: smooth_depthmap).
	const std::vector<uint16_t> *depth16 = nullptr;
	// The texels of `depth16` changed since the state's bake (any overlap; a texel twice is the same edit), each
	// rect inside the map: an edit across its wrap arrives as several rects. An edit of an 8-bit map at (x, y)
	// changes the smoothed texels (x - 1 .. x, y - 1 .. y), wrapping (smooth_depthmap's 2 x 2 box). A rect not
	// inside the map is refused.
	std::vector<DepthRect> dirty;
	// The threads a level's nodes are made on, the embedder's count (0 is taken as 1).
	unsigned threads = 1;
	// Read between nodes and before the tile and draw-back passes: set, the bake stops and reports "cancelled".
	const std::atomic<bool> *cancel = nullptr;
};

// The time a level's nodes took (the region bake's bench).
struct RegionBakeLevelCost {
	int nodes = 0;
	double seconds = 0.0;
};

struct RegionBakeResult {
	std::vector<int> rebuilt;                  // the remade tiles' indices, ascending (0 when leaf (0, 0) is remade)
	std::vector<CptTile> tiles;                // their tiles, in `rebuilt`'s order
	std::vector<std::shared_ptr<const NodeBakeState>> nodes; // their state, in `rebuilt`'s order (the root's null)
	std::vector<DepthRect> footprints;         // R: the remade leaves' footprints
	std::vector<std::vector<uint16_t>> atlas;  // the atlas over each footprint, its rows top first
	std::array<RegionBakeLevelCost, 5> cost{}; // by depth
	double tile_seconds = 0.0;                 // the remade nodes' tiles (the strip pass)
	double draw_back_seconds = 0.0;            // the draw-back over R
	double seconds = 0.0;                      // the whole region bake
};

// The region bake over `state` (bake_terrain_state's, or an earlier region bake's applied over it). The state
// is only read: a failure or a cancel leaves it as it was. False, with `error`, for a state not made or made
// without the draw-back (TerrainBakeInput::rasterize_depth), a map of the wrong size, a dirty rect not inside it,
// ground the whole bake refuses (D-TERRAIN-22), a buffer the port refused, or "cancelled".
bool bake_terrain_region(const TerrainBakeState &state, const RegionBakeInput &input, RegionBakeResult &out,
                         std::string &error);

// Lands a region bake in the state it was made over (or that state as earlier region bakes left it): the remade
// nodes' parts replace theirs (a state not made is left as it is).
void apply_region_bake_state(const RegionBakeResult &result, TerrainBakeState &state);
// Lands a region bake in the .cpt the state's bake made, as earlier region bakes left it: the remade tiles in
// their places and the atlas over the footprints. False, nothing changed, with `error`, for a file not of the
// bake's 341 tiles and 1024 x 1024 atlas.
bool apply_region_bake_cpt(const RegionBakeResult &result, CptFile &cpt, std::string &error);

} // namespace opennova::trngen
