#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <env/env_weather.h>

namespace godot {

// Integer-faithful weather color smoother: the engine's 12.20 fixed-point
// per-channel eighth-step with rate clamps and +0x80000 rounding on repack
// [orig: interpolate_weather_color @ 0x57d9e0]. One instance per smoothed
// color channel (NovaWeather holds four). See docs/env/env-tod-re.md.
class NovaColorSmoother : public RefCounted {
	GDCLASS(NovaColorSmoother, RefCounted)

private:
	opennova::env::ColorChannelState state;
	Color current = Color(0, 0, 0);

protected:
	static void _bind_methods();

public:
	void snap(const Color &p_color);
	// Steps toward the target and returns the new current color.
	// max_step is in byte units per tick (255 disables the clamp in practice).
	Color step(const Color &p_target, float p_max_step = 255.0f);
	Color get_current() const;
};

} // namespace godot
