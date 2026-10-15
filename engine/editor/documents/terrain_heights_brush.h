#pragma once

#include <cstdint>
#include <vector>

#include <editor/documents/terrain_heights_document.h>
#include <editor/model/raster_document.h>

namespace opennova::editor {

// The heights' brush operators (ADR 0046 S25, the heights lane): what one stroke sample does to the input plane of a
// terrain's heights (TrnGen's depth input), dab by dab. UI over the game's data, never a terrain model: a dab weighs
// the atlas texels the game routes its disc of the mission to (the planner's: session/terrain_stroke_heights), each
// texel once at its strongest weight (ground a tiled layout repeats takes the strongest of its repeats), and the
// operators change the raw16 heights there (1/256 of a world unit a step):
//
// - Raise / Lower: h +/- strength x 256 x w (strength world units a dab at full weight);
// - Smooth: towards S, the mean of TrnGen's own 2 x 2 smoothing kernel at the texel and its mirror ((h + h east +
//   h south + h south-east) >> 2 [orig: TrnGen.exe build_terrain_thread @ 0x4013A0, its 8-bit map's smoothing], and
//   the same box to the west and north, so that a feature does not drift), read from the heights as the dab found
//   them, the neighbours wrapping at 1024 as the kernel does: h + (S - h) x strength x w;
// - Flatten: towards the live height under the stroke's first sample (the planner's target);
// - Set: towards a typed height, strength 1 an exact plateau.
//
// Each result rounds to the nearest raw step, kept within [0, top] (65535, or an imported terrain's white). The
// regions a sample changed join the side state's regions whose bake is due.
enum class HeightsOperator : uint8_t { Raise, Lower, Smooth, Flatten, Set };

struct HeightsDabTexel {
	int32_t x = 0, z = 0; // the atlas texel, column and row
	float weight = 0.0f;
};

struct HeightsStrokeEdit : RasterEditPayload {
	HeightsOperator op = HeightsOperator::Raise;
	float strength = 0.5f;
	int32_t target = 0;    // Flatten, Set: raw16
	int32_t top = 65535;   // the clamp, raw16
	std::vector<std::vector<HeightsDabTexel>> dabs;

	const char *token() const override { return "heights.stroke"; }
	bool apply(RasterCanvas &canvas, Diagnostic &error) const override;
};

// The regions of the atlas a set of texels lies in: each 64 x 64 leaf cell's texels as their bounding rectangle, the
// rectangles of a row of cells joined where they meet (the regions whose bake is due).
std::vector<HeightsRect> heights_regions(const std::vector<HeightsDabTexel> &texels);

// The due regions with `added` joined: a rectangle inside one already due adds nothing.
void join_heights_regions(std::vector<HeightsRect> &due, const std::vector<HeightsRect> &added);

} // namespace opennova::editor
