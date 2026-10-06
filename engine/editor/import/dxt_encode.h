#pragma once

#include <cstdint>
#include <vector>

namespace opennova::editor {

// The DXT textures the tools write (ADR 0046 S18, the image importer's `dds` format; ADR 0047,
// `opennova-3di texture`, the Blender add-on's model textures): tooling, not a port. The game decodes a
// DDS's blocks and keeps its file's chain (render-material-re, "The DDS reader"), so how the blocks and
// the levels are made is the author's tool's choice; the game's own DXT path, the terrain's atlas made DXT
// at load, stays the D3DX codec's port (runtime/renderer/texture_dxt).
//
// The levels: with `full_chain`, one per halving of the larger side down to 1 x 1 (dxt_full_chain_levels),
// the chain every DXT `.dds` the game ships carries (render-material-re, "The install"); else the first
// alone. Each level is the D3DX box filter of the level before it (renderer::box_filter_half
// [orig: D3DXTex::CBlt::BltBox2D @ 0x6E1100], the filter the game's texture creator halves with), taken over
// the source's texels at full precision rather than over the blocks the level before was stored as: a
// chain built from decoded blocks compounds each level's error into the next (1 dB a level on the base
// game's textures).
//
// The blocks: rgbcx's (third_party/rgbcx; level 10, its recommended setting; BC1 in its ideal decode, the
// one D3D's DXT1 sampling follows), which keeps 1.3 to 3.6 dB of PSNR over the D3DX codec's encoder on the
// base game's model textures. A DXT1 block holding a texel whose alpha is below 128 is the D3DX codec's,
// colour-keyed (renderer::encode_dxt1_block: alpha below one half takes the transparent index), so a DXT1
// texture keeps its alpha on or off as D3DX would store it; rgbcx's BC1 reads no alpha.
//
// `rgba` holds width x height texels, R, G, B, A each, the top row first. The result holds each level's
// blocks, the first level first, 4 x 4 texels a block (8 bytes for DXT1, 16 for DXT5), row by row; empty
// for an empty image.
std::vector<std::vector<uint8_t>> encode_dxt_levels(const uint8_t *rgba, uint32_t width, uint32_t height, bool dxt5,
                                                    bool full_chain);

// The levels of a full chain: one per halving of the larger side, to 1 x 1 (D3DX's full chain, MipLevels 0).
uint32_t dxt_full_chain_levels(uint32_t width, uint32_t height);

} // namespace opennova::editor
