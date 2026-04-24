#pragma once

#include <cstdint>

namespace opennova::terrain {

// Packed colors are 0xAARRGGBB, matching the little-endian BYTE2/BYTE1/LOBYTE
// access pattern in the Jointops.exe terrain routines.

// Engine: Jointops.exe Terrain_SetLightingColors@0x005C4B10.
// Builds the global light color later consumed by Terrain_GetModulatedColorAtPos.
uint32_t terrain_light_color_from_ambient_diffuse_argb(uint32_t ambient_argb,
                                                       uint32_t diffuse_argb) noexcept;

// Engine: Jointops.exe Render_SetFogParams@0x0054B4B0 ->
// Render_ConfigureFog@0x005F9890.
// fog_type 0 is exponential with density ln(64) / end. Types 1/2/3 are
// linear; type 2 starts at 0.5*end and type 3 at 0.25*end.
float terrain_fog_start_for_type(float fog_end, int fog_type) noexcept;
float terrain_fog_factor_for_distance(float distance, float fog_end, int fog_type) noexcept;

// Engine: Jointops.exe Foliage_BuildGeometry@0x005BF5F0.
// Averages four packed ARGB samples with the nibble-preserving expression used
// after the four Terrain_GetModulatedColorAtPos calls.
uint32_t terrain_average_four_argb(uint32_t c0, uint32_t c1, uint32_t c2, uint32_t c3) noexcept;

// Engine: Jointops.exe Terrain_GetModulatedColorAtPos@0x005C5FE0.
// Multiplies base colormap RGB by the packed light color, divides by 128
// via >> 7, clamps each channel to 255, and preserves base alpha.
uint32_t terrain_modulate_color_argb(uint32_t base_argb, uint32_t light_argb) noexcept;

} // namespace opennova::terrain
