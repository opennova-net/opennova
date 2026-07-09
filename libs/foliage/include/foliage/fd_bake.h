#pragma once

// The ":fd" foliage texture bake, ported from retail Jointops.exe. Both
// foliage tiers (the far quads AND the near model draw) texture with this
// bake of the model's OWN diffuse: the alpha channel is smoothed by a 3x3
// kernel and the RGB is flattened to exactly 0x808080 - the foliage look's
// color comes entirely from the lighting/colormap combine, never from the
// diffuse RGB. Registered in retail as "<texture>:fd".
// [orig: Foliage_LoadDefAssets @ 0x601260 tail ->
// GTexture_CreateFromPixelDataWithAlphaBlend @ 0x687270; bound by
// Foliage_DrawModelTileSlot @ 0x601d90 and the quad tier alike]
// Witness record: docs/foliage/foliage-re.md §Def-asset load and the :fd bake.

#include <cstdint>

namespace opennova::foliage {

// In-place :fd bake over RGBA8 pixel data (4 bytes per pixel, R,G,B,A byte
// order; the fold writes the VALUE 0x80 into each of the three color bytes,
// so channel order does not matter).
//
// - alpha' = (4*center + 1*(N+S+E+W) + 2*(4 corners)) >> 4, neighbors read
//   from the ORIGINAL alpha plane with power-of-two wraparound
//   ((w-1) & x / (h-1) & y masks);
// - RGB flattened to exactly 0x808080 (retail: out = 0x808080 |
//   (px & 0xFF808080) - per channel (c & 0x80) | 0x80 = 0x80 always);
// - alpha byte = the smoothed value.
//
// Returns false (buffer untouched) unless width and height are both
// positive powers of two - the retail wraparound masks assume it.
bool bake_fd_rgba(uint8_t *pixels, int width, int height) noexcept;

} // namespace opennova::foliage
