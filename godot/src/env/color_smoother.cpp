#include "env/color_smoother.h"
#include "util/color_convert.h"

#include <algorithm>

using namespace godot;

namespace {

} // namespace

void ColorSmoother::_bind_methods() {
	ClassDB::bind_method(D_METHOD("snap", "color"), &ColorSmoother::snap);
	ClassDB::bind_method(D_METHOD("step", "target", "max_step"), &ColorSmoother::step, DEFVAL(255.0f));
	ClassDB::bind_method(D_METHOD("get_current"), &ColorSmoother::get_current);
}

void ColorSmoother::snap(const Color &p_color) {
	state.snap_to(opennova::argb_from_color(p_color));
	current = p_color;
}

Color ColorSmoother::step(const Color &p_target, float p_max_step) {
	const int max_step_fp = std::max(1, static_cast<int>(p_max_step * static_cast<float>(1 << 20)));
	const uint32_t packed = state.step(opennova::argb_from_color(p_target), max_step_fp);
	current = opennova::color_from_argb(packed);
	return current;
}

Color ColorSmoother::get_current() const {
	return current;
}
