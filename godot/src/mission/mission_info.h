#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/string.hpp>

#include <formats/mission/mission.h>

namespace godot {

// The .bms mission header as a record (mission::MissionInfo by value): the
// identity strings, the climate/weather/type words, the attrib flags and the
// game mode decoded from them, the clock (raw start_time, minutes per day), the
// audio words, the wind, the map zoom, and the raw environment override fields
// (the gated, unit-converted form is MissionData.get_environment_overrides).
// Field witnesses live on the engine struct (formats/mission/mission.h).
class MissionInfo : public RefCounted {
	GDCLASS(MissionInfo, RefCounted)

	opennova::mission::MissionInfo value_;
	int game_mode_ = 0;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::mission::MissionInfo &p_value, int p_game_mode);

	String get_mission_name() const;
	String get_designer() const;
	String get_briefing() const;
	String get_terrain() const;
	String get_environment() const;
	int get_climate() const { return value_.climate; }
	int get_weather() const { return value_.weather; }
	int get_mission_type() const { return value_.mission_type; }
	int get_attrib_flags() const { return value_.attrib_flags; }
	// The single mode bit of attrib_flags (MissionData.get_game_mode).
	int get_game_mode() const { return game_mode_; }
	// Raw packed u16 (NOT decoded HH:MM).
	int get_start_time() const { return value_.start_time; }
	int get_minutes_per_day() const { return value_.minutes_per_day; }
	int get_player_health() const { return value_.player_health; }
	int get_max_saves() const { return value_.max_saves; }
	int get_music() const { return value_.music; }
	int get_reverb() const { return value_.reverb; }
	int get_wind_speed() const { return value_.wind_speed; }
	int get_wind_direction() const { return value_.wind_direction; }
	float get_map_zoom() const { return value_.map_zoom; }
	// The raw override fields: water in file half-units, fog in world units,
	// colors as authored bytes / 255, murk as the authored byte.
	int get_water_override() const { return value_.water_override; }
	int get_fog_override() const { return value_.fog_override; }
	Color get_fog_color() const;
	Color get_water_color() const;
	int get_water_murk() const { return value_.water_murk; }
	// The attrib gates (bms::AttribFlags WaterOverrideEnable / FogDistance /
	// FogColor).
	bool get_has_water_override() const;
	bool get_has_fog_distance_override() const;
	bool get_has_fog_color_override() const;
};

} // namespace godot
