#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>

#include <runtime/devtools/environment_snapshot.h>

#include <cstdint>

namespace godot {

// The F3 Environment window's record (ADR 0042 d6) as a typed read for the
// GUT/probe side (Simulation.get_environment_snapshot): a value wrapper over
// the engine's devtools::EnvironmentSnapshot — the ENGINE join over the
// weather home in its display units (metres, percent, minutes; the colors
// packed 0x00RRGGBB). Null without a world; the witness map lives on the
// engine struct (retail Debug_DrawEnvironmentValues rows).
class EnvironmentSnapshot : public RefCounted {
	GDCLASS(EnvironmentSnapshot, RefCounted)

	opennova::devtools::EnvironmentSnapshot value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::devtools::EnvironmentSnapshot &p_value) { value_ = p_value; }

	bool get_valid() const { return value_.valid; }
	int64_t get_logic_tick() const { return static_cast<int64_t>(value_.logic_tick); }
	String get_env_name() const;
	String get_trn_name() const;
	int get_blink_flags() const { return static_cast<int>(value_.blink_flags); }
	int get_fog_type() const { return value_.fog_type; }
	int get_fog_dist_metres() const { return value_.fog_dist_metres; }
	int get_fog_target_metres() const { return value_.fog_target_metres; }
	int get_color_fade_seconds() const { return value_.color_fade_seconds; }
	int get_sun_fade_pct() const { return value_.sun_fade_pct; }
	bool get_night() const { return value_.night; }
	int get_fog_rgb() const { return static_cast<int>(value_.fog_rgb); }
	int get_sky_rgb() const { return static_cast<int>(value_.sky_rgb); }
	int get_sun_rgb() const { return static_cast<int>(value_.sun_rgb); }
	int get_lightning_rgb() const { return static_cast<int>(value_.lightning_rgb); }
	int get_fov_degrees() const { return value_.fov_degrees; }
	int get_sky_height_metres() const { return value_.sky_height_metres; }
	int get_sky_speed() const { return value_.sky_speed; }
	int get_sky_speed_target() const { return value_.sky_speed_target; }
	int get_rain_pct() const { return value_.rain_pct; }
	int get_rain_target_pct() const { return value_.rain_target_pct; }
	int get_overcast_pct() const { return value_.overcast_pct; }
	int get_overcast_target_pct() const { return value_.overcast_target_pct; }
	int get_minute_of_day() const { return value_.minute_of_day; }
	int get_quake_ticks() const { return value_.quake_ticks; }
	int get_precipitation_kind() const { return value_.precipitation_kind; }
	int get_wind_scale() const { return value_.wind_scale; }
	int get_lightning_timer_a() const { return value_.lightning_timer_a; }
	int get_lightning_timer_b() const { return value_.lightning_timer_b; }
	int get_lightning_level() const { return value_.lightning_level; }
	bool get_authority() const { return value_.authority; }
};

} // namespace godot
