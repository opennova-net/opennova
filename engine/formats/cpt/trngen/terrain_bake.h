#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <formats/cpt/cpt.h>

#include "quadtree_node.h"

namespace opennova::trngen {

// NovaLogic's terrain build tool, TrnGen.exe, ported (docs/terrain/terrain-re.md, the historical
// build module map): a 1024 x 1024 depth map in, the terrain's .cpt out: the depth atlas and the
// ground mesh the game draws, every quadtree node from the 1024 root down to the 64-cell leaves
// simplified into its eight LOD index lists. The port was byte-identical with TrnGen's own .cpt over
// the Sample, Gradient, Checker64 and Perlin projects when ADR 0037 retired it; it keeps its tile
// files (TrnGen writes `.tml` then `.tms` files beside the output) in memory, written and read
// through the same codec, and is otherwise the same code.
//
// Units: a depth texel is 1/256 of a world unit (raw16), one texel per world unit across. An 8-bit
// depth map is TrnGen's own input, each texel smoothed with its right, lower and lower-right
// neighbours into 32 times their sum (so a grey level is half a world unit, 0 to 127.5); a 16-bit
// one is taken as the raw16 heights it is, unsmoothed (TrnGen's raw16 path).
inline constexpr int kDepthSide = 1024;

struct TerrainBakeInput {
	// Exactly one of them, kDepthSide * kDepthSide texels, the top row first.
	std::vector<uint8_t> depth8;
	std::vector<uint16_t> depth16;
	// The header's three strings (64, 64 and 16 bytes with their NUL; longer ones are cut). TrnGen
	// writes the project's name and creator and the Windows user it ran as; the game reads none of
	// them (it reads the header's level count alone).
	std::string terrain_name;
	std::string creator;
	std::string version;
	// The quadrant lock flags (top-left, top-right, bottom-left, bottom-right): a locked axis keeps a
	// leaf's height taps inside its own 512 quadrant (TrnGen's .tpj lock_* pairs, the .trn's).
	std::array<CornerLockFlags, 4> locks{};
	// CDEP (what every shipped map carries; the game takes no other: a DPTH depth section loads no
	// terrain [orig: Terrain_LoadLodStorage @ 0x6037B2]) or DPTH (TrnGen's own output).
	DepthFormat depth_format = DepthFormat::CDEP;
	// The leaves' triangles rasterized back over the depth map (TrnGen's Output.dep), else the map
	// as read.
	bool rasterize_depth = true;
	// The threads a quadtree level's nodes are made on, the embedder's count (ADR 0049 d5; 0 is taken
	// as 1, the calling thread alone). The output is the same whatever the count (quadtree_node.h).
	unsigned threads = 1;

	// Every hardware thread, at least one: the desktop sizing. An embedder whose platform fixes its
	// thread pool up front (the web build's pthread pool) passes a smaller count.
	static unsigned for_hardware() noexcept;
};

// The 8-bit map smoothed to raw16 [orig: TrnGen.exe build_terrain_thread @ 0x4013A0].
std::vector<uint16_t> smooth_depthmap(const std::vector<uint8_t> &raw);

// The bake. False, with `error` in plain words, for an input of the wrong size or a buffer size the
// port refuses (a count so large it can only be corrupt: lod_mesh_data.h, tristrip.h).
bool bake_terrain(const TerrainBakeInput &input, CptFile &out, std::string &error);
// The bake's .cpt bytes; false, with `error`, also for a .cpt the CPT writer cannot encode (save_cpt).
bool bake_terrain_bytes(const TerrainBakeInput &input, std::vector<uint8_t> &out, std::string &error);

} // namespace opennova::trngen
