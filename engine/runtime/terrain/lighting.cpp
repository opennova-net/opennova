#include <runtime/terrain/lighting.h>

// [orig: Terrain_InitLightingColorRamps @0x604ee0; Terrain_SampleColorMapTinted @0x606030;
//  Foliage_GenerateInstances_0 @0x5ffdd0 — lighting.h carries the per-function sites]

#include <algorithm>
#include <cmath>

namespace opennova::terrain {
namespace {

// The float32 literal retail multiplies the ambient term with. NOT the
// 0xB5/256 byte weight of the env blend (env_render's kTerrainLightWeightByte)
// and NOT a sun-direction component: three different 0.707s, one per home.
constexpr float kTerrainAmbientWeightF = 0.70700002f;

inline uint8_t byte_from_channel(uint32_t argb, int shift) noexcept {
	return static_cast<uint8_t>((argb >> shift) & 0xFFu);
}

inline uint8_t light_ratio_byte(uint8_t ambient, uint8_t diffuse) noexcept {
	const float ambient_f = static_cast<float>(ambient) * (1.0f / 255.0f);
	const float diffuse_f = static_cast<float>(diffuse) * (1.0f / 255.0f);
	const float combined = ambient_f * kTerrainAmbientWeightF + diffuse_f;
	float ratio = 1.0f;
	if (combined != 0.0f) {
		ratio = diffuse_f / combined;
	}
	const int byte_value = static_cast<int>(ratio * 255.0f);
	return static_cast<uint8_t>(std::clamp(byte_value, 0, 255));
}

inline uint8_t modulated_channel(uint8_t base, uint8_t light) noexcept {
	const unsigned int value = (static_cast<unsigned int>(base) * static_cast<unsigned int>(light)) >> 7;
	return static_cast<uint8_t>(value > 0xFFu ? 0xFFu : value);
}

} // namespace

uint32_t terrain_light_color_from_ambient_diffuse_argb(uint32_t ambient_argb,
                                                       uint32_t diffuse_argb) noexcept {
	const uint8_t r = light_ratio_byte(byte_from_channel(ambient_argb, 16),
	                                  byte_from_channel(diffuse_argb, 16));
	const uint8_t g = light_ratio_byte(byte_from_channel(ambient_argb, 8),
	                                  byte_from_channel(diffuse_argb, 8));
	const uint8_t b = light_ratio_byte(byte_from_channel(ambient_argb, 0),
	                                  byte_from_channel(diffuse_argb, 0));
	return 0xFF000000u | (static_cast<uint32_t>(r) << 16) |
	       (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}

uint32_t terrain_average_four_argb(uint32_t c0, uint32_t c1, uint32_t c2, uint32_t c3) noexcept {
	const uint32_t low =
	    ((((c0 & 0x000F0F0Fu) + (c1 & 0x000F0F0Fu) + (c2 & 0x000F0F0Fu) + (c3 & 0x000F0F0Fu)) >> 2) &
	     0x000F0F0Fu);
	const uint32_t high =
	    ((((c0 & 0x00F0F0F0u) + (c1 & 0x00F0F0F0u) + (c2 & 0x00F0F0F0u) + (c3 & 0x00F0F0F0u)) >> 2) &
	     0x00F0F0F0u);
	const uint32_t alpha = (((c0 >> 24) + (c1 >> 24) + (c2 >> 24) + (c3 >> 24)) >> 2) << 24;
	return alpha | high | low;
}

uint32_t terrain_modulate_color_argb(uint32_t base_argb, uint32_t light_argb) noexcept {
	const uint8_t a = byte_from_channel(base_argb, 24);
	const uint8_t r = modulated_channel(byte_from_channel(base_argb, 16),
	                                    byte_from_channel(light_argb, 16));
	const uint8_t g = modulated_channel(byte_from_channel(base_argb, 8),
	                                    byte_from_channel(light_argb, 8));
	const uint8_t b = modulated_channel(byte_from_channel(base_argb, 0),
	                                    byte_from_channel(light_argb, 0));
	return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) |
	       (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}

} // namespace opennova::terrain
