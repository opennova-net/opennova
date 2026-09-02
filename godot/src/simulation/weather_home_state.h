#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <cstdint>

// The probe/test view of the weather home in native units
// (Simulation.get_weather_state; engine: runtime/world/weather_state.h
// carries the cites): the generation counters, the fog / cloud / rain /
// overcast / sun-dim / sky-height q16 words, the time-of-day clock, the
// precipitation and lightning facts and the night verdict. Null without a
// weather home.
#define WEATHER_HOME_STATE_FIELDS(X)     \
	X(bool, valid)                       \
	X(int64_t, generation)               \
	X(int64_t, command_generation)       \
	X(int64_t, fog_target_q16)           \
	X(int64_t, fog_current_q16)          \
	X(int64_t, fog_accel_clamp)          \
	X(int, fog_type)                     \
	X(int64_t, tod_fixed24)              \
	X(int64_t, tod_advance_per_tick)     \
	X(int64_t, quake_ticks)              \
	X(int64_t, cloud_scroll_rate_target) \
	X(int64_t, cloud_scroll_rate)        \
	X(int64_t, rain_pct_current_q16)     \
	X(int64_t, rain_pct_target_q16)      \
	X(int64_t, overcast_blend_q16)       \
	X(int64_t, overcast_target_q16)      \
	X(int64_t, sun_dim_pct_q16)          \
	X(int64_t, sky_height_q16)           \
	X(int64_t, precipitation_kind)       \
	X(int64_t, lightning_color)          \
	X(int64_t, color_fade_ticks)         \
	X(int64_t, wind_scale)               \
	X(int, lightning_timer_a)            \
	X(int, lightning_timer_b)            \
	X(int, lightning_level)              \
	X(bool, night)

namespace godot {

class WeatherHomeState : public RefCounted {
	GDCLASS(WeatherHomeState, RefCounted)

public:
#define WEATHER_HOME_ACCESSORS(m_type, m_name)                \
	m_type get_##m_name() const { return m_name##_; }        \
	void set_##m_name(m_type p_value) { m_name##_ = p_value; }
	WEATHER_HOME_STATE_FIELDS(WEATHER_HOME_ACCESSORS)
#undef WEATHER_HOME_ACCESSORS

protected:
	static void _bind_methods();

private:
#define WEATHER_HOME_MEMBER(m_type, m_name) m_type m_name##_{};
	WEATHER_HOME_STATE_FIELDS(WEATHER_HOME_MEMBER)
#undef WEATHER_HOME_MEMBER
};

} // namespace godot
