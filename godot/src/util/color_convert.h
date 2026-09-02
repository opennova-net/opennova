#pragma once

#include <godot_cpp/variant/color.hpp>

#include <formats/env/env.h>

#include <algorithm>
#include <cstdint>

namespace opennova {

// The packed-colour <-> godot::Color edges the bindings share. Packed words
// decode straight to float channels (byte / 255.0f); the encoder rounds
// (int(v * 255 + 0.5), clamped to a byte) -- the form the GUT vectors pin as
// the hex idiom. Format writers with their own arithmetic keep it:
// cbin_credits_resource.cpp's color_to_cbin truncates (CBIN parity), the
// particle renderer multiplies by 1/255 (its golden pins), and the byte
// extractions into image buffers are not conversions at all.

// 0xAARRGGBB -> Color.
inline godot::Color color_from_argb(uint32_t argb) {
	return godot::Color(static_cast<float>((argb >> 16) & 0xFFu) / 255.0f,
			static_cast<float>((argb >> 8) & 0xFFu) / 255.0f,
			static_cast<float>(argb & 0xFFu) / 255.0f,
			static_cast<float>((argb >> 24) & 0xFFu) / 255.0f);
}

// 0x00RRGGBB -> opaque Color.
inline godot::Color color_from_rgb24(uint32_t rgb) {
	return godot::Color(static_cast<float>((rgb >> 16) & 0xFFu) / 255.0f,
			static_cast<float>((rgb >> 8) & 0xFFu) / 255.0f,
			static_cast<float>(rgb & 0xFFu) / 255.0f);
}

inline uint32_t color_byte(float v) {
	return static_cast<uint32_t>(std::clamp(static_cast<int>(v * 255.0f + 0.5f), 0, 255));
}

// Color -> 0xAARRGGBB, every channel rounded.
inline uint32_t argb_from_color(const godot::Color &c) {
	return (color_byte(c.a) << 24) | (color_byte(c.r) << 16) | (color_byte(c.g) << 8) |
			color_byte(c.b);
}

// Color -> 0xFFRRGGBB: the alpha byte forced opaque (the scar rows).
inline uint32_t argb_from_color_opaque(const godot::Color &c) {
	return 0xFF000000u | (color_byte(c.r) << 16) | (color_byte(c.g) << 8) | color_byte(c.b);
}

// The env document's float triple <-> Color (alpha 1).
inline godot::Color color_from_env_rgb(const env::Rgb &rgb) {
	return godot::Color(rgb.r, rgb.g, rgb.b);
}

inline env::Rgb env_rgb_from_color(const godot::Color &c) {
	return env::Rgb{static_cast<float>(c.r), static_cast<float>(c.g), static_cast<float>(c.b)};
}

// 0xAARRGGBB -> the env float triple (alpha dropped).
inline env::Rgb env_rgb_from_argb(uint32_t argb) {
	return env_rgb_from_color(color_from_argb(argb));
}

} // namespace opennova
