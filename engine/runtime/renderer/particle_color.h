#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

namespace renderer {

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

} // namespace renderer
