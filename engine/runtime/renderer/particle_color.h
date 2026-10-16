#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace opennova::renderer {

// Deterministic UNORM conversion for caller-authored color/tint values. Keeping
// every float-to-integer cast inside the proven finite range avoids C++ UB for
// NaN, infinities, and hostile PTL values.
inline std::uint8_t particle_unit_byte(float value) noexcept {
	if (std::isnan(value) || value <= 0.0f)
		return 0;
	if (value >= 1.0f)
		return 255;
	return static_cast<std::uint8_t>(value * 255.0f);
}

// Retail's bump-color path truncates ((v+1)*0.5*255) to a signed dword and
// packs its low byte without saturation. x86 invalid conversion produces
// 0x80000000, whose low byte is zero; return the same deterministic byte for
// non-finite and signed-dword-out-of-range inputs.
inline std::uint8_t particle_retail_low_byte(float value) noexcept {
	const float encoded = (value + 1.0f) * 0.5f * 255.0f;
	const double widened = static_cast<double>(encoded);
	if (!std::isfinite(widened) ||
			widened < static_cast<double>(
					std::numeric_limits<std::int32_t>::min()) ||
			widened > static_cast<double>(
					std::numeric_limits<std::int32_t>::max())) {
		return 0;
	}
	const std::int32_t converted = static_cast<std::int32_t>(encoded);
	return static_cast<std::uint8_t>(
			static_cast<std::uint32_t>(converted) & 0xffu);
}

// Bump/Bumpadd DIFFUSE carries the encoded local light and particle alpha;
// SPECULAR retains the original modulated RGB. Keep the literal operation
// transpose(Rx(roll) * view), including its float multiplication grouping.
// [orig: CParticleEmitter_BuildBillboardQuads @0x5e6d60;
//  docs/particles/ptl-format-re.md]
inline std::uint32_t particle_lit_primary_color(float bump_scale, float roll,
		std::uint8_t alpha, const std::array<float, 9> &view_rows) noexcept {
	constexpr float light_component = 0.5773503f;
	const float seed = bump_scale * light_component;
	const float c = std::cos(roll), s = std::sin(roll);
	const float rotation[9] = {1.0f, 0.0f, 0.0f, 0.0f, c, -s, 0.0f, s, c};
	float combined[9];
	for (int row = 0; row < 3; ++row) {
		for (int col = 0; col < 3; ++col) {
			combined[row * 3 + col] = rotation[row * 3] * view_rows[col] +
					rotation[row * 3 + 1] * view_rows[3 + col] +
					rotation[row * 3 + 2] * view_rows[6 + col];
		}
	}
	std::uint8_t bytes[3];
	for (int col = 0; col < 3; ++col) {
		const float local = combined[col] * seed + combined[3 + col] * seed + combined[6 + col] * seed;
		bytes[col] = particle_retail_low_byte(local);
	}
	return (static_cast<std::uint32_t>(alpha) << 24) |
			(static_cast<std::uint32_t>(bytes[0]) << 16) |
			(static_cast<std::uint32_t>(bytes[1]) << 8) | bytes[2];
}

}  // namespace opennova::renderer