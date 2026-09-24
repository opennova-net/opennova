#include "simulation/environment_snapshot.h"

#include "util/record_bind.h"
#include "util/string_convert.h"

using namespace godot;

String EnvironmentSnapshot::get_env_name() const { return opennova::to_gd(value_.env_name); }
String EnvironmentSnapshot::get_trn_name() const { return opennova::to_gd(value_.trn_name); }

void EnvironmentSnapshot::_bind_methods() {
	OPENNOVA_RECORD_READ_ONLY(EnvironmentSnapshot, Variant::BOOL, valid)
	OPENNOVA_RECORD_READ_ONLY(EnvironmentSnapshot, Variant::INT, logic_tick)
	OPENNOVA_RECORD_READ_ONLY(EnvironmentSnapshot, Variant::STRING, env_name)
	OPENNOVA_RECORD_READ_ONLY(EnvironmentSnapshot, Variant::STRING, trn_name)
	OPENNOVA_RECORD_READ_ONLY(EnvironmentSnapshot, Variant::INT, blink_flags)
	OPENNOVA_RECORD_READ_ONLY(EnvironmentSnapshot, Variant::INT, fog_type)
	OPENNOVA_RECORD_READ_ONLY(EnvironmentSnapshot, Variant::INT, fog_dist_metres)
	OPENNOVA_RECORD_READ_ONLY(EnvironmentSnapshot, Variant::INT, fog_target_metres)
	OPENNOVA_RECORD_READ_ONLY(EnvironmentSnapshot, Variant::INT, color_fade_seconds)
	OPENNOVA_RECORD_READ_ONLY(EnvironmentSnapshot, Variant::INT, sun_fade_pct)
	OPENNOVA_RECORD_READ_ONLY(EnvironmentSnapshot, Variant::INT, sun_fade_target_pct)
	OPENNOVA_RECORD_READ_ONLY(EnvironmentSnapshot, Variant::BOOL, night)
	OPENNOVA_RECORD_READ_ONLY(EnvironmentSnapshot, Variant::INT, fog_rgb)
	OPENNOVA_RECORD_READ_ONLY(EnvironmentSnapshot, Variant::INT, sky_rgb)
	OPENNOVA_RECORD_READ_ONLY(EnvironmentSnapshot, Variant::INT, sun_rgb)
	OPENNOVA_RECORD_READ_ONLY(EnvironmentSnapshot, Variant::INT, lightning_rgb)
	OPENNOVA_RECORD_READ_ONLY(EnvironmentSnapshot, Variant::INT, fov_degrees)
	OPENNOVA_RECORD_READ_ONLY(EnvironmentSnapshot, Variant::INT, sky_height_metres)
	OPENNOVA_RECORD_READ_ONLY(EnvironmentSnapshot, Variant::INT, sky_speed)
	OPENNOVA_RECORD_READ_ONLY(EnvironmentSnapshot, Variant::INT, sky_speed_target)
	OPENNOVA_RECORD_READ_ONLY(EnvironmentSnapshot, Variant::INT, rain_pct)
	OPENNOVA_RECORD_READ_ONLY(EnvironmentSnapshot, Variant::INT, rain_target_pct)
	OPENNOVA_RECORD_READ_ONLY(EnvironmentSnapshot, Variant::INT, overcast_pct)
	OPENNOVA_RECORD_READ_ONLY(EnvironmentSnapshot, Variant::INT, overcast_target_pct)
	OPENNOVA_RECORD_READ_ONLY(EnvironmentSnapshot, Variant::INT, minute_of_day)
	OPENNOVA_RECORD_READ_ONLY(EnvironmentSnapshot, Variant::INT, quake_ticks)
	OPENNOVA_RECORD_READ_ONLY(EnvironmentSnapshot, Variant::INT, precipitation_kind)
	OPENNOVA_RECORD_READ_ONLY(EnvironmentSnapshot, Variant::INT, wind_scale)
	OPENNOVA_RECORD_READ_ONLY(EnvironmentSnapshot, Variant::INT, lightning_timer_a)
	OPENNOVA_RECORD_READ_ONLY(EnvironmentSnapshot, Variant::INT, lightning_timer_b)
	OPENNOVA_RECORD_READ_ONLY(EnvironmentSnapshot, Variant::INT, lightning_level)
	OPENNOVA_RECORD_READ_ONLY(EnvironmentSnapshot, Variant::BOOL, authority)
}
