#pragma once

#include <cstdint>

namespace opennova::terrain {

// Packed colors are 0xAARRGGBB, matching the little-endian BYTE2/BYTE1/LOBYTE
// access pattern in the Jointops.exe terrain routines.

// [orig: jodemo Terrain_SetLightingColors @0x5C4B10]
// [orig: Terrain_InitLightingColorRamps @0x604ee0 — per channel blend = ambient*0.707 + sun
//  @0x604f6f..0x604f83, ratio = sun/blend (1.0 when blend == 0) packed ARGB @0x605182..0x60524c]
// Builds the global light color later consumed by terrain_modulate_color_argb.
uint32_t terrain_light_color_from_ambient_diffuse_argb(uint32_t ambient_argb,
                                                       uint32_t diffuse_argb) noexcept;

// Foliage detail-tier parity helper (jodemo Foliage_BuildGeometry @0x5BF5F0 was the
// pre-retail anchor): the nibble-split four-sample colormap average
// [orig: Foliage_GenerateInstances_0 @0x5ffdd0 — four Terrain_SampleColorMapTinted taps at
//  +-0x8000 @0x6001a3..0x6001eb, alpha sum >>2 @0x60021c, nibble split @0x600278].
uint32_t terrain_average_four_argb(uint32_t c0, uint32_t c1, uint32_t c2, uint32_t c3) noexcept;

// [orig: jodemo Terrain_GetModulatedColorAtPos @0x5C5FE0]
// [orig: Terrain_SampleColorMapTinted @0x606030 — (base*light)>>7 per channel clamped
//  to 255 @0x60606b..0x6060c2, alpha passthrough @0x6060c6]
// Multiplies base colormap RGB by the packed light color, divides by 128
// via >> 7, clamps each channel to 255, and preserves base alpha.
uint32_t terrain_modulate_color_argb(uint32_t base_argb, uint32_t light_argb) noexcept;

} // namespace opennova::terrain
