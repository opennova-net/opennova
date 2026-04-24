#pragma once

#include <cstdint>

namespace opennova::terrain {

// Packed colors are 0xAARRGGBB, matching the little-endian BYTE2/BYTE1/LOBYTE
// access pattern in the jodemo terrain routines.

// Engine: jodemo.exe Terrain_SetLightingColors@0x005C4B10.
// Builds the global light color later consumed by Terrain_GetModulatedColorAtPos.
uint32_t terrain_light_color_from_ambient_diffuse_argb(uint32_t ambient_argb,
                                                       uint32_t diffuse_argb) noexcept;

// Engine: jodemo.exe Terrain_GetModulatedColorAtPos@0x005C5FE0.
// Multiplies base colormap RGB by the packed light color, divides by 128
// via >> 7, clamps each channel to 255, and preserves base alpha.
uint32_t terrain_modulate_color_argb(uint32_t base_argb, uint32_t light_argb) noexcept;

} // namespace opennova::terrain
