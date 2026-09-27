#include "env/glare_occlusion.h"

using namespace godot;

int GlareOcclusion::get_brightness() const {
	return state.brightness;
}

int GlareOcclusion::get_window() const {
	return static_cast<int>(state.window);
}
