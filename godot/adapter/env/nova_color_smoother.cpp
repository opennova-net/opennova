#include "env/nova_color_smoother.h"

#include <algorithm>

using namespace godot;

namespace {

uint32_t color_to_packed(const Color &color) {
	const auto to_byte = [](float v) -> uint32_t {
		return static_cast<uint32_t>(std::clamp(static_cast<int>(v * 255.0f + 0.5f), 0, 255));
	};
	return to_byte(color.b) | (to_byte(color.g) << 8) | (to_byte(color.r) << 16) | (to_byte(color.a) << 24);
}

Color packed_to_color(uint32_t packed) {
	return Color(
			static_cast<float>((packed >> 16) & 0xFF) / 255.0f,
			static_cast<float>((packed >> 8) & 0xFF) / 255.0f,
			static_cast<float>(packed & 0xFF) / 255.0f,
			static_cast<float>((packed >> 24) & 0xFF) / 255.0f);
}

} // namespace

void NovaColorSmoother::_bind_methods() {
	ClassDB::bind_method(D_METHOD("snap", "color"), &NovaColorSmoother::snap);
	ClassDB::bind_method(D_METHOD("step", "target", "max_step"), &NovaColorSmoother::step, DEFVAL(255.0f));
	ClassDB::bind_method(D_METHOD("get_current"), &NovaColorSmoother::get_current);
}

void NovaColorSmoother::snap(const Color &p_color) {
	state.snap_to(color_to_packed(p_color));
	current = p_color;
}

Color NovaColorSmoother::step(const Color &p_target, float p_max_step) {
	const int max_step_fp = std::max(1, static_cast<int>(p_max_step * static_cast<float>(1 << 20)));
	const uint32_t packed = state.step(color_to_packed(p_target), max_step_fp);
	current = packed_to_color(packed);
	return current;
}

Color NovaColorSmoother::get_current() const {
	return current;
}
